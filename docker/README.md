# Docker

本目录包含 Hical 框架的所有 Docker 相关配置。

## 目录结构

```
docker/
├── test/                       # 多平台编译测试（复刻 CI）
│   ├── gcc14.Dockerfile
│   ├── clang20.Dockerfile
│   ├── clang20-tidy.Dockerfile
│   └── README.md
├── prod/                       # 生产级部署（Hical + MySQL + Nginx）
│   ├── Dockerfile
│   ├── docker-compose.yml
│   ├── nginx.conf
│   ├── init.sql
│   ├── prod_server.cpp
│   ├── .env.example
│   └── README.md
├── TFB/                        # TechEmpower Framework Benchmarks
│   ├── hical.dockerfile        # TFB 专用 Dockerfile（遵循 TFB 命名规范）
│   ├── benchmark_config.json   # TFB 配置文件
│   └── bench_main.cpp          # TFB 极简服务器（仅 /json + /plaintext）
├── bench.Dockerfile            # 一体化 benchmark（编译 + wrk 全场景测试）
├── bench_main.cpp              # 压测服务器入口
├── flamegraph-analysis-cn.md   # 火焰图分析（中文）
├── flamegraph-analysis.md      # 火焰图分析（英文）
└── README.md                   # 本文件
```

## 多平台测试

在 `docker/test/` 目录下执行：

```bash
cd docker/test
docker compose up --build --abort-on-container-exit
```

详见 [test/README.md](test/README.md)。

## 生产部署

```bash
cd docker/prod
docker compose up -d --build
```

详见 [prod/README.md](prod/README.md)。

---

## Benchmark（VM 环境）

### 前置条件

- Linux VM（推荐 Ubuntu 22.04/24.04），**至少 4 核 CPU + 4GB 内存**
- 已安装 Docker Engine（推荐 24.0+）
- 已将 Hical 源码传输到 VM 上

### 1. 传输源码到 VM

```bash
# 方式一：git clone
ssh user@<VM_IP>
git clone https://github.com/Hical61/Hical.git
cd Hical

# 方式二：从本地 rsync（适合未推送的改动）
rsync -avz --exclude=build --exclude=.git \
    /path/to/Hical/ user@<VM_IP>:~/Hical/
ssh user@<VM_IP>
cd ~/Hical
```

### 2. 构建镜像

> 以下所有命令均在**项目根目录**（`~/Hical`）下执行。

```bash
docker build -f docker/bench.Dockerfile -t hical-bench .
```

构建过程约 3-8 分钟（取决于网络和 CPU），包含：
- 安装 GCC 14 + Boost + wrk
- Release 模式编译 bench_server
- 内嵌 wrk lua 脚本和全场景测试脚本

> **VM 重启后无需重新构建**。镜像持久存储在 Docker image store 中，直接跳到第 3 步运行即可。
>
> 需要重新 `docker build` 的情况：
> - 修改了 Hical 源码（需要重新编译）
> - 修改了 `docker/bench_main.cpp` 或 `docker/bench.Dockerfile`
> - 执行了 `docker image prune` 清理了镜像

### 3. 运行 Benchmark

```bash
# 默认参数（4 线程, 30s/场景）
docker run --rm hical-bench

# 自定义参数
docker run --rm -e DURATION=60s -e THREADS=8 hical-bench

# 保存原始输出（末尾含 Markdown 汇总表，可直接复制到 benchmark-results.md）
docker run --rm hical-bench 2>&1 | tee bench-raw-output.txt
# 提取汇总表追加到 benchmark-results.md
sed -n '/^## Results Summary/,$ p' bench-raw-output.txt >> docker/benchmark-results.md
```

### 4. 测试场景（12 场景）

| #   | 场景             | 路径                  | 方法 | 并发  |
| --- | ---------------- | --------------------- | ---- | ----- |
| 1   | Hello World      | `/`                   | GET  | 100   |
| 2   | JSON 响应        | `/api/status`         | GET  | 100   |
| 3   | JSON Echo        | `/api/echo`           | POST | 100   |
| 4   | 路径参数         | `/users/42`           | GET  | 100   |
| 5   | 中间件 0 层      | `/middleware/0`       | GET  | 100   |
| 6   | 中间件 3 层      | `/middleware/3`       | GET  | 100   |
| 7   | 中间件 10 层     | `/middleware/10`      | GET  | 100   |
| 8   | 同步中间件 3 层  | `/sync-middleware/3`  | GET  | 100   |
| 9   | 同步中间件 10 层 | `/sync-middleware/10` | GET  | 100   |
| 10  | 高并发 100       | `/`                   | GET  | 100   |
| 11  | 高并发 1000      | `/`                   | GET  | 1000  |
| 12  | 高并发 10000     | `/`                   | GET  | 10000 |

### 5. 可调参数

| 环境变量   | 默认值 | 说明                              |
| ---------- | ------ | --------------------------------- |
| `DURATION` | `30s`  | 每个场景的 wrk 持续时间           |
| `THREADS`  | `4`    | wrk 线程数（建议 <= VM CPU 核数） |

> **THREADS 不要超过容器 CPU 核数**。分离模式下 wrk 容器限制为 4 核，设 `THREADS=8` 会导致线程互相竞争，QPS 反而降到 1/3。保持 `THREADS <= cpus` 即可。

### 6. 性能调优建议（VM 环境）

在运行 benchmark 前，对 VM 做以下调优可获得更稳定的结果：

```bash
# 提升最大文件描述符（高并发场景需要）
ulimit -n 65535

# 优化 TCP 连接回收（避免 TIME_WAIT 堆积）
sudo sysctl -w net.ipv4.tcp_tw_reuse=1
sudo sysctl -w net.ipv4.tcp_fin_timeout=15

# 增大 TCP backlog（c=10000 场景需要）
sudo sysctl -w net.core.somaxconn=65535
sudo sysctl -w net.ipv4.tcp_max_syn_backlog=65535

# 增大端口范围（高并发短连接需要）
sudo sysctl -w net.ipv4.ip_local_port_range="1024 65535"
```

对于 Docker 容器内的 benchmark（server 和 wrk 在同一容器），loopback 接口的性能受限于：
- **spin_lock 竞争**：loopback 接口的每个数据包都经过内核自旋锁
- **CPU 竞争**：server 和 wrk 共享同一组 CPU，互相竞争资源

如需更准确的结果，使用下方的**分离模式**。

---

## Benchmark 分离模式（推荐）

将 server 和 wrk 拆分到独立容器，消除 CPU 竞争和 loopback 瓶颈。

### 方式一：同一 VM、两个容器（Docker Compose）

server 和 wrk 各自独立容器（各 4 CPU / 512MB），通过 Docker bridge 网络通信。

**首次运行（构建 + 测试）**：

```bash
# 在项目根目录执行（--build 自动判断：有变化的镜像重建，没变的用缓存跳过）
DURATION=60s THREADS=4 docker compose -f docker/docker-compose.bench.yml up --build --abort-on-container-exit
```

**强制全部重建（两个镜像从头构建，再跑测试）**：

> **注意**：构建过程（编译 bench_server）会大量占用 CPU 缓存和内存带宽。如果构建完立刻跑测试，CPU 还未冷却，QPS 可能比正常低 20-30%。建议分两步执行，中间等待 1-2 分钟：

```bash
# 步骤一：构建
docker compose -f docker/docker-compose.bench.yml build --no-cache

# （等待 CPU 冷却，用 uptime 确认 load average 降到 0.0x）
uptime
# 看到 load average: 0.05, 0.10, ... 再继续

# 步骤二：跑测试
DURATION=60s THREADS=4 docker compose -f docker/docker-compose.bench.yml up --abort-on-container-exit
```

**后续运行（镜像已存在、源码未改动）**：

```bash
# 默认参数（4 线程, 30s/场景）
docker compose -f docker/docker-compose.bench.yml up --abort-on-container-exit

# 自定义参数
DURATION=60s THREADS=4 docker compose -f docker/docker-compose.bench.yml up --abort-on-container-exit
```

> 脚本跑完后会自动在容器内生成 `/output/benchmark-results.md`（完整 Markdown 报告，含汇总表 + 详细 wrk 输出 + 中间件对比表）。


此模式下 server 和 wrk 不再共享 CPU，但仍共享同一物理机的网络栈。

### 方式二：跨 VM（server 在 VM-A，wrk 在 VM-B）

最准确的测试方式，彻底消除 loopback 和 CPU 竞争。

**VM-A（server 端）**：

```bash
# 构建并启动 server（前台运行，观察日志）
docker build -f docker/bench-server.Dockerfile -t hical-bench-server .
docker run --rm -p 8080:8080 --cpus=4 --memory=512m hical-bench-server
```

**VM-B（wrk 端）**：

```bash
# 构建 wrk 镜像
docker build -f docker/bench-wrk.Dockerfile -t hical-bench-wrk .

# 运行压测（SERVER_HOST 指向 VM-A 的 IP，-v 映射报告输出目录）
docker run --rm \
    -e SERVER_HOST=<VM-A-IP>:8080 \
    -e DURATION=60s \
    -e THREADS=4 \
    -v $(pwd)/docker:/output \
    hical-bench-wrk
```

> 跑完后报告自动写入 `docker/benchmark-results.md`。

### 分离模式的文件结构

```
docker/
├── docker-compose.bench.yml # Compose 编排（同一 VM 双容器）
├── bench-server.Dockerfile  # Server 镜像（多阶段构建，最小运行时）
├── bench-wrk.Dockerfile     # wrk 镜像（含全场景脚本）
├── bench.Dockerfile         # 一体化模式（单容器，快速测试用）
└── bench_main.cpp           # 压测服务器入口
```

### 可调参数

| 环境变量      | 默认值              | 说明                                    |
| ------------- | ------------------- | --------------------------------------- |
| `SERVER_HOST` | `bench-server:8080` | server 地址（跨 VM 时改为 `<IP>:8080`） |
| `DURATION`    | `30s`               | 每场景 wrk 持续时间                     |
| `THREADS`     | `4`                 | wrk 线程数（建议 <= wrk 机器 CPU 核数） |

### 三种模式对比

| 模式         | 命令                                                   | CPU 竞争                 | 网络延迟      | 适用场景     |
| ------------ | ------------------------------------------------------ | ------------------------ | ------------- | ------------ |
| 一体化       | `docker run hical-bench`                               | 高（共享）               | loopback      | 快速冒烟测试 |
| 同 VM 双容器 | `docker compose -f docker/docker-compose.bench.yml up` | 中（隔离但共享物理 CPU） | Docker bridge | 日常基准测试 |
| 跨 VM        | 两台 VM 分别 `docker run`                              | 无                       | 真实网络      | 正式性能报告 |

### 7. 解读结果

wrk 输出关键指标：

```
Running 30s test @ http://127.0.0.1:8080/
  4 threads and 100 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency     0.63ms  148.95us   5.23ms   78.52%
    Req/Sec    39.87k     1.23k   42.15k    68.33%
  4772815 requests in 30.01s, 579.51MB read
Requests/sec: 159040.28          <-- QPS（核心指标）
Transfer/sec:     19.31MB
```

- **Requests/sec (QPS)**：核心性能指标，数值越高越好
- **Latency Avg**：平均延迟，越低越好
- **Latency Max**：最大延迟，反映尾部延迟
- **Socket errors**：如出现说明并发过高或系统参数需调优
- **Non-2xx responses**：如出现说明路由配置有误

### 8. 关闭容器与清理

#### 一体化模式

`docker run --rm` 的 `--rm` 参数会在容器退出后自动销毁，无需手动操作。

```bash
# 仅需清理镜像（可选）
docker image rm hical-bench
```

#### 分离模式 — 同 VM 双容器（Docker Compose）

`--abort-on-container-exit` 会在 wrk 跑完后让两个容器同时退出，但容器实例和网络仍然残留，需要手动清理：

```bash
# 停止并移除容器 + 网络
docker compose -f docker/docker-compose.bench.yml down

# 如果还想删除构建的镜像
docker compose -f docker/docker-compose.bench.yml down --rmi all
```

#### 分离模式 — 跨 VM

两端都使用了 `--rm`，容器退出后自动销毁。仅需清理镜像：

```bash
# VM-A（server 端）
docker image rm hical-bench-server

# VM-B（wrk 端）
docker image rm hical-bench-wrk
```

#### 通用清理（可选）

```bash
# 只清理已停止的容器
docker container prune -f

# 只清理悬空镜像（无 tag 的中间层）
docker image prune -f

# 激进清理：已停止容器 + 悬空镜像 + 未使用网络 + 全部构建缓存
# ⚠️ 会删除构建缓存，下次 docker build 将从头开始（无缓存加速）
docker system prune -f
```

---

## TechEmpower Framework Benchmarks (TFB)

Hical 参与 [TechEmpower Framework Benchmarks](https://www.techempower.com/benchmarks/) 跑分，当前实现了 `json` 和 `plaintext` 两个测试类型。

```bash
# 快速开始：构建并验证
cd ~/Hical
docker build -f docker/TFB/hical.dockerfile -t hical-tfb .
docker run --rm -p 8080:8080 hical-tfb

# 另一个终端验证
curl -i http://localhost:8080/json
curl -i http://localhost:8080/plaintext
```

详见 [TFB/README.md](TFB/README.md)。
