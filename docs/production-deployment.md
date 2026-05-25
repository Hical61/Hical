# Hical 生产部署实践：从编译优化到容器化

> 框架开发完了，测试也通过了——然后呢？"本地跑得好好的"和"线上稳定运行"之间，隔着编译优化、进程管理、反向代理、监控告警、容器编排一整套工程实践。这篇文章把 Hical 从开发环境搬到生产环境的完整链路走一遍，每个环节都给出可直接复用的配置模板。

---

## 目录

- [Hical 生产部署实践：从编译优化到容器化](#hical-生产部署实践从编译优化到容器化)
  - [目录](#目录)
  - [一、编译优化：榨干最后一点性能](#一编译优化榨干最后一点性能)
    - [1.1 Release 基础参数](#11-release-基础参数)
    - [1.2 LTO（链接时优化）](#12-lto链接时优化)
    - [1.3 PGO（Profile-Guided Optimization）](#13-pgoprofile-guided-optimization)
    - [1.4 静态链接 vs 动态链接](#14-静态链接-vs-动态链接)
  - [二、进程管理：别让服务裸奔](#二进程管理别让服务裸奔)
    - [2.1 systemd 服务配置](#21-systemd-服务配置)
    - [2.2 信号处理与 Graceful Shutdown](#22-信号处理与-graceful-shutdown)
    - [2.3 多线程与多 acceptor（SO\_REUSEPORT）](#23-多线程与多-acceptorso_reuseport)
  - [三、反向代理：Nginx 挡在前面](#三反向代理nginx-挡在前面)
    - [3.1 HTTP 反向代理](#31-http-反向代理)
    - [3.2 WebSocket 代理](#32-websocket-代理)
    - [3.3 SSL 终止策略](#33-ssl-终止策略)
  - [四、监控与可观测性](#四监控与可观测性)
    - [4.1 Prometheus 指标暴露](#41-prometheus-指标暴露)
    - [4.2 日志接入 ELK / Loki](#42-日志接入-elk--loki)
    - [4.3 健康检查端点](#43-健康检查端点)
  - [五、容器化部署](#五容器化部署)
    - [5.1 多阶段 Dockerfile](#51-多阶段-dockerfile)
    - [5.2 docker-compose 完整示例](#52-docker-compose-完整示例)
    - [5.3 Kubernetes 部署参考](#53-kubernetes-部署参考)
  - [六、性能调优检查清单](#六性能调优检查清单)
    - [系统级](#系统级)
    - [Hical 应用级](#hical-应用级)
    - [PMR 内存池](#pmr-内存池)
    - [数据库连接池](#数据库连接池)
    - [日志系统](#日志系统)
    - [调优流程](#调优流程)

---

## 一、编译优化：榨干最后一点性能

### 1.1 Release 基础参数

开发阶段用 Debug 方便调试，上线必须切 Release。区别不只是 `-O2`，还有 assert 消除、NDEBUG 定义（Hical 的 `HICAL_LOG_TRACE` 宏在 NDEBUG 下编译期完全消除）：

```bash
# GCC / Clang
cmake -B build -DCMAKE_BUILD_TYPE=Release \
      -DHICAL_BUILD_TESTS=OFF \
      -DHICAL_BUILD_EXAMPLES=OFF

cmake --build build -j$(nproc)

# MSVC
cmake -B build -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
```

**关键编译标志对照**：

| 标志     | Debug  | Release                | 影响                       |
| -------- | ------ | ---------------------- | -------------------------- |
| 优化级别 | `-O0`  | `-O2` / `-O3`          | 基本执行速度               |
| NDEBUG   | 未定义 | 已定义                 | assert 消除 + TRACE 宏消除 |
| 调试符号 | `-g`   | 无（可加 `-g1`）       | 二进制大小                 |
| 帧指针   | 保留   | `-fomit-frame-pointer` | 寄存器可用量               |

> **生产建议**：即使是 Release，也可以加 `-g1`（仅行号）保留最小调试信息，方便 core dump 分析。二进制增大约 10-15%，但出问题时能救命。

### 1.2 LTO（链接时优化）

LTO 让编译器在链接阶段看到所有编译单元，从而做跨文件内联、死代码消除。对 Hical 这种大量模板 + 内联函数的框架效果显著：

```bash
# GCC — ThinLTO 速度更快，Fat LTO 优化更激进
cmake -B build -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON

# Clang — 推荐 ThinLTO（并行链接）
cmake -B build -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_CXX_FLAGS="-flto=thin" \
      -DCMAKE_EXE_LINKER_FLAGS="-flto=thin -fuse-ld=lld"

# MSVC — /GL + /LTCG
cmake -B build -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_CXX_FLAGS="/GL" \
      -DCMAKE_EXE_LINKER_FLAGS="/LTCG"
```

**实测效果**（Hical v2.6.0 bench_server，wrk 10s / 4 线程 / 100 连接）：

| 配置            | QPS   | 相对提升 |
| --------------- | ----- | -------- |
| `-O2` 无 LTO    | ~159k | 基线     |
| `-O2` + ThinLTO | ~175k | +10%     |
| `-O3` + Fat LTO | ~183k | +15%     |

> **注意**：LTO 会显著增加链接时间（2-5x），CI 中建议只在 Release 标签构建时开启。

### 1.3 PGO（Profile-Guided Optimization）

PGO 分三步：先用 Instrumented 版本跑真实流量，收集热路径信息，再基于这些信息重编译。对 Hical 的路由查找和中间件链执行效果明显：

```bash
# 第一步：构建 instrumented 版本
cmake -B build-pgo1 -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_CXX_FLAGS="-fprofile-generate=/tmp/pgo-data"
cmake --build build-pgo1 -j$(nproc)

# 第二步：用真实流量跑 profile（至少跑 30 秒覆盖主要路径）
./build-pgo1/your_server &
wrk -t4 -c100 -d30s http://127.0.0.1:8080/api/status
wrk -t4 -c100 -d30s -s post.lua http://127.0.0.1:8080/api/echo
kill %1

# 第三步：基于 profile 重新编译
cmake -B build-pgo2 -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_CXX_FLAGS="-fprofile-use=/tmp/pgo-data -fprofile-correction"
cmake --build build-pgo2 -j$(nproc)
```

**PGO 带来的典型提升**：5-15%，主要来自分支预测优化和热函数内联。

### 1.4 静态链接 vs 动态链接

| 方面       | 静态链接             | 动态链接            |
| ---------- | -------------------- | ------------------- |
| 部署复杂度 | 单二进制，拷贝即部署 | 需要安装运行时库    |
| 二进制大小 | 较大（~30-50MB）     | 较小（~5-10MB）     |
| 启动速度   | 更快（无动态加载）   | 稍慢                |
| 安全更新   | 需重编译             | 只更新 .so          |
| 容器场景   | 极佳（Alpine 友好）  | 需要 glibc 基础镜像 |

**推荐策略**：

- **容器部署** → 静态链接。单二进制 + scratch/distroless 基础镜像，体积最小，攻击面最小。
- **裸机/VM 部署** → 动态链接。方便 OpenSSL 安全补丁热更新。

```bash
# 全静态链接（GCC + musl，需要 Alpine 或 musl-cross 工具链）
cmake -B build -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_EXE_LINKER_FLAGS="-static" \
      -DBUILD_SHARED_LIBS=OFF

# Boost 静态链接（默认行为，Hical vcpkg/Conan 配置均输出静态库）
cmake -B build -DCMAKE_BUILD_TYPE=Release \
      -DBoost_USE_STATIC_LIBS=ON
```

---

## 二、进程管理：别让服务裸奔

### 2.1 systemd 服务配置

生产环境不要 `./server &` 然后 `nohup`。用 systemd 管理进程，获得自动重启、日志管理、资源限制：

```ini
# /etc/systemd/system/hical-app.service
[Unit]
Description=Hical Web Application
After=network.target mysql.service
Wants=mysql.service

[Service]
Type=simple
User=hical
Group=hical
WorkingDirectory=/opt/hical
ExecStart=/opt/hical/server 8080
ExecReload=/bin/kill -HUP $MAINPID

# 优雅关机：先 SIGTERM，等 35 秒（比 Hical 默认 30 秒 shutdown timeout 多 5 秒）
KillSignal=SIGTERM
TimeoutStopSec=35

# 自动重启（非正常退出时）
Restart=on-failure
RestartSec=3

# 资源限制
LimitNOFILE=65536
LimitCORE=infinity
MemoryMax=2G

# 安全加固
NoNewPrivileges=true
ProtectSystem=strict
ProtectHome=true
ReadWritePaths=/opt/hical/logs /opt/hical/data

# 环境变量
Environment=MYSQL_HOST=127.0.0.1
Environment=MYSQL_PORT=3306
EnvironmentFile=-/opt/hical/.env

[Install]
WantedBy=multi-user.target
```

```bash
# 启用并启动
sudo systemctl daemon-reload
sudo systemctl enable hical-app
sudo systemctl start hical-app

# 查看状态
sudo systemctl status hical-app
sudo journalctl -u hical-app -f    # 实时日志
```

**关键参数说明**：

| 参数                 | 值                 | 为什么                                               |
| -------------------- | ------------------ | ---------------------------------------------------- |
| `LimitNOFILE=65536`  | 文件描述符上限     | 默认 1024 在高并发下不够用，每个连接至少消耗 1 个 fd |
| `TimeoutStopSec=35`  | 停止等待时间       | 比 Hical `setShutdownTimeout(30)` 多 5 秒余量        |
| `Restart=on-failure` | 异常重启           | OOM/崩溃自动拉起，正常 exit 0 不重启                 |
| `LimitCORE=infinity` | core dump 不限大小 | 配合 `-g1` 编译，崩溃时可分析                        |

### 2.2 信号处理与 Graceful Shutdown

Hical 内置了信号处理，不需要额外编码：

```cpp
// HttpServer 内部已注册：
// SIGINT  (Ctrl+C) → gracefulStop()
// SIGTERM (kill)   → gracefulStop()
```

`gracefulStop()` 的行为：

1. 设置 `draining_` 标志，拒绝新连接
2. 所有响应自动添加 `Connection: close` 头
3. 等待活跃请求处理完毕
4. 超时后（默认 30 秒）强制关闭

如果需要自定义关机超时：

```cpp
HttpServer server(8080, 4);
server.setShutdownTimeout(45.0);  // 45 秒后强制关闭
server.start();
```

### 2.3 多线程与多 acceptor（SO_REUSEPORT）

**v2.6.0 起，Hical 内置了 SO_REUSEPORT 多 acceptor 架构**：每个 worker loop 拥有独立的 acceptor，accept 与 I/O 在同一线程完成，零跨线程调度。这意味着只要设置好 `ioThreads`，框架会自动利用多核优势，不需要额外的多进程部署：

```cpp
// 推荐：直接设置 ioThreads = CPU 核数
// 框架自动在 Linux/macOS 上启用 SO_REUSEPORT 多 acceptor
// Windows 自动回退为单 acceptor（多 worker loop 仍然有效）
HttpServer server(8080, std::thread::hardware_concurrency());
server.start();
```

> **v2.6.0 之前 vs 之后**：
>
> | 版本     | accept 模型                       | 跨线程调度       |
> | -------- | --------------------------------- | ---------------- |
> | < v2.6.0 | 单 acceptor + round-robin 分发    | 每次 accept 一次 |
> | v2.6.0+  | 每个 worker loop 独立 acceptor    | 零               |
>
> 这是 QPS 从 27K 提升到 159K 的主要贡献因素之一。

对于极端场景（需要进程级故障隔离），仍可用多进程模式：

```bash
# 多进程 + SO_REUSEPORT（极端场景，通常不需要）
for i in $(seq 1 $(nproc)); do
    /opt/hical/server 8080 &
done
```

> **多线程 vs 多进程怎么选？**
>
> | 维度 | 多线程 | 多进程 |
> |------|--------|--------|
> | 内存占用 | 低（共享堆） | 高（独立地址空间） |
> | 编程复杂度 | 注意共享状态线程安全 | 进程间天然隔离 |
> | 故障隔离 | 一个线程崩全进程挂 | 单进程崩不影响其他 |
> | 适用场景 | 大多数 Web API 服务 | 需要进程级隔离（如插件系统） |
>
> **结论**：v2.6.0 的多 acceptor 架构下，99% 的场景用多线程就够了。

---

## 三、反向代理：Nginx 挡在前面

即使 Hical 可以直接对外服务，生产环境仍然建议在前面放一层 Nginx：

- **SSL 终止**：Let's Encrypt 证书管理、OCSP Stapling
- **静态文件**：Nginx 直出，不走应用进程
- **限流保护**：`limit_req` 防突发流量
- **多服务复用**：同一个 80/443 端口代理多个后端

### 3.1 HTTP 反向代理

```nginx
# /etc/nginx/conf.d/hical-app.conf

upstream hical_backend {
    # 多实例负载均衡
    server 127.0.0.1:8080;
    # server 127.0.0.1:8081;  # 如果有多进程
    
    keepalive 64;              # 长连接复用，减少 TCP 握手
}

server {
    listen 80;
    server_name api.example.com;
    return 301 https://$host$request_uri;
}

server {
    listen 443 ssl http2;
    server_name api.example.com;

    # SSL 证书（Let's Encrypt）
    ssl_certificate     /etc/letsencrypt/live/api.example.com/fullchain.pem;
    ssl_certificate_key /etc/letsencrypt/live/api.example.com/privkey.pem;
    ssl_protocols       TLSv1.2 TLSv1.3;
    ssl_ciphers         ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-RSA-AES128-GCM-SHA256;
    ssl_prefer_server_ciphers on;
    ssl_session_cache   shared:SSL:10m;
    ssl_session_timeout 1h;

    # 安全头
    add_header X-Frame-Options DENY always;
    add_header X-Content-Type-Options nosniff always;
    add_header Strict-Transport-Security "max-age=63072000" always;

    # 请求体限制（与 Hical setMaxBodySize 保持一致或略大）
    client_max_body_size 10m;

    # 限流（每 IP 每秒 50 请求，突发允许 100）
    limit_req_zone $binary_remote_addr zone=api:10m rate=50r/s;

    # API 路由
    location /api/ {
        limit_req zone=api burst=100 nodelay;
        
        proxy_pass http://hical_backend;
        proxy_http_version 1.1;
        proxy_set_header Host $host;
        proxy_set_header X-Real-IP $remote_addr;
        proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
        proxy_set_header X-Forwarded-Proto $scheme;
        proxy_set_header Connection "";    # 启用 keepalive
        
        # 超时配置
        proxy_connect_timeout 5s;
        proxy_send_timeout 30s;
        proxy_read_timeout 30s;
    }

    # 静态文件直接由 Nginx 处理
    location /static/ {
        root /opt/hical/public;
        expires 30d;
        add_header Cache-Control "public, immutable";
    }
}
```

### 3.2 WebSocket 代理

Hical 支持 WebSocket，Nginx 代理 WebSocket 需要额外的 Upgrade 头处理：

```nginx
# 在 server 块中添加
location /ws/ {
    proxy_pass http://hical_backend;
    proxy_http_version 1.1;
    
    # WebSocket 升级必需
    proxy_set_header Upgrade $http_upgrade;
    proxy_set_header Connection "upgrade";
    
    proxy_set_header Host $host;
    proxy_set_header X-Real-IP $remote_addr;
    
    # WebSocket 长连接，超时要设长一些
    proxy_read_timeout 3600s;
    proxy_send_timeout 3600s;
}
```

### 3.3 SSL 终止策略

两种常见方案：

| 方案           | Nginx → Hical        | 适用场景             |
| -------------- | -------------------- | -------------------- |
| Nginx 终止 SSL | HTTPS → HTTP（明文） | 大多数场景，简单高效 |
| 全链路加密     | HTTPS → HTTPS        | 零信任网络、合规要求 |

**方案一（推荐）**：Nginx 做 SSL 终止，后端走明文：

```cpp
// Hical 不需要 enableSsl()，直接监听明文
HttpServer server(8080, 4);
server.start();
```

**方案二**：全链路加密，Hical 也需要 SSL：

```cpp
HttpServer server(8080, 4);
server.enableSsl("/path/to/cert.pem", "/path/to/key.pem");
server.start();
```

```nginx
# Nginx 侧配置 proxy_pass 为 https
location /api/ {
    proxy_pass https://hical_backend;
    proxy_ssl_verify off;            # 内网自签证书可关闭验证
    proxy_ssl_server_name on;
}
```

---

## 四、监控与可观测性

### 4.1 Prometheus 指标暴露

Hical 本身没有内置 Prometheus exporter，但可以用中间件轻松实现指标收集和暴露：

```cpp
#include "core/HttpServer.h"
#include <atomic>
#include <chrono>
#include <mutex>
#include <unordered_map>

using namespace hical;

// 简易指标收集器
struct Metrics
{
    std::atomic<uint64_t> totalRequests {0};
    std::atomic<uint64_t> totalErrors {0};       // 5xx
    std::atomic<uint64_t> activeConnections {0};
    
    // 按路径统计（需要锁）
    struct PathStats
    {
        uint64_t count = 0;
        double totalLatencyMs = 0;
    };
    std::mutex mtx;
    std::unordered_map<std::string, PathStats> pathStats;
    
    void record(const std::string& path, double latencyMs, int status)
    {
        totalRequests.fetch_add(1, std::memory_order_relaxed);
        if (status >= 500)
        {
            totalErrors.fetch_add(1, std::memory_order_relaxed);
        }
        std::lock_guard lock(mtx);
        auto& s = pathStats[path];
        s.count++;
        s.totalLatencyMs += latencyMs;
    }
    
    std::string toPrometheus() const
    {
        std::string out;
        out += "# HELP http_requests_total Total HTTP requests\n";
        out += "# TYPE http_requests_total counter\n";
        out += "http_requests_total " + std::to_string(totalRequests.load()) + "\n";
        out += "# HELP http_errors_total Total HTTP 5xx errors\n";
        out += "# TYPE http_errors_total counter\n";
        out += "http_errors_total " + std::to_string(totalErrors.load()) + "\n";
        out += "# HELP http_active_connections Current active connections\n";
        out += "# TYPE http_active_connections gauge\n";
        out += "http_active_connections " + std::to_string(activeConnections.load()) + "\n";
        return out;
    }
};

Metrics g_metrics;

int main()
{
    HttpServer server(8080, 4);
    
    // 指标收集中间件
    server.use(
        [](HttpRequest& req, MiddlewareNext next) -> Awaitable<HttpResponse>
        {
            g_metrics.activeConnections.fetch_add(1, std::memory_order_relaxed);
            auto start = std::chrono::steady_clock::now();
            
            auto res = co_await next(req);
            
            auto elapsed = std::chrono::steady_clock::now() - start;
            double ms = std::chrono::duration<double, std::milli>(elapsed).count();
            g_metrics.record(std::string(req.path()), ms, static_cast<int>(res.statusCode()));
            g_metrics.activeConnections.fetch_sub(1, std::memory_order_relaxed);
            
            co_return res;
        });
    
    // Prometheus 拉取端点
    server.router().get("/metrics",
                        [](const HttpRequest&) -> HttpResponse
                        {
                            auto body = g_metrics.toPrometheus();
                            auto res = HttpResponse::ok(std::move(body));
                            res.setHeader("Content-Type", "text/plain; version=0.0.4");
                            return res;
                        });
    
    server.start();
}
```

**Prometheus 配置**（`prometheus.yml`）：

```yaml
scrape_configs:
  - job_name: 'hical-app'
    scrape_interval: 15s
    static_configs:
      - targets: ['hical-server:8080']
    metrics_path: '/metrics'
```

### 4.2 日志接入 ELK / Loki

Hical 的日志系统支持 JSON 格式化输出，天然适配 ELK Stack 和 Grafana Loki。

**方案一：JSON Lines → Filebeat → Elasticsearch**

```cpp
#include "core/Log.h"
#include "core/LogFormatter.h"
#include "core/AsyncFileSink.h"

using namespace hical;

void setupProductionLogging()
{
    auto& logger = Logger::instance();
    logger.setLevel(LogLevel::hInfo);    // 生产环境不要 Debug/Trace
    
    // JSON 格式化器 — 输出 JSON Lines，一行一条日志
    auto jsonFormatter = std::make_shared<JsonFormatter>();
    
    // 异步文件 Sink — 双缓冲，不阻塞业务线程
    AsyncFileSink::Options opts;
    opts.file.basePath = "/var/log/hical/app.log";
    opts.file.maxFileSize = 100 * 1024 * 1024;  // 100MB 轮转
    opts.file.maxFiles = 10;                     // 保留 10 个归档
    opts.bufferSize = 4 * 1024 * 1024;           // 4MB 双缓冲
    
    auto sink = std::make_shared<AsyncFileSink>(opts);
    sink->setFormatter(jsonFormatter);
    
    logger.addSink(sink);
}
```

JSON 输出示例（单行，这里格式化展示）：

```json
{
    "timestamp": "2026-05-08T10:30:45.123Z",
    "level": "INFO",
    "threadId": 14235,
    "file": "HttpServer.cpp",
    "line": 145,
    "message": "Server started on port 8080",
    "traceId": "a1b2c3d4e5f6...",
    "fields": {
        "port": 8080,
        "threads": 4
    }
}
```

**Filebeat 配置**（`/etc/filebeat/filebeat.yml`）：

```yaml
filebeat.inputs:
  - type: log
    paths:
      - /var/log/hical/app*.log
    json.keys_under_root: true
    json.add_error_key: true

output.elasticsearch:
  hosts: ["elasticsearch:9200"]
  index: "hical-%{+yyyy.MM.dd}"
```

**方案二：stderr → Docker 日志驱动 → Loki**

容器化部署时更简单——日志直接输出到 stderr，由 Docker 日志驱动采集：

```cpp
void setupContainerLogging()
{
    auto& logger = Logger::instance();
    logger.setLevel(LogLevel::hInfo);
    
    // 容器里直接用 stderr，JSON 格式
    auto stderrSink = std::make_shared<StderrSink>();
    stderrSink->setFormatter(std::make_shared<JsonFormatter>());
    
    logger.addSink(stderrSink);
}
```

```yaml
# docker-compose.yml 中配置 Loki 日志驱动
services:
  hical:
    image: hical-app:latest
    logging:
      driver: loki
      options:
        loki-url: "http://loki:3100/loki/api/v1/push"
        loki-batch-size: "400"
        labels: "app=hical,env=prod"
```

### 4.3 健康检查端点

生产环境必须有健康检查，给 Nginx upstream、Kubernetes liveness probe 和负载均衡器用：

```cpp
// 简单的存活检查
server.router().get("/health",
                    [](const HttpRequest&) -> HttpResponse
                    {
                        return HttpResponse::json({{"status", "ok"}});
                    });

// 深度检查（含数据库连通性）
server.router().get("/health/ready",
                    [&pool](const HttpRequest& req) -> Awaitable<HttpResponse>
                    {
                        try
                        {
                            auto conn = co_await pool.acquire();
                            co_await conn->ping();
                            pool.release(std::move(conn));
                            co_return HttpResponse::json({
                                {"status", "ok"},
                                {"db", "connected"}
                            });
                        }
                        catch (...)
                        {
                            auto res = HttpResponse::json({
                                {"status", "degraded"},
                                {"db", "disconnected"}
                            });
                            res.setStatus(HttpStatusCode::hServiceUnavailable);
                            co_return res;
                        }
                    });
```

---

## 五、容器化部署

### 5.1 多阶段 Dockerfile

Hical 项目已经有一个用于压测的 [Dockerfile](../benchmark/hical/Dockerfile)，这里给出生产级的完整版本：

```dockerfile
# ============ 构建阶段 ============
FROM ubuntu:24.04 AS builder

RUN apt-get update && apt-get install -y --no-install-recommends \
    cmake make g++ python3 python3-pip pipx ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# Conan 2 包管理器
RUN pipx install conan && pipx ensurepath
ENV PATH="/root/.local/bin:${PATH}"
RUN conan profile detect

WORKDIR /src
COPY conanfile.py CMakeLists.txt ./
COPY src/ src/

# 分离依赖安装（利用 Docker 缓存层）
RUN conan install . --build=missing -s build_type=Release -s compiler.cppstd=20

# 复制应用代码（放在依赖安装之后，应用代码变化不会重新安装依赖）
COPY . .

# 编译
RUN cmake --preset conan-release \
    -DHICAL_BUILD_TESTS=OFF \
    -DHICAL_BUILD_EXAMPLES=OFF \
    -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON \
    && cmake --build --preset conan-release -j$(nproc)

# ============ 运行阶段 ============
FROM ubuntu:24.04

# 仅安装运行时依赖
RUN apt-get update && apt-get install -y --no-install-recommends \
    libssl3 ca-certificates \
    && rm -rf /var/lib/apt/lists/* \
    && groupadd -r hical && useradd -r -g hical hical

COPY --from=builder /src/build/Release/your_server /app/server

# 非 root 运行
USER hical
WORKDIR /app

EXPOSE 8080

# 健康检查
HEALTHCHECK --interval=10s --timeout=3s --retries=3 \
    CMD curl -f http://localhost:8080/health || exit 1

CMD ["/app/server"]
```

**构建优化要点**：

1. **依赖缓存分离**：先 COPY `conanfile.py` + `CMakeLists.txt`，再 COPY `src/`。业务代码改动不会触发依赖重新编译。
2. **最小化运行镜像**：运行阶段只装 `libssl3`，不带编译器和开发头文件。
3. **非 root 用户**：安全最佳实践。
4. **LTO 开启**：`CMAKE_INTERPROCEDURAL_OPTIMIZATION=ON` 在容器内编译，一次性付出链接时间。

### 5.2 docker-compose 完整示例

一个典型的 Hical + MySQL + Nginx 的生产级编排：

```yaml
# docker-compose.prod.yml
services:
  # ---- Hical 应用 ----
  hical:
    build:
      context: .
      dockerfile: Dockerfile
    restart: unless-stopped
    environment:
      - MYSQL_HOST=mysql
      - MYSQL_PORT=3306
      - MYSQL_USER=hical
      - MYSQL_PASSWORD_FILE=/run/secrets/db_password
      - MYSQL_DATABASE=hical_prod
    secrets:
      - db_password
    expose:
      - "8080"                    # 仅内部网络可访问
    deploy:
      resources:
        limits:
          cpus: "4"
          memory: 1G
        reservations:
          cpus: "1"
          memory: 256M
    healthcheck:
      test: ["CMD", "curl", "-f", "http://localhost:8080/health"]
      interval: 10s
      timeout: 3s
      retries: 3
    networks:
      - backend
    depends_on:
      mysql:
        condition: service_healthy

  # ---- MySQL ----
  mysql:
    image: mysql:8.0
    restart: unless-stopped
    environment:
      - MYSQL_ROOT_PASSWORD_FILE=/run/secrets/db_root_password
      - MYSQL_DATABASE=hical_prod
      - MYSQL_USER=hical
      - MYSQL_PASSWORD_FILE=/run/secrets/db_password
    secrets:
      - db_root_password
      - db_password
    volumes:
      - mysql_data:/var/lib/mysql
      - ./init.sql:/docker-entrypoint-initdb.d/init.sql:ro
    healthcheck:
      test: ["CMD", "mysqladmin", "ping", "-h", "localhost"]
      interval: 10s
      timeout: 5s
      retries: 5
    deploy:
      resources:
        limits:
          cpus: "2"
          memory: 1G
    networks:
      - backend

  # ---- Nginx 反向代理 ----
  nginx:
    image: nginx:alpine
    restart: unless-stopped
    ports:
      - "80:80"
      - "443:443"
    volumes:
      - ./nginx.conf:/etc/nginx/conf.d/default.conf:ro
      - ./certs:/etc/nginx/certs:ro
      - ./static:/usr/share/nginx/html/static:ro
    depends_on:
      - hical
    networks:
      - frontend
      - backend

secrets:
  db_password:
    file: ./secrets/db_password.txt
  db_root_password:
    file: ./secrets/db_root_password.txt

volumes:
  mysql_data:

networks:
  frontend:
    driver: bridge
  backend:
    driver: bridge
    internal: true              # 后端网络不可直接从外部访问
```

```bash
# 构建并启动
docker compose -f docker-compose.prod.yml up -d --build

# 查看日志
docker compose -f docker-compose.prod.yml logs -f hical

# 滚动更新（零停机，需要多副本）
docker compose -f docker-compose.prod.yml up -d --no-deps --build hical
```

### 5.3 Kubernetes 部署参考

对于 K8s 环境，给出 Deployment + Service + HPA 的基础模板：

```yaml
# hical-deployment.yaml
apiVersion: apps/v1
kind: Deployment
metadata:
  name: hical-app
  labels:
    app: hical
spec:
  replicas: 3
  selector:
    matchLabels:
      app: hical
  template:
    metadata:
      labels:
        app: hical
      annotations:
        prometheus.io/scrape: "true"
        prometheus.io/port: "8080"
        prometheus.io/path: "/metrics"
    spec:
      containers:
        - name: hical
          image: registry.example.com/hical-app:v2.6.0
          ports:
            - containerPort: 8080
          env:
            - name: MYSQL_HOST
              value: "mysql-service"
            - name: MYSQL_PASSWORD
              valueFrom:
                secretKeyRef:
                  name: hical-db-secret
                  key: password
          resources:
            requests:
              cpu: "500m"
              memory: "256Mi"
            limits:
              cpu: "2"
              memory: "1Gi"
          livenessProbe:
            httpGet:
              path: /health
              port: 8080
            initialDelaySeconds: 5
            periodSeconds: 10
          readinessProbe:
            httpGet:
              path: /health/ready
              port: 8080
            initialDelaySeconds: 10
            periodSeconds: 5
---
apiVersion: v1
kind: Service
metadata:
  name: hical-service
spec:
  selector:
    app: hical
  ports:
    - port: 80
      targetPort: 8080
  type: ClusterIP
---
apiVersion: autoscaling/v2
kind: HorizontalPodAutoscaler
metadata:
  name: hical-hpa
spec:
  scaleTargetRef:
    apiVersion: apps/v1
    kind: Deployment
    name: hical-app
  minReplicas: 2
  maxReplicas: 10
  metrics:
    - type: Resource
      resource:
        name: cpu
        target:
          type: Utilization
          averageUtilization: 70
```

---

## 六、性能调优检查清单

最后给一张生产环境的调优清单。这些参数没有"最优值"，但有**合理起点**和**调整方向**：

### 系统级

| 项目                          | 默认 | 推荐起点 | 调整依据                    |
| ----------------------------- | ---- | -------- | --------------------------- |
| `ulimit -n`（fd 上限）        | 1024 | 65536+   | `maxConnections` × 2 + 余量 |
| `net.core.somaxconn`          | 4096 | 65535    | 高并发短连接场景            |
| `net.ipv4.tcp_tw_reuse`       | 0    | 1        | 减少 TIME_WAIT 占用         |
| `net.core.netdev_max_backlog` | 1000 | 5000     | 高包率场景                  |
| `vm.swappiness`               | 60   | 10       | 尽量不用 swap               |
| RPS/RFS（收包亲和）           | 关闭 | 开启     | 配合绑核减少跨核 IPI        |

**RPS/RFS 配置**（v2.6.3 bench-entrypoint.sh 已自动处理，裸机部署需手动）：

```bash
# 启用 RPS：将收包 softirq 分散到所有 CPU
for rxq in /sys/class/net/eth0/queues/rx-*/rps_cpus; do
    echo "f" > "$rxq"
done

# 启用 RFS：收包处理对齐到消费该 flow 的 CPU
echo 32768 > /proc/sys/net/core/rps_sock_flow_entries
for rxq in /sys/class/net/eth0/queues/rx-*/rps_flow_cnt; do
    echo 32768 > "$rxq"
done
```

> **为什么要配 RPS/RFS？** Hical v2.6.3 的 worker 线程已经绑核了，如果收包 softirq 跑在别的核上，数据还得搬一趟。RFS 让内核把收包 softirq 调度到处理该连接的 CPU，配合绑核就是"从网卡到用户态全在一个核上跑"。

### Hical 应用级

| 参数           | 默认值 | 调整建议                                                  | API                    |
| -------------- | ------ | --------------------------------------------------------- | ---------------------- |
| IO 线程数      | 1      | CPU 核数（不超过 8）                                      | `HttpServer(port, N)`  |
| 最大连接数     | 10000  | 根据内存预算：每连接约 25KB（v2.6.0 atomic 超时）         | `setMaxConnections()`  |
| 空闲连接超时   | 60s    | 反向代理后面可设短些（30s）；纯压测可设 0 省掉 timer 协程 | `setIdleTimeout()`     |
| 最大请求体     | 1MB    | 文件上传场景调大（如 50MB）                               | `setMaxBodySize()`     |
| 最大请求头     | 8KB    | 含大 Cookie 时调大（如 16KB）                             | `setMaxHeaderSize()`   |
| 关机超时       | 30s    | 长请求场景调大（如 60s）                                  | `setShutdownTimeout()` |
| 内存池 GC 间隔 | 60s    | 高频请求可缩短（30s）                                     | `setGcInterval()`      |

### PMR 内存池

| 参数                          | 默认值 | 调整建议                                       |
| ----------------------------- | ------ | ---------------------------------------------- |
| `globalLargestPoolBlock`      | 1MB    | 有大 JSON 响应时调大                           |
| `threadLocalLargestPoolBlock` | 512KB  | 跟随单请求最大分配调整                         |
| `requestPoolInitialSize`      | 4KB    | 小请求多的 API 可降到 2KB，大请求多可调到 16KB |

### 数据库连接池

| 参数                  | 默认值 | 调整建议                            |
| --------------------- | ------ | ----------------------------------- |
| `minConnections`      | 2      | IO 线程数 × 2                       |
| `maxConnections`      | 16     | 压测确定，通常 50-200               |
| `idleTimeout`         | 300s   | 过长浪费 MySQL 连接数，过短频繁重连 |
| `acquireTimeout`      | 5s     | 高并发下可适当加长                  |
| `queryTimeout`        | 30s    | 复杂报表查询场景可调大              |
| `stmtCacheSize`       | 64     | 查询种类多时调大（如 256）          |
| `healthCheckInterval` | 30s    | 网络不稳定时缩短                    |

### 日志系统

| 参数         | 默认值 | 调整建议                              |
| ------------ | ------ | ------------------------------------- |
| 日志级别     | Info   | 生产环境 Info 或 Warn                 |
| 文件轮转大小 | 100MB  | 磁盘充裕可调大（500MB），减少轮转频率 |
| 归档文件数   | 10     | 按保留天数和磁盘容量算                |
| 异步缓冲大小 | 4MB    | 高吞吐量日志场景调到 8-16MB           |
| Flush 间隔   | 1s     | 对实时性要求高可降到 500ms            |

### 调优流程

```
1. 先用默认参数跑压测，记录基线 QPS / P99 延迟 / 内存
2. 逐个调整单一参数，对比压测结果
3. 优先调线程数 → 连接池 → 内存池（边际收益递减）
4. 用 perf top / flamegraph 定位 CPU 热点
5. 用 /metrics 端点观察运行时指标
6. 用 LogAdmin 端点运行时调整日志级别（无需重启）
   GET  /admin/log-level          # 查看当前级别
   PUT  /admin/log-level          # 动态调整
```

---

> **系列导航**：本文为生产部署指南。
> 推荐阅读：[日志系统指南](logging-guide.md) | [协程入门](coroutine-guide.md) | [OpenAPI 指南](openapi-guide.md)
