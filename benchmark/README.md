# Hical vs Gin vs Actix-web 压测环境

## 快速开始

```bash
cd benchmark

# 1. 构建并启动所有服务（首次约 5-10 分钟）
# 同时构建 4 个容器资源不够，分步构建
# 先单独构建 wrk（最轻量）
docker compose build wrk
docker compose up -d wrk

# 再启动其余服务
docker compose up -d --build

# 2. 验证服务正常
curl http://localhost:8080/api/status   # Hical
curl http://localhost:8081/api/status   # Gin
curl http://localhost:8082/api/status   # Actix-web

# 3. 运行压测脚本（结果自动写入 benchmark/results.md）
docker compose exec wrk bash /bench/run_bench.sh

# 4. 采集补充数据（内存、二进制大小、镜像大小、代码行数 → stats.md）
bash collect_stats.sh

# 5. 清理
docker compose down
```

> **说明**: wrk 容器自建镜像（基于 Alpine），预装 wrk + bash + curl。
> `run_bench.sh` 通过 volume 挂载到容器 `/bench/run_bench.sh`，
> 容器内通过 Docker 网络名（hical / gin / actix）访问各服务，无需指定 HOST 环境变量。

### 宿主机直接运行（需要本地安装 wrk）

```bash
HICAL_HOST=localhost:8080 GIN_HOST=localhost:8081 ACTIX_HOST=localhost:8082 bash run_bench.sh
```

## 环境说明

| 服务  | 端口 | 框架         | 语言                               |
| ----- | ---- | ------------ | ---------------------------------- |
| hical | 8080 | Hical v2.5.0 | C++20 (Ubuntu 24.04 GCC + Conan 2) |
| gin   | 8081 | Gin v1.10    | Go 1.22                            |
| actix | 8082 | Actix-web 4  | Rust 1.78                          |
| wrk   | —    | wrk 压测工具 | Alpine 3.20                        |

每个服务容器限制 4 CPU + 512MB 内存，确保公平对比。

## 压测场景

| 场景        | 端点            | 描述                                     |
| ----------- | --------------- | ---------------------------------------- |
| Hello World | GET /           | 纯文本响应，测试框架底层开销             |
| JSON 响应   | GET /api/status | JSON 序列化，测试 JSON 库性能            |
| JSON Echo   | POST /api/echo  | JSON 反序列化+序列化，测试完整 JSON 处理 |
| 路径参数    | GET /users/42   | 路由匹配+参数提取+JSON 响应              |

## 常见问题

### `service "wrk" is not running`

wrk 容器未启动。确认 `docker compose up -d --build` 已执行且无报错：

```bash
docker compose ps          # 确认所有容器 Running
docker compose logs wrk    # 查看 wrk 容器日志
```

### PowerShell 中 `<` 报错 "运算符是为将来使用而保留的"

PowerShell 不支持 `<` 输入重定向。本文档已改为 volume 挂载方式，直接执行
`docker compose exec wrk bash /bench/run_bench.sh` 即可，不依赖 shell 重定向。

## 结果

| 脚本               | 输出文件     | 内容                                     |
| ------------------ | ------------ | ---------------------------------------- |
| `run_bench.sh`     | `results.md` | QPS、延迟、吞吐量（wrk 容器内执行）      |
| `collect_stats.sh` | `stats.md`   | 内存占用、二进制大小、镜像大小、代码行数 |

两份数据可直接填入 `docs/blog/11-cpp-vs-go-rust-web.md` 和 `BENCHMARK_REPORT.md`。

### collect_stats.sh 采集项

| 采集项      | 数据来源                            | 说明                            |
| ----------- | ----------------------------------- | ------------------------------- |
| 空载内存    | `docker stats --no-stream`          | 服务启动后、无请求时采样        |
| 满载内存    | 压测进行中 `docker stats` 多次采样  | wrk 4t100c 30s，3 次采样        |
| 满载 CPU    | 同上                                | Windows Docker 可能偏低（已知） |
| 二进制大小  | `docker exec <容器> ls -lh /server` | 容器内可执行文件                |
| Docker 镜像 | `docker images`                     | 含基础镜像层                    |
| 代码行数    | `wc -l`                             | benchmark 目录下各框架源码      |

> **注意**：`collect_stats.sh` 在**宿主机** bash 中运行（不是容器内），需要 Docker CLI 可用。
> Windows 用户请在 Git Bash / MSYS2 / WSL 中执行。
> 注意：满载 CPU% 在 Windows Docker Desktop (Hyper-V) 上因统计刷新延迟可能显示为 0%，这是平台已知限制，不影响内存数据准确性。Linux 上运行无此问题。