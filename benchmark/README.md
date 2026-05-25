# 框架压测环境

通过 Docker Compose profile 管理两套独立的压测集：

| Profile      | 框架                                                  | 场景数 | 报告文件                  |
| ------------ | ----------------------------------------------------- | ------ | ------------------------- |
| `cpp`        | Hical / Drogon / Crow / Oat++ / cpp-httplib / Cinatra | 12     | `CPP_BENCHMARK_REPORT.md` |
| `cross-lang` | Hical / Gin / Fiber / Actix-web                       | 4      | `BENCHMARK_REPORT.md`     |

## 快速开始：C++ 框架对比

```bash
cd benchmark

# 1. 内核调优（VM / Linux 宿主机上执行，容器启动前生效）
sudo sysctl -w net.ipv4.ip_local_port_range="1024 65535"
sudo sysctl -w net.core.somaxconn=65535
sudo sysctl -w net.ipv4.tcp_max_syn_backlog=65535
sudo sysctl -w net.ipv4.tcp_tw_reuse=1

# 2. 构建（首次约 15-30 分钟，Drogon 编译较慢）
docker compose --profile cpp build

# 3. 启动（必须在 sysctl 之后，容器创建时继承内核参数）
docker compose --profile cpp up -d

# 4. 验证服务正常
curl http://localhost:8080/           # Hical
curl http://localhost:8083/           # Drogon
curl http://localhost:8084/           # Crow
curl http://localhost:8085/           # Oat++
curl http://localhost:8086/           # cpp-httplib
curl http://localhost:8087/           # Cinatra

# 5. 运行压测（12 场景：基础 4 + 中间件 5 + 高并发 3）
docker compose --profile cpp exec wrk bash -c "BENCH_MODE=cpp bash /bench/run_bench.sh"

# 6. 采集补充数据（内存、二进制大小等）
BENCH_MODE=cpp bash collect_stats.sh

# 7. 清理
docker compose --profile cpp down
```

## 快速开始：跨语言对比

```bash
cd benchmark

# 1. 内核调优（如果已在同一次启动中执行过，可跳过）
sudo sysctl -w net.ipv4.ip_local_port_range="1024 65535"
sudo sysctl -w net.core.somaxconn=65535
sudo sysctl -w net.ipv4.tcp_max_syn_backlog=65535
sudo sysctl -w net.ipv4.tcp_tw_reuse=1

# 2. 构建并启动
docker compose --profile cross-lang up -d --build

# 3. 验证
curl http://localhost:8080/           # Hical
curl http://localhost:8081/           # Gin
curl http://localhost:8089/           # Fiber
curl http://localhost:8082/           # Actix-web

# 4. 运行压测（4 基础场景）
docker compose --profile cross-lang exec wrk bash -c "BENCH_MODE=cross-lang bash /bench/run_bench.sh"

# 5. 采集补充数据
BENCH_MODE=cross-lang bash collect_stats.sh

# 6. 清理
docker compose --profile cross-lang down
```

> **说明**: wrk 容器基于 Alpine，预装 wrk + bash + curl，以 `sleep infinity` 常驻运行，压测通过 `docker compose exec` 进入执行。
> `run_bench.sh` 和 `post_echo.lua` 通过 volume 只读挂载到容器 `/bench/`，
> 压测结果输出到 `benchmark/output/results.md`（通过目录挂载自动同步到宿主机）。
> 容器内通过 Docker 网络名访问各服务。Hical 同时属于两个 profile，始终参与测试。
> **profiling profile 与 cpp/cross-lang 互斥**（共用 8080 端口），不要同时启动。

### 宿主机直接运行（需要本地安装 wrk）

```bash
BENCH_MODE=cpp HICAL_HOST=localhost:8080 DROGON_HOST=localhost:8083 \
CROW_HOST=localhost:8084 OATPP_HOST=localhost:8085 \
CPPHTTPLIB_HOST=localhost:8086 CINATRA_HOST=localhost:8087 \
bash run_bench.sh
```

### 自定义压测参数

```bash
# 调整并发数和持续时间
docker compose --profile cpp exec wrk bash -c "BENCH_MODE=cpp CONNECTIONS=500 DURATION=60s bash /bench/run_bench.sh"
```

## 环境说明

| 服务       | 端口 | 框架              | 语言 / 依赖                          |
| ---------- | ---- | ----------------- | ------------------------------------ |
| hical      | 8080 | Hical latest      | C++20 (Ubuntu 24.04 GCC + Conan 2)   |
| drogon     | 8083 | Drogon v1.9.8     | C++20 (Ubuntu 24.04 GCC + Trantor)   |
| crow       | 8084 | Crow v1.2.0       | C++20 (Ubuntu 24.04 GCC + Asio)      |
| oatpp      | 8085 | Oat++ v1.3.0      | C++20 (Ubuntu 24.04 GCC, 零外部依赖) |
| cpphttplib | 8086 | cpp-httplib v0.18 | C++20 (Ubuntu 24.04 GCC, 单头文件)   |
| cinatra    | 8087 | Cinatra latest    | C++20 (Ubuntu 24.04 GCC, 协程框架)   |
| gin        | 8081 | Gin v1.10         | Go 1.24                              |
| fiber      | 8089 | Fiber v2          | Go 1.24 (fasthttp)                   |
| actix      | 8082 | Actix-web 4       | Rust latest stable                   |
| wrk        | —    | wrk 4.1.0         | Alpine 3.20                          |

每个服务容器限制 **4 CPU + 1024MB 内存**，fd 上限 65536，确保公平对比。

## 压测场景

### 基础场景（4 个）

| 场景        | 端点              | 描述                                     |
| ----------- | ----------------- | ---------------------------------------- |
| Hello World | `GET /`           | 纯文本响应，测试框架底层开销             |
| JSON 响应   | `GET /api/status` | JSON 序列化，测试 JSON 库性能            |
| JSON Echo   | `POST /api/echo`  | JSON 反序列化+序列化，测试完整 JSON 处理 |
| 路径参数    | `GET /users/42`   | 路由匹配+参数提取+JSON 响应              |

### 中间件链场景（5 个）

| 场景                         | 端点                      | 描述                     |
| ---------------------------- | ------------------------- | ------------------------ |
| 中间件 0 层                  | `GET /middleware/0`       | 无中间件基线             |
| 中间件 3 层（原生机制）      | `GET /middleware/3`       | 3 层空操作中间件         |
| 中间件 10 层（原生机制）     | `GET /middleware/10`      | 10 层空操作中间件        |
| 中间件 3 层（Hical SyncMW）  | `GET /sync-middleware/3`  | Hical 同步中间件快速路径 |
| 中间件 10 层（Hical SyncMW） | `GET /sync-middleware/10` | Hical 同步中间件快速路径 |

> **中间件实现差异**：Hical（RouteGroup 洋葱链）和 Drogon（HttpFilter）使用真实框架中间件机制；
> Crow 和 Oat++ 因编译时/全局中间件限制，使用 handler 内 `std::function` 调用链模拟等价开销。

### 高并发场景（3 个）

| 场景          | 端点    | 并发连接 | 描述                       |
| ------------- | ------- | -------- | -------------------------- |
| 高并发 100    | `GET /` | 100      | 默认并发（基线）           |
| 高并发 1,000  | `GET /` | 1,000    | 中等并发                   |
| 高并发 10,000 | `GET /` | 10,000   | 极限并发，观察错误率和 OOM |

## 目录结构

```
benchmark/
├── README.md                  # 本文件
├── BENCHMARK_REPORT.md        # 跨语言对比报告（Hical/Gin/Fiber/Actix-web）
├── CPP_BENCHMARK_REPORT.md    # C++ 框架对比报告（Hical/Drogon/Crow/Oat++/cpp-httplib/Cinatra）
├── output/                    # 压测结果输出目录（自动创建）
│   └── results.md             # 压测结果（自动生成）
├── stats.md                   # 统计数据（自动生成）
├── run_bench.sh               # 压测脚本（BENCH_MODE 驱动）
├── collect_stats.sh           # 统计采集脚本（BENCH_MODE 驱动）
├── docker-compose.yml         # 容器编排（profile: cpp / cross-lang）
├── hical/                     # Hical benchmark 源码
│   ├── Dockerfile
│   ├── CMakeLists.txt
│   └── main.cpp
├── drogon/                    # Drogon benchmark 源码
│   ├── Dockerfile
│   ├── CMakeLists.txt
│   └── main.cpp
├── crow/                      # Crow benchmark 源码
│   ├── Dockerfile
│   ├── CMakeLists.txt
│   └── main.cpp
├── oatpp/                     # Oat++ benchmark 源码
│   ├── Dockerfile
│   ├── CMakeLists.txt
│   └── main.cpp
├── cpphttplib/                # cpp-httplib benchmark 源码
│   ├── Dockerfile
│   ├── CMakeLists.txt
│   └── main.cpp
├── cinatra/                   # Cinatra benchmark 源码
│   ├── Dockerfile
│   ├── CMakeLists.txt
│   └── main.cpp
├── gin/                       # Gin benchmark 源码
│   ├── Dockerfile
│   ├── go.mod
│   └── main.go
├── fiber/                     # Fiber benchmark 源码（Go fasthttp 对照组）
│   ├── Dockerfile
│   ├── go.mod
│   └── main.go
├── actix/                     # Actix benchmark 源码
│   ├── Dockerfile
│   ├── Cargo.toml
│   └── src/main.rs
└── wrk/                       # wrk 压测工具容器
    ├── Dockerfile
    └── post_echo.lua          # POST 测试 Lua 脚本
```

## VM 宿主机内核调优

快速开始流程中已包含 `sysctl` 命令。这里补充说明：

- **为什么要调**：容器内 `ulimits.nofile: 65536` 只管 fd 上限，网络栈参数（端口范围、backlog、TIME_WAIT 复用）是内核级的，需要在 VM 宿主机上设置
- **为什么要先于容器启动**：Docker 容器创建时会从宿主机继承 net namespace 参数。已经运行的容器不会自动同步，需要 `docker compose down` 后重新 `up -d`
- **持久化**：`sysctl -w` 重启后失效。如需永久生效，写入 `/etc/sysctl.d/99-bench.conf` 后执行 `sudo sysctl --system`
- **Docker Desktop (macOS/Windows)**：这些参数需要在 Docker VM 内设置，而非宿主机。可以通过 `docker run --privileged` 临时容器执行，或修改 Docker Desktop 的 `sysctl` 配置。Linux / Linux VM 上直接设置即可

## 常见问题

### VirtualBox 环境下 Go 框架 QPS 异常低

Go 1.21+ 引入的 per-P timer 机制大量调用 `timer_create` / `timer_settime`，VirtualBox 对这些系统调用的虚拟化开销比 KVM/Hyper-V 高 5-10 倍（参见 [golang/go#65073](https://github.com/golang/go/issues/65073)）。C++ 和 Rust 基于 `epoll_wait` 超时参数实现定时器，不受影响。如果在 VirtualBox 环境中观察到 Gin/Fiber QPS 仅为 C++/Rust 的 1/5~1/10，这是已知的虚拟化问题，建议换用 KVM 或裸机环境验证。

### `service "wrk" is not running`

wrk 容器未启动。确认 `docker compose up -d` 已执行且无报错：

```bash
docker compose ps          # 确认所有容器 Running
docker compose logs wrk    # 查看 wrk 容器日志
```

### Drogon 构建很慢

Drogon 需要从源码编译整个框架（含 Trantor 网络库），首次构建约 10-15 分钟。后续重建会利用 Docker 层缓存，只有 `main.cpp` 变更时才重编译 benchmark server。

### 高并发 10K 测试报 Socket errors

这是预期行为。10,000 并发连接会挑战容器的 fd 限制和内存上限（1024MB），部分框架可能出现连接失败。这本身是有价值的对比数据——错误率和恢复能力也是框架质量的指标。

## 结果输出

| 脚本               | 输出文件            | 内容                                     |
| ------------------ | ------------------- | ---------------------------------------- |
| `run_bench.sh`     | `output/results.md` | QPS、延迟、吞吐量（场景数取决于模式）    |
| `collect_stats.sh` | `stats.md`          | 内存占用、二进制大小、镜像大小、代码行数 |

### collect_stats.sh 采集项

| 采集项      | 数据来源                            | 说明                       |
| ----------- | ----------------------------------- | -------------------------- |
| 空载内存    | `docker stats --no-stream`          | 服务启动后、无请求时采样   |
| 满载内存    | 压测进行中 `docker stats` 多次采样  | wrk 4t100c 30s，3 次采样   |
| 二进制大小  | `docker exec <容器> ls -lh /server` | 容器内可执行文件           |
| Docker 镜像 | `docker images`                     | 含基础镜像层               |
| 代码行数    | `wc -l`                             | benchmark 目录下各框架源码 |

> **注意**：`collect_stats.sh` 在**宿主机** bash 中运行（不是容器内），需要 Docker CLI 可用。
> Windows 用户请在 Git Bash / MSYS2 / WSL 中执行。
> 脚本虽然也采集满载 CPU%，但 `docker stats` 在 Docker Desktop (Hyper-V) 和 VirtualBox 环境下 CPU% 数据普遍不准确（显示为 0%），仅供参考，不纳入报告。
