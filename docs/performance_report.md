# Hical 性能测试报告

> 原生 HTTP/WebSocket 网络栈性能分析、PMR 内存池基准测试、Docker 容器化压测与框架对比

---

## 目录

- [1. 测试环境](#1-测试环境)
- [2. 核心性能架构 (v2.6)](#2-核心性能架构v26)
- [3. PMR 内存池性能](#3-pmr-内存池性能)
- [4. HTTP 服务器吞吐量](#4-http-服务器吞吐量)
- [5. 路由分发性能](#5-路由分发性能)
- [6. 零拷贝与 Scatter-Gather 优化](#6-零拷贝与-scatter-gather-优化)
- [7. Docker 容器化压测](#7-docker-容器化压测)
- [8. 性能调优指南](#8-性能调优指南)
- [9. 与主流框架对比分析](#9-与主流框架对比分析)
- [10. 测试工具与复现方法](#10-测试工具与复现方法)

---

## 1. 测试环境

### 硬件配置

| 项目 | 规格                            |
| ---- | ------------------------------- |
| CPU  | 推荐 8 核以上（多线程测试需要） |
| 内存 | 16GB+                           |
| 存储 | SSD                             |
| 网络 | 本地回环 (localhost)            |

### 软件环境

| 组件     | 版本                        |
| -------- | --------------------------- |
| 操作系统 | Windows 10 / Ubuntu 24.04   |
| 编译器   | GCC 14+ / Clang 20+ (C++20) |
| Boost    | 1.82+（DB 中间件 1.85+）    |
| OpenSSL  | 3.0+                        |
| CMake    | 3.20+                       |
| 构建类型 | Release (-O2)               |

> **注意**：以下数据为框架基准测试方法论与分析维度说明。具体数值因硬件环境差异较大，用户应在目标部署环境上运行附带的基准测试工具获取实际数据。

---

## 2. 核心性能架构（v2.6）

v2.6.0 将 HTTP 解析/序列化和 WebSocket 全面替换为原生实现（picohttpparser + 自研 WebSocket），性能大幅提升。以下是关键优化点：

### 2.1 原生 HTTP 栈

| 组件        | 优化前 (Beast)          | 优化后 (自研)                                        | 收益                           |
| ----------- | ----------------------- | ---------------------------------------------------- | ------------------------------ |
| HTTP 解析器 | Boost.Beast HTTP parser | picohttpparser                                       | CPU 热点占比大幅下降           |
| 请求表示    | 拥有式 string 拷贝      | `string_view` 引用 `ReadBufferPool` 借来的 `readBuf` | 零堆分配，空闲连接不持有缓冲区 |
| 头部存储    | Beast 内部容器          | 栈上 `array<Entry,64>` (`RequestHeaders`)            | 零堆分配                       |
| 响应序列化  | Beast serializer        | `FixedBuffer<512>` 栈缓冲 + scatter-gather           | 单次 `async_write`             |

### 2.2 多 Acceptor 架构 (SO_REUSEPORT)

```
┌─────────────────────────────────────────────────┐
│                   内核 TCP 栈                     │
│       SO_REUSEPORT 内核级负载均衡                 │
└──────┬──────────┬──────────┬──────────┬─────────┘
       │          │          │          │
  ┌────▼────┐┌───▼────┐┌───▼────┐┌───▼────┐
  │Worker 0 ││Worker 1││Worker 2││Worker 3│
  │acceptor ││acceptor││acceptor││acceptor│
  │  + I/O  ││  + I/O ││  + I/O ││  + I/O │
  └─────────┘└────────┘└────────┘└────────┘
```

- 每个 worker loop 独立 acceptor，accept 和 I/O 在同一线程
- **零跨线程调度**：新连接无需 `post()` 到其他线程
- Windows 自动回退为单 acceptor + least-connections 分发
- Linux/macOS 下消除了 accept 锁竞争

### 2.3 同步快速路径

同步注册的路由 handler 和中间件可跳过协程帧分配：

| 路径                                     | 机制                                           | 开销           |
| ---------------------------------------- | ---------------------------------------------- | -------------- |
| `Router::dispatchSync()`                 | 同步 handler 直接返回结果                      | 零协程帧       |
| `SyncBeforeHandler` / `SyncAfterHandler` | 前置/后置同步中间件                            | 零协程帧       |
| `buildOptimizedChain()`                  | N 层连续同步中间件合并                         | 仅 1 次堆分配  |
| `HttpSessionImpl` 无中间件路径           | 先尝试 `dispatchSync()`，`nullopt` 时 fallback | 热路径零协程帧 |

### 2.4 编译防火墙

| 文件                    | 隔离内容                          | 收益                                    |
| ----------------------- | --------------------------------- | --------------------------------------- |
| `GenericConnection.hci` | ~780 行模板实现                   | 修改用户代码不重编译                    |
| `HttpSessionImpl.cpp`   | picohttpparser + WebSocket 重模板 | `HttpServer.h` 修改不触发 parser 重编译 |
| `MetaJsonError.h/cpp`   | `throw` 非模板化                  | 减少 `HICAL_JSON` 实例化代码体积        |

### 2.5 无锁化优化

| 组件                     | 优化前                         | 优化后                      | 收益                                  |
| ------------------------ | ------------------------------ | --------------------------- | ------------------------------------- |
| GenericConnection 写队列 | `mutex` + `deque`              | Vyukov Intrusive MPSC Queue | wait-free O(1) push，消除写路径锁竞争 |
| TcpServer 连接表         | 全局 `mutex` + `unordered_set` | per-loop `LoopShard` 分片   | idle 扫描/增删全程无锁                |
| requestPool upstream     | `globalPool`（同步池）         | `threadLocalPool`           | 扩容零锁竞争                          |

### 2.6 热路径微优化

- HTTP header 查找：按长度 + 首字符快速过滤
- 响应头 `insert()` O(1) 替代 `set()` O(N)
- `attributes_` 延迟构造（绝大多数请求不使用属性）
- 200 OK 状态行预计算字面量
- HTTP Date 头 `thread_local` 每秒缓存更新
- `ReadBufferPool` 借还 `readBuf`：请求期间借用，响应写完归还，空闲连接零缓冲区驻留

### 2.7 高并发场景优化（v2.6.3）

strace + perf 火焰图实测 10K 并发下发现的浪费点，逐一消除：

| 优化项                     | 优化前                                                       | 优化后                                            | 收益                                              |
| -------------------------- | ------------------------------------------------------------ | ------------------------------------------------- | ------------------------------------------------- |
| Idle timeout 条件初始化    | 每连接无条件分配 `socketAlive` + `lastActiveMs` + timer 协程 | 仅 `idleTimeout_ > 0` 时分配                      | benchmark 场景省 3× `make_shared` + 1 个协程/连接 |
| SocketGuard shutdown       | 析构时无条件 `shutdown()`（99.99% 失败）                     | 仅 `cleanExit`（正常 keep-alive 结束）才 shutdown | 省一次无效系统调用/连接                           |
| coSpawn completion handler | 每次 co_spawn 都 malloc/free handler                         | `recycling_allocator` + `thread_local` 缓存复用   | 高并发下消除大量小对象分配                        |
| Worker 线程 CPU 亲和性     | 内核随意迁移线程                                             | `pthread_setaffinity_np` 绑核                     | 消除 TLB flush + 跨核 IPI                         |
| mimalloc 替代 glibc malloc | glibc 的 `mprotect` arena 扩展                               | mimalloc（benchmark 构建）                        | 减少内核态 syscall                                |
| RPS/RFS 网络亲和           | 收包 softirq 随机分配                                        | 容器启动时配置 RPS/RFS 对齐处理线程 CPU           | 减少跨核数据搬运                                  |

### 2.8 空闲长连接内存优化（v2.6.5）

面向百万级空闲长连接场景的内存压降优化：

| 优化项                | 优化前                             | 优化后                                                | 每连接节省  |
| --------------------- | ---------------------------------- | ----------------------------------------------------- | ----------- |
| readBuf 借还          | 连接常驻 8KB `std::string`         | `ReadBufferPool` 按需借还，空闲时不持有               | **~7.5 KB** |
| `inputBuffer_` 懒分配 | 连接建立即分配 `PmrBuffer`（~2KB） | `std::optional<PmrBuffer>`，`readLoop` 第一次进才创建 | **~2 KB**   |

**合计**：空闲连接从 ~17.44 KB 降至 ~8 KB（节省约 55%），百万连接内存占用从 ~16.6 GB 降至 ~7–8 GB（内核侧 TCP 缓冲 ~4 KB/连接不可优化）。

### 2.9 消除多余的 epoll_ctl(MOD) 调用（v2.6.6）

strace 实测 10K 并发下，每次 keep-alive 请求的 `async_wait(wait_read)` 都触发一次多余的 `epoll_ctl(MOD)`——EPOLLIN 早在注册连接时就加了，反复 MOD 纯属浪费。改法：循环顶用 256B 栈缓冲 `async_read_some` 替代 `async_wait`，走 Asio 投机路径，有数据直接返回、没数据挂起等 epoll 通知，全程不碰 epoll_ctl。

| 指标 | 优化前 | 优化后 | 变化 |
|------|:------:|:------:|:----:|
| epoll_ctl 调用数 | 32,563 | 9,173 | ↓71.8% |
| epoll_ctl 时间占比 | 26.51% | 5.57% | ↓20.94 p.p. |
| 空闲连接额外内存 | 0 | +256B（协程帧栈） | 可忽略 |
| 总系统调用数 | 99,579 | 79,504 | ↓20.2% |

---

## 3. PMR 内存池性能

### 3.1 测试场景

Hical 提供了两个 PMR 基准测试程序：

- **`pmr_poc`** — PMR 概念验证，涵盖缓冲区复用、批量分配、PmrBuffer 功能和多线程并发
- **`pmr_benchmark`** — 系统化性能对比，覆盖不同分配器策略和不同块大小

### 3.2 测试维度

#### 测试 1：单线程分配/释放性能

对比五种分配策略在不同块大小下的表现：

| 分配策略                      | 描述             | 预期特点           |
| ----------------------------- | ---------------- | ------------------ |
| `new/delete`                  | 标准堆分配       | 基线，有全局锁     |
| `pmr::synchronized_pool`      | 同步池           | 线程安全，有锁开销 |
| `pmr::unsynchronized_pool`    | 非同步池         | 无锁，单线程最快   |
| `pmr::monotonic`              | 单调池           | 只分配不释放，极速 |
| `hical::threadLocalAllocator` | Hical 线程本地池 | 无锁 + 池化复用    |

测试块大小梯度：64B、256B、4KB，各执行 50 万~100 万次。

**典型性能排序（由快到慢）：**

```
monotonic > unsynchronized_pool ≈ hical threadLocal > synchronized_pool > new/delete
```

**关键指标解读：**

- **monotonic 池**：仅递增指针分配，性能最优。适合请求级一次性分配场景
- **unsynchronized_pool / hical threadLocal**：无锁池化，接近 monotonic 性能。适合线程内频繁分配/释放
- **synchronized_pool**：有锁但池化复用，优于 new/delete
- **new/delete**：全局堆操作，多线程下锁竞争严重

#### 测试 2：多线程并发分配

使用全部硬件线程并发分配 256B 块，每线程 50 万次：

| 分配策略                      | 并发特点                           |
| ----------------------------- | ---------------------------------- |
| `new/delete`                  | 全局锁竞争，线程越多性能下降越严重 |
| `hical::threadLocalAllocator` | 每线程独享池，零竞争，线性扩展     |

**Hical 线程本地池优势**：
- 每个线程有独立的 `unsynchronized_pool_resource`
- 线程间无共享状态，无锁竞争
- 全局池统计显示极少的跨线程分配（仅初始化时向全局池申请大块）

#### 测试 3：PmrBuffer 追加性能

对比三种缓冲区策略在 128B 数据追加场景下的表现：

| 策略              | 描述                  |
| ----------------- | --------------------- |
| `std::string`     | 标准字符串追加        |
| `PmrBuffer(默认)` | 使用默认分配器        |
| `PmrBuffer(pool)` | 使用 hical 线程本地池 |

**预期结果**：PmrBuffer(pool) 在频繁追加/清空循环中优于 std::string，因为池化分配器避免了反复向系统申请/归还内存。

### 3.3 运行方式

```bash
# 编译
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build

# 运行 PMR PoC
./build/examples/pmr_poc

# 运行 PMR 基准测试
./build/examples/pmr_benchmark
```

### 3.4 内存池监控

运行时可通过 `MemoryPool::getStats()` 监控内存使用：

```cpp
auto stats = MemoryPool::instance().getStats();
// stats.totalAllocations      — 全局池总分配次数
// stats.totalDeallocations    — 全局池总释放次数
// stats.currentBytesAllocated — 当前已分配字节数
// stats.peakBytesAllocated    — 峰值字节数
```

**健康指标**：
- `totalAllocations` 远小于请求处理次数 → 池化复用效果好
- `currentBytesAllocated` 稳定不增长 → 无内存泄漏
- `peakBytesAllocated` 合理 → 未出现内存飙升

---

## 4. HTTP 服务器吞吐量

### 4.1 测试工具

Hical 内置 HTTP 基准测试客户端 `http_benchmark`，支持：

- 可配置并发连接数
- 可配置每连接请求数
- 支持 GET/POST 等方法
- 延迟分布统计（P50/P90/P95/P99）
- Keep-Alive 连接复用

### 4.2 测试场景

#### 场景 A：静态路由 GET 请求

```bash
# 启动服务器
./build/examples/http_server 8080

# 压测：50 并发 x 1000 请求/连接
./build/examples/http_benchmark localhost 8080 50 1000 /api/status GET
```

- **路由类型**：静态路由（哈希表 O(1) 查找）
- **响应类型**：JSON `{"status":"running","version":"1.0.0","framework":"hical"}`
- **关注指标**：QPS、平均延迟、P99 延迟

#### 场景 B：POST Echo 请求

```bash
./build/examples/http_benchmark localhost 8080 50 1000 /api/echo POST '{"hello":"world"}'
```

- **请求体**：JSON 数据
- **响应类型**：原样回写
- **关注指标**：QPS、请求体处理开销

#### 场景 C：参数路由 GET 请求

```bash
./build/examples/http_benchmark localhost 8080 50 1000 /users/42 GET
```

- **路由类型**：参数路由（线性匹配）
- **关注指标**：参数路由 vs 静态路由的性能差距

### 4.3 输出指标

`http_benchmark` 输出以下指标：

```
========== 测试结果 ==========
  总请求数:    50000
  成功请求:    50000
  失败请求:    0
  总耗时:      xxx ms
  QPS:         xxxxx req/s

  延迟分布:
    平均:  x.xx ms
    P50:   x.xx ms
    P90:   x.xx ms
    P95:   x.xx ms
    P99:   x.xx ms
    最小:  x.xx ms
    最大:  x.xx ms
==============================
```

### 4.4 多线程 IO 测试

```cpp
// 单线程
HttpServer server(8080, 1);

// 4 线程
HttpServer server(8080, 4);

// 匹配 CPU 核数
HttpServer server(8080, std::thread::hardware_concurrency());
```

对比不同 IO 线程数下的 QPS 变化，观察线性扩展能力。

### 4.5 性能影响因素

| 因素       | 影响                         | 优化建议                 |
| ---------- | ---------------------------- | ------------------------ |
| IO 线程数  | 直接影响并发能力             | 设为 CPU 核数            |
| Keep-Alive | 减少连接建立开销             | 默认启用                 |
| 响应体大小 | 影响带宽吞吐                 | 小响应体更能反映框架开销 |
| 内存池配置 | 影响分配速度                 | 调整 PoolConfig 参数     |
| 编译优化   | Release 模式性能数倍于 Debug | 务必使用 -O2             |

---

## 5. 路由分发性能

### 5.1 测试用例

项目内置路由性能测试 `test_router_perf`，覆盖：

| 测试          | 描述                           |
| ------------- | ------------------------------ |
| 100 路由查找  | 注册 100 条静态路由，随机查找  |
| 1000 路由查找 | 注册 1000 条静态路由，随机查找 |
| 参数路由匹配  | `string_view` 零分配路径段解析 |

### 5.2 静态路由性能

静态路由使用 `unordered_map<RouteKey, RouteHandler>`：

- **查找复杂度**：O(1) 平均
- **哈希函数**：`hash(method) ^ (hash(path) << 1)`
- **预期性能**：路由数量增长不影响查找时间

### 5.3 参数路由性能

参数路由使用 `vector<ParamRouteEntry>` 线性匹配：

- **查找复杂度**：O(n)，n 为参数路由数量
- **零分配优化**：使用 `string_view` 分割路径段，避免字符串拷贝
- **适用范围**：参数路由数量通常较少（< 50），线性扫描可接受

### 5.4 运行方式

```bash
# 运行路由性能测试
ctest --test-dir build -R test_router_perf --output-on-failure
```

---

## 6. 零拷贝与 Scatter-Gather 优化

### 6.1 零拷贝策略

Hical 在以下环节减少内存拷贝：

| 环节     | 优化                                |
| -------- | ----------------------------------- |
| 网络读取 | 直接读入 PmrBuffer，无中间栈缓冲区  |
| 数据发送 | `send(string&&)` 移动语义，避免拷贝 |
| 路径解析 | `string_view` 零分配段分割          |
| 路由查找 | 哈希表直接定位，无遍历拷贝          |

### 6.2 Scatter-Gather I/O

当写队列中有多条待发送消息时，GenericConnection 使用 Scatter-Gather 合并为一次系统调用：

```cpp
// 传统方式：N 条消息 = N 次 async_write
for (auto& msg : queue) {
    co_await async_write(socket, buffer(msg));  // N 次系统调用
}

// Hical Scatter-Gather：N 条消息 = 1 次 async_write
std::vector<const_buffer> buffers;
for (auto& msg : queue) {
    buffers.emplace_back(msg.data(), msg.size());
}
co_await async_write(socket, buffers);  // 1 次系统调用
```

**收益**：减少系统调用次数，降低内核态切换开销。在高吞吐量场景（如推送多条消息给客户端）效果显著。

### 6.3 响应前缀模板化

keep-alive 连接上，`Server` / `Connection` / `Date` 三个通用响应头在连接生命周期内几乎不变。Hical 在连接级别预构建这段 wire bytes（~90B），每个请求只需一次 `memcpy` 追加到 `FixedBuffer`，替代 3 次 `HeaderMap::insert`（6 个 `std::string` 构造）+ 序列化循环中多遍历 3 个 entry。

| 指标               | 传统方式                  | 前缀模板                      |
| ------------------ | ------------------------- | ----------------------------- |
| 每请求 string 构造 | 6 次                      | 0 次                          |
| 每请求 vector 操作 | 3 次 emplace_back         | 0 次                          |
| 头部序列化循环     | 遍历 N+3 个 entry         | 遍历 N 个 entry + 1 次 memcpy |
| Date 更新频率      | 每请求调用 cachedHttpDate | 每秒 1 次 memcpy(29B)         |

### 6.4 TcpCorkGuard（文件响应路径）

`writeFileResponse()` 需要先发头部、再分块发文件。TCP_NODELAY 启用时头部会独立成一个小 TCP 段。`TcpCorkGuard` RAII 在函数入口 cork socket（Linux `TCP_CORK` / macOS `TCP_NOPUSH`），析构时 uncork flush：

- 头部 ~200B + 首个 64KB chunk 合并为一个 TCP 段
- 消除接收方 delayed ACK 等待
- Windows 下 no-op（依赖已有的应用层 scatter-gather）

---

## 7. Docker 容器化压测

Hical 提供两套 Docker 压测方案，确保环境一致性和可复现性。

### 7.1 快速压测（单机）

使用 `docker/docker-compose.bench.yml`，在同一台机器上启动 server 和 wrk 容器：

```bash
# 在项目根目录执行
docker compose -f docker/docker-compose.bench.yml up --build --abort-on-container-exit
```

- server 和 wrk 各限制 4 CPU / 1GB 内存，fd 上限 65536
- 默认压测 30 秒，4 线程
- 自定义参数：`DURATION=60s THREADS=4 docker compose -f docker/docker-compose.bench.yml up --build`
- **v2.6.3 新增**：bench-server 启用 mimalloc（`HICAL_WITH_MIMALLOC=ON`），入口脚本自动配置 RPS/RFS 网络亲和（需 `CAP_NET_ADMIN`），`setIdleTimeout(0)` 关闭空闲检测省掉 per-connection timer 开销

### 7.2 跨 VM 压测

分离 server 和 wrk 到不同机器，消除本地回环干扰：

```bash
# VM-A (server):
docker compose -f docker/docker-compose.bench.yml up --build bench-server

# VM-B (wrk):
docker run --rm -e SERVER_HOST=<VM-A-IP>:8080 hical-bench-wrk
```

### 7.3 多框架横向对比

使用 `benchmark/docker-compose.yml` 对比多个 C++ 框架和跨语言框架：

```bash
# C++ 框架对比（Hical / Drogon / Crow / Oat++ / cpp-httplib / Cinatra）
docker compose -f benchmark/docker-compose.yml --profile cpp up --build

# 跨语言对比（Hical / Gin / Actix / Fiber）
docker compose -f benchmark/docker-compose.yml --profile cross-lang up --build

# 性能剖析模式（附带 perf/FlameGraph）
docker compose -f benchmark/docker-compose.yml --profile profiling up --build
```

所有框架统一限制 4 CPU / 512MB 内存，wrk 容器自动逐个压测并输出 `results.md`。

### 7.4 TFB 模式

`docker/TFB/bench_main.cpp` 是 TechEmpower Framework Benchmarks 专用入口，仅暴露 `/json` + `/plaintext` 路由，集成 mimalloc 分配器（正式场景可通过 `HICAL_WITH_MIMALLOC=ON` 启用），适合与 TFB 排行榜数据对比。

---

## 8. 性能调优指南

### 8.1 编译优化

```bash
# Release 模式编译（启用 -O2 优化）
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

**Debug vs Release 性能差距**：Release 模式下 STL 容器和 Boost 库会移除断言和边界检查，性能可能提升 3-10 倍。

### 8.2 IO 线程数配置

```cpp
// 根据 CPU 核数设置
HttpServer server(8080, std::thread::hardware_concurrency());
```

- **CPU 密集型处理器**：IO 线程数 = CPU 核数
- **IO 密集型（大量等待）**：IO 线程数 = CPU 核数 * 2
- **单核环境**：IO 线程数 = 1（避免上下文切换开销）

### 8.3 PMR 内存池调优

```cpp
PoolConfig config;

// 高并发短请求场景
config.requestPoolInitialSize = 4096;       // 小请求池
config.threadLocalMaxBlocksPerChunk = 128;  // 更多块复用

// 大请求体场景（文件上传等）
config.requestPoolInitialSize = 65536;      // 64KB 请求池
config.threadLocalLargestPoolBlock = 1024 * 1024; // 1MB 最大块

MemoryPool::instance().configure(config);
```

### 8.4 性能监控

运行时通过内存池统计接口监控：

```cpp
// 定期采集
auto stats = MemoryPool::instance().getStats();

// 告警条件示例
if (stats.currentBytesAllocated > 500 * 1024 * 1024) {
    LOG_WARN("内存使用超过 500MB");
}
if (stats.totalAllocations - stats.totalDeallocations > 10000) {
    LOG_WARN("疑似内存泄漏，未释放分配数过多");
}
```

---

## 9. 与主流框架对比分析

### 9.1 对比维度

| 维度       | 说明                 |
| ---------- | -------------------- |
| QPS        | 每秒处理请求数       |
| 延迟       | P50 / P99 延迟分布   |
| 内存占用   | 稳态内存使用量       |
| 内存碎片   | 长时间运行后的碎片率 |
| CPU 利用率 | 多核利用效率         |
| 连接容量   | 最大并发连接数       |

### 9.2 架构层面对比

| 特性        | Hical                      | Drogon                 | Cinatra                 | Crow                 |
| ----------- | -------------------------- | ---------------------- | ----------------------- | -------------------- |
| 语言        | C++20/26                   | C++17                  | C++20                   | C++11/14             |
| 异步模型    | 协程 (co_await)            | 回调 + 协程            | 协程 (co_await)         | 多线程 + 回调        |
| HTTP 解析   | picohttpparser（零拷贝栈） | 自研 (Trantor)         | picohttpparser          | 自研 (http_parser)   |
| WebSocket   | 自研 RFC 6455 + deflate    | Beast WebSocket        | —                       | 内置 WebSocket       |
| 内存管理    | PMR 三层池                 | 传统分配器             | 传统分配器              | 传统分配器           |
| 线程模型    | SO_REUSEPORT 多 acceptor   | 1:1 (thread:EventLoop) | 1:1 (thread:io_context) | 线程池               |
| SSL         | 模板化编译期分支           | 运行时分支             | 运行时分支              | Boost.Asio SSL       |
| 路由        | 哈希表 O(1) + 同步快速路径 | 基数树                 | 基数树                  | Trie 树              |
| 中间件      | 洋葱模型 + SyncMiddleware  | AOP 过滤器             | —                       | before/after handler |
| 反射/序列化 | C++26 反射 + 宏回退        | 宏 + JSON 自动         | 自动序列化              | —                    |

### 9.3 测试方法

使用外部工具 `wrk` 进行跨框架对比：

```bash
# 安装 wrk
# Ubuntu: apt install wrk
# macOS: brew install wrk

# 4 线程 100 并发 30 秒
wrk -t4 -c100 -d30s http://localhost:8080/api/status
```

或使用 Hical 内置的 `http_benchmark`：

```bash
./build/examples/http_benchmark localhost 8080 100 10000 /api/status GET
```

---

## 10. 测试工具与复现方法

### 10.1 内置测试工具

| 工具               | 路径                          | 用途                       |
| ------------------ | ----------------------------- | -------------------------- |
| `pmr_poc`          | `examples/pmr_poc.cpp`        | PMR 概念验证和功能测试     |
| `pmr_benchmark`    | `examples/pmr_benchmark.cpp`  | PMR 分配器系统化性能对比   |
| `benchmark`        | `examples/benchmark.cpp`      | TCP Echo Server 压力测试   |
| `http_benchmark`   | `examples/http_benchmark.cpp` | HTTP 服务器 QPS 和延迟测试 |
| `test_router_perf` | `tests/test_router_perf.cpp`  | 路由分发性能测试           |

### 10.2 完整测试流程

```bash
# 1. Release 模式编译
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build

# 2. 运行单元测试（确保功能正确）
ctest --test-dir build --output-on-failure --timeout 60 -j4

# 3. PMR 内存池基准测试
./build/examples/pmr_poc
./build/examples/pmr_benchmark

# 4. 路由性能测试
ctest --test-dir build -R test_router_perf --output-on-failure

# 5. HTTP 吞吐量测试
# 终端 1：启动服务器
./build/examples/http_server 8080

# 终端 2：运行压测
./build/examples/http_benchmark localhost 8080 50 1000 /api/status GET
./build/examples/http_benchmark localhost 8080 50 1000 /api/echo POST '{"test":true}'
./build/examples/http_benchmark localhost 8080 100 5000 / GET

# 6. Echo Server 压测（底层网络性能基线）
# 终端 1：启动 Echo Server
./build/examples/echo_server 8888

# 终端 2：运行压测
./build/examples/benchmark localhost 8888 100 1000
```

### 10.3 外部压测工具

```bash
# wrk — 高性能 HTTP 压测
wrk -t4 -c100 -d30s http://localhost:8080/api/status

# ab (Apache Bench)
ab -n 10000 -c 100 http://localhost:8080/api/status

# curl — 功能验证
curl http://localhost:8080/api/status
curl -X POST -d '{"hello":"world"}' http://localhost:8080/api/echo
```

### 10.4 注意事项

1. **编译模式**：务必使用 Release 模式（`-DCMAKE_BUILD_TYPE=Release`），Debug 模式的性能数据无参考价值
2. **预热**：首次请求可能包含初始化开销，建议先发送少量预热请求
3. **资源限制**：高并发测试前检查系统 ulimit（Linux）或 TCP 连接数限制（Windows）
4. **本地回环**：本地测试消除了网络延迟因素，聚焦于框架本身性能
5. **多次取平均**：建议每个场景至少运行 3 次取中位数

---

> 更多信息：[架构设计](architecture.md) | [API 文档](api_reference.md) | [快速上手](quickstart_cn.md)
