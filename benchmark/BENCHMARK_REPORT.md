# Hical vs Gin vs Actix-web 压测报告

> 测试日期：2026-05-06
> 测试目的：为 `docs/blog/11-cpp-vs-go-rust-web.md` 提供真实性能数据
> 数据来源：三轮独立测试取平均值

---

## 1. 测试环境

| 项目         | 配置                                                  |
| ------------ | ----------------------------------------------------- |
| 宿主机 OS    | Windows 10 Enterprise LTSC 2021                       |
| CPU          | 16 核                                                 |
| 内存         | 32 GB                                                 |
| Docker       | Docker Desktop (Hyper-V 后端)                         |
| Docker 资源  | CPU 16 / Memory 8GB / Swap 1GB / Disk 1TB             |
| 容器资源限制 | 每容器 4 CPU + 512MB 内存                             |
| 压测工具     | wrk 4.1.0（独立 Alpine 容器，预装 wrk + bash + curl） |
| 压测参数     | `-t4 -c100 -d30s`（4 线程、100 并发连接、30 秒持续）  |
| 测试轮数     | 3 轮，取平均值                                        |

### 框架版本

| 框架      | 语言  | 版本    | 编译器/运行时             | 基础镜像                           |
| --------- | ----- | ------- | ------------------------- | ---------------------------------- |
| Hical     | C++20 | v2.5.0  | GCC (Ubuntu 24.04 默认)   | ubuntu:24.04                       |
| Gin       | Go    | v1.10.0 | Go 1.22                   | golang:1.22-alpine → alpine:3.19   |
| Actix-web | Rust  | 4.x     | rust:latest (1.87+)       | rust:latest → debian:bookworm-slim |

### 网络拓扑

```
┌──────────────────────────────────────────────────────────┐
│                Docker 内部网桥 (bench)                     │
│                                                           │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌─────────┐ │
│  │  hical   │  │   gin    │  │  actix   │  │   wrk   │ │
│  │  :8080   │  │  :8081   │  │  :8082   │  │ (Alpine)│ │
│  └──────────┘  └──────────┘  └──────────┘  └─────────┘ │
│                                                  ↑       │
│                                  wrk 通过网桥访问各服务   │
└──────────────────────────────────────────────────────────┘
```

> **说明**：wrk 容器自建镜像（基于 Alpine），预装 wrk + bash + curl。
> `run_bench.sh` 通过 volume 挂载到容器 `/bench/run_bench.sh`，
> 容器内通过 Docker 网络名（hical / gin / actix）访问各服务。
> 三个框架的网络条件完全一致，均走 Docker 内部网桥。

---

## 2. 测试场景

| 场景          | 端点              | 描述                           | 响应体大小 |
| ------------- | ----------------- | ------------------------------ | ---------- |
| Hello World   | `GET /`           | 纯文本响应，测试框架底层开销   | ~13 bytes  |
| JSON Response | `GET /api/status` | JSON 序列化，测试 JSON 库性能  | ~40 bytes  |
| JSON Echo     | `POST /api/echo`  | JSON 反序列化+序列化，完整链路 | ~50 bytes  |
| 路径参数      | `GET /users/42`   | 参数路由匹配+JSON 响应         | ~30 bytes  |

### 各框架等价实现

**Hello World 端点**：三个框架都返回纯文本 `Hello, World!`，无中间件、无 JSON 序列化。

**JSON 端点**：三个框架返回等价 JSON：
- Hical: `{"status":"running","framework":"hical"}` (40 bytes)
- Gin: `{"framework":"gin","status":"running"}` (38 bytes)
- Actix: `{"status":"running","framework":"actix-web"}` (44 bytes)

**JSON Echo 端点**：POST 请求体 → 反序列化 → 序列化 → 响应。

**路径参数端点**：`/users/{id}` 参数路由匹配 + JSON 响应。

---

## 3. 测试结果（三轮平均值）

### 3.1 Hello World (GET /)

| 指标             | Hical (C++)  | Gin (Go)   | Actix-web (Rust) |
| ---------------- | ------------ | ---------- | ---------------- |
| **Requests/sec** | **261,021**  | 171,460    | **586,482**      |
| Avg Latency      | **371.6 μs** | 5.31 ms    | 155.9 μs         |
| Stdev Latency    | **205.3 μs** | 10.19 ms   | 201.4 μs         |
| Max Latency      | 22.85 ms     | 51.48 ms   | 22.18 ms         |

### 3.2 JSON Response (GET /api/status)

| 指标             | Hical (C++)  | Gin (Go)   | Actix-web (Rust) |
| ---------------- | ------------ | ---------- | ---------------- |
| **Requests/sec** | **250,410**  | 146,765    | **535,111**      |
| Avg Latency      | **385.4 μs** | 6.00 ms    | 171.5 μs         |
| Stdev Latency    | **69.7 μs**  | 11.16 ms   | 66.4 μs          |
| Max Latency      | 12.46 ms     | 61.71 ms   | 3.07 ms          |

### 3.3 JSON Echo (POST /api/echo)

| 指标             | Hical (C++)  | Gin (Go)   | Actix-web (Rust) |
| ---------------- | ------------ | ---------- | ---------------- |
| **Requests/sec** | **253,954**  | 172,113    | **568,754**      |
| Avg Latency      | **380.0 μs** | 5.29 ms    | 158.9 μs         |
| Stdev Latency    | **84.9 μs**  | 10.15 ms   | 62.5 μs          |
| Max Latency      | 10.40 ms     | 52.32 ms   | 3.20 ms          |

> **注意**：JSON Echo 测试中所有框架均返回 Non-2xx 响应（wrk 默认不携带 POST Body）。此场景主要测试路由匹配 + 错误路径的吞吐量。

### 3.4 路径参数 (GET /users/42)

| 指标             | Hical (C++)  | Gin (Go)   | Actix-web (Rust) |
| ---------------- | ------------ | ---------- | ---------------- |
| **Requests/sec** | **246,024**  | 143,567    | **472,806**      |
| Avg Latency      | **393.0 μs** | 6.04 ms    | 198.8 μs         |
| Stdev Latency    | **73.7 μs**  | 11.18 ms   | 78.2 μs          |
| Max Latency      | 12.80 ms     | 61.30 ms   | 3.59 ms          |

---

## 4. 关键分析

### 4.1 QPS 排名：Actix > Hical > Gin

| 场景        | Actix / Hical 比值 | Hical / Gin 比值 |
| ----------- | ------------------ | ---------------- |
| Hello World | 2.25x              | 1.52x            |
| JSON        | 2.14x              | 1.71x            |
| JSON Echo   | 2.24x              | 1.48x            |
| 路径参数    | 1.92x              | 1.71x            |

- 三者网络条件一致（均走 Docker 网桥），QPS 差距反映的是框架本身性能
- Hical 稳定领先 Gin 约 **48–71%**（各场景 Hical/Gin 比值 1.48x–1.71x）

### 4.2 延迟分析

| 场景        | Hical Avg  | Gin Avg | Actix Avg |
| ----------- | ---------- | ------- | --------- |
| Hello World | 371.6 μs   | 5.31 ms | 155.9 μs  |
| JSON        | 385.4 μs   | 6.00 ms | 171.5 μs  |
| JSON Echo   | 380.0 μs   | 5.29 ms | 158.9 μs  |
| 路径参数    | 393.0 μs   | 6.04 ms | 198.8 μs  |

- **Actix 延迟最低**（155–199μs），所有场景均在亚毫秒级
- **Hical 延迟亚毫秒级**（372–393μs），稳定在 400μs 以内
- **Gin 延迟在毫秒级**（5.3–6.0ms），显著高于 C++/Rust

### 4.3 延迟稳定性

| 指标          | Hical       | Gin        | Actix       |
| ------------- | ----------- | ---------- | ----------- |
| Stdev (Hello) | 205.3 μs    | 10.19 ms   | 201.4 μs    |
| Stdev (JSON)  | 69.7 μs     | 11.16 ms   | 66.4 μs     |
| Stdev (Echo)  | 84.9 μs     | 10.15 ms   | 62.5 μs     |
| Stdev (路径)  | 73.7 μs     | 11.18 ms   | 78.2 μs     |
| Max (Hello)   | 22.85 ms    | 51.48 ms   | 22.18 ms    |
| Max (JSON)    | 12.46 ms    | 61.71 ms   | 3.07 ms     |
| Max (Echo)    | 10.40 ms    | 52.32 ms   | 3.20 ms     |
| Max (路径)    | 12.80 ms    | 61.30 ms   | 3.59 ms     |

关键发现：
- **Hical 和 Actix 的 Stdev 在同一量级**（62–205μs vs 62–201μs），延迟抖动极小
- **Gin 的 Stdev 高两个数量级**（10–11ms 级）
- **Gin 的 Max 延迟 51–62ms**，是 Hical（10–23ms）和 Actix（3–22ms）的数倍

### 4.4 各场景 QPS 下降分析

| 框架  | Hello QPS | JSON QPS | Echo QPS | 路径参数 QPS | JSON 下降 | 路径参数下降 |
| ----- | --------: | -------: | -------: | -----------: | --------: | -----------: |
| Hical | 261,021   | 250,410  | 253,954  | 246,024      | -4.1%     | -5.7%        |
| Gin   | 171,460   | 146,765  | 172,113  | 143,567      | -14.4%    | -16.3%       |
| Actix | 586,482   | 535,111  | 568,754  | 472,806      | -8.8%     | -19.4%       |

- **Hical QPS 下降最小**（4–6%）：JSON 序列化和参数路由匹配对 QPS 影响极小
- **Gin 下降 14–16%**：JSON 场景开销最大，可能与 Go `encoding/json` 反射机制有关（未 profiling 确认）
- **Actix 在路径参数场景下降 19.4%**：参数路由匹配场景 QPS 下降幅度三者中最大

### 4.5 三轮数据稳定性

| 场景        | 框架  | 第1轮   | 第2轮   | 第3轮   | 平均    | 最大波动 |
| ----------- | ----- | ------: | ------: | ------: | ------: | -------: |
| Hello World | Hical | 258,289 | 261,443 | 263,332 | 261,021 | ±1.0%    |
|             | Gin   | 168,891 | 171,901 | 173,587 | 171,460 | ±1.5%    |
|             | Actix | 573,395 | 590,399 | 595,653 | 586,482 | ±2.2%    |
| JSON        | Hical | 249,389 | 251,418 | 250,424 | 250,410 | ±0.4%    |
|             | Gin   | 145,811 | 146,967 | 147,516 | 146,765 | ±0.7%    |
|             | Actix | 527,782 | 530,301 | 547,249 | 535,111 | ±2.3%    |
| JSON Echo   | Hical | 252,593 | 255,253 | 254,015 | 253,954 | ±0.5%    |
|             | Gin   | 171,959 | 172,507 | 171,873 | 172,113 | ±0.3%    |
|             | Actix | 565,765 | 572,859 | 567,638 | 568,754 | ±0.7%    |
| 路径参数    | Hical | 244,059 | 248,630 | 245,384 | 246,024 | ±1.1%    |
|             | Gin   | 144,149 | 143,774 | 142,779 | 143,567 | ±0.5%    |
|             | Actix | 466,418 | 476,511 | 475,490 | 472,806 | ±1.4%    |

三轮测试 QPS 波动均在 ±2.3% 以内，数据高度稳定可信。

---

## 5. 结论

| 维度             | 第一名        | 说明                                             |
| ---------------- | ------------- | ------------------------------------------------ |
| **吞吐量 (QPS)** | Actix-web     | 各场景均领先，领先 Hical 约 1.9–2.3x             |
| **平均延迟**     | Actix-web     | 156–199μs；Hical 372–393μs；网络条件一致         |
| **延迟稳定性**   | Hical / Actix | 两者 Stdev 均在 μs 级，Gin 在 ms 级              |
| **尾延迟 (Max)** | Actix-web     | Max 3–22ms；Hical 10–23ms；Gin 51–62ms           |
| **JSON 效率**    | Hical         | QPS 下降仅 4.1%，三者中最小                      |
| **参数路由效率** | Hical         | QPS 下降仅 5.7%，三者中最小                      |

**一句话**：Actix 在吞吐量和延迟上全面领先，Hical 在 JSON 和参数路由场景 QPS 下降最小、延迟稳定性与 Actix 同级，Gin 在所有性能维度排第三。

---

## 6. 已知局限性

1. **JSON Echo 为错误路径**：压测脚本通过 heredoc 管道向 wrk 传递 Lua 脚本携带 POST Body，但实测中所有框架均返回 Non-2xx（Lua 脚本未正确生效），测试的是错误处理吞吐量而非正常 JSON Echo
2. **未测内存**：容器化环境不方便精确测量进程内存
3. **单机测试**：Docker 容器共享宿主机 CPU 缓存，可能与独立物理机结果有差异
4. **容器资源限制**：512MB 内存和 4 CPU 限制可能影响极限性能

---

## 7. 后续验证计划

### 7.1 正确的 POST JSON Echo 场景

```bash
# wrk Lua 脚本
cat > /tmp/post.lua << 'EOF'
wrk.method = "POST"
wrk.body   = '{"name":"Alice","age":30,"email":"alice@example.com"}'
wrk.headers["Content-Type"] = "application/json"
EOF

wrk -t4 -c100 -d30s -s /tmp/post.lua http://hical:8080/api/echo
wrk -t4 -c100 -d30s -s /tmp/post.lua http://gin:8081/api/echo
wrk -t4 -c100 -d30s -s /tmp/post.lua http://actix:8082/api/echo
```

### 7.2 内存占用测试

```bash
# 空载
docker stats --no-stream --format "table {{.Name}}\t{{.MemUsage}}\t{{.CPUPerc}}"

# 满载（压测进行时）
docker stats --no-stream --format "table {{.Name}}\t{{.MemUsage}}\t{{.CPUPerc}}"
```

---

## 8. 复现步骤

```powershell
cd D:\hical\Hical\benchmark

# 构建并启动所有服务（含 wrk 容器）
docker compose up -d --build

# 验证服务正常
curl http://localhost:8080/api/status   # Hical
curl http://localhost:8081/api/status   # Gin
curl http://localhost:8082/api/status   # Actix

# 运行自动化压测脚本（在 wrk 容器内执行）
docker compose exec wrk /bench/run_bench.sh

# 清理
docker compose down
```

---

## 9. QPS 汇总（三轮平均值）

| 场景                        |    Hical |      Gin | Actix-web |
| --------------------------- | -------: | -------: | --------: |
| Hello World (GET /)         | 261,021  | 171,460  |   586,482 |
| JSON 响应 (GET /api/status) | 250,410  | 146,765  |   535,111 |
| JSON Echo (POST /api/echo)  | 253,954  | 172,113  |   568,754 |
| 路径参数 (GET /users/42)    | 246,024  | 143,567  |   472,806 |

---

## 10. 文件清单

```
benchmark/
├── README.md                  ← 快速使用说明
├── docker-compose.yml         ← 四服务编排（hical/gin/actix/wrk）
├── run_bench.sh               ← 自动化压测脚本（volume 挂载到 wrk 容器）
├── results.md                 ← 原始压测输出（第三轮）
├── BENCHMARK_REPORT.md        ← 本文件（压测报告，三轮平均值）
├── hical/
│   ├── Dockerfile             ← Ubuntu 24.04 + GCC + Conan + Boost
│   └── main.cpp               ← 压测服务器源码
├── gin/
│   ├── Dockerfile             ← Go 1.22 Alpine
│   ├── go.mod
│   └── main.go
├── actix/
│   ├── Dockerfile             ← Rust latest + Debian
│   ├── Cargo.toml
│   └── src/main.rs
└── wrk/
    └── Dockerfile             ← Alpine + wrk + bash + curl
```
