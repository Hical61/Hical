# Hical 性能测试报告

> PMR 内存池基准测试、HTTP 吞吐量测试与框架性能分析

---

## 目录

- [1. 测试环境](#1-测试环境)
- [2. PMR 内存池性能](#2-pmr-内存池性能)
- [3. HTTP 服务器吞吐量](#3-http-服务器吞吐量)
- [4. 路由分发性能](#4-路由分发性能)
- [5. 零拷贝与 Scatter-Gather 优化](#5-零拷贝与-scatter-gather-优化)
- [6. 性能调优指南](#6-性能调优指南)
- [7. 与主流框架对比分析](#7-与主流框架对比分析)
- [8. 测试工具与复现方法](#8-测试工具与复现方法)

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
| Boost    | 1.78+                       |
| OpenSSL  | 3.0+                        |
| CMake    | 3.20+                       |
| 构建类型 | Release (-O2)               |

> **注意**：以下数据为框架基准测试方法论与分析维度说明。具体数值因硬件环境差异较大，用户应在目标部署环境上运行附带的基准测试工具获取实际数据。

---

## 2. PMR 内存池性能

### 2.1 测试场景

Hical 提供了两个 PMR 基准测试程序：

- **`pmr_poc`** — PMR 概念验证，涵盖缓冲区复用、批量分配、PmrBuffer 功能和多线程并发
- **`pmr_benchmark`** — 系统化性能对比，覆盖不同分配器策略和不同块大小

### 2.2 测试维度

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

### 2.3 运行方式

```bash
# 编译
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build

# 运行 PMR PoC
./build/examples/pmr_poc

# 运行 PMR 基准测试
./build/examples/pmr_benchmark
```

### 2.4 内存池监控

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

## 3. HTTP 服务器吞吐量

### 3.1 测试工具

Hical 内置 HTTP 基准测试客户端 `http_benchmark`，支持：

- 可配置并发连接数
- 可配置每连接请求数
- 支持 GET/POST 等方法
- 延迟分布统计（P50/P90/P95/P99）
- Keep-Alive 连接复用

### 3.2 测试场景

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

### 3.3 输出指标

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

### 3.4 多线程 IO 测试

```cpp
// 单线程
HttpServer server(8080, 1);

// 4 线程
HttpServer server(8080, 4);

// 匹配 CPU 核数
HttpServer server(8080, std::thread::hardware_concurrency());
```

对比不同 IO 线程数下的 QPS 变化，观察线性扩展能力。

### 3.5 性能影响因素

| 因素       | 影响                         | 优化建议                 |
| ---------- | ---------------------------- | ------------------------ |
| IO 线程数  | 直接影响并发能力             | 设为 CPU 核数            |
| Keep-Alive | 减少连接建立开销             | 默认启用                 |
| 响应体大小 | 影响带宽吞吐                 | 小响应体更能反映框架开销 |
| 内存池配置 | 影响分配速度                 | 调整 PoolConfig 参数     |
| 编译优化   | Release 模式性能数倍于 Debug | 务必使用 -O2             |

---

## 4. 路由分发性能

### 4.1 测试用例

项目内置路由性能测试 `test_router_perf`，覆盖：

| 测试          | 描述                           |
| ------------- | ------------------------------ |
| 100 路由查找  | 注册 100 条静态路由，随机查找  |
| 1000 路由查找 | 注册 1000 条静态路由，随机查找 |
| 参数路由匹配  | `string_view` 零分配路径段解析 |

### 4.2 静态路由性能

静态路由使用 `unordered_map<RouteKey, RouteHandler>`：

- **查找复杂度**：O(1) 平均
- **哈希函数**：`hash(method) ^ (hash(path) << 1)`
- **预期性能**：路由数量增长不影响查找时间

### 4.3 参数路由性能

参数路由使用 `vector<ParamRouteEntry>` 线性匹配：

- **查找复杂度**：O(n)，n 为参数路由数量
- **零分配优化**：使用 `string_view` 分割路径段，避免字符串拷贝
- **适用范围**：参数路由数量通常较少（< 50），线性扫描可接受

### 4.4 运行方式

```bash
# 运行路由性能测试
ctest --test-dir build -R test_router_perf --output-on-failure
```

---

## 5. 零拷贝与 Scatter-Gather 优化

### 5.1 零拷贝策略

Hical 在以下环节减少内存拷贝：

| 环节     | 优化                                |
| -------- | ----------------------------------- |
| 网络读取 | 直接读入 PmrBuffer，无中间栈缓冲区  |
| 数据发送 | `send(string&&)` 移动语义，避免拷贝 |
| 路径解析 | `string_view` 零分配段分割          |
| 路由查找 | 哈希表直接定位，无遍历拷贝          |

### 5.2 Scatter-Gather I/O

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

---

## 6. 性能调优指南

### 6.1 编译优化

```bash
# Release 模式编译（启用 -O2 优化）
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

**Debug vs Release 性能差距**：Release 模式下 STL 容器和 Boost 库会移除断言和边界检查，性能可能提升 3-10 倍。

### 6.2 IO 线程数配置

```cpp
// 根据 CPU 核数设置
HttpServer server(8080, std::thread::hardware_concurrency());
```

- **CPU 密集型处理器**：IO 线程数 = CPU 核数
- **IO 密集型（大量等待）**：IO 线程数 = CPU 核数 * 2
- **单核环境**：IO 线程数 = 1（避免上下文切换开销）

### 6.3 PMR 内存池调优

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

### 6.4 性能监控

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

## 7. 与主流框架对比分析

### 7.1 对比维度

| 维度       | 说明                 |
| ---------- | -------------------- |
| QPS        | 每秒处理请求数       |
| 延迟       | P50 / P99 延迟分布   |
| 内存占用   | 稳态内存使用量       |
| 内存碎片   | 长时间运行后的碎片率 |
| CPU 利用率 | 多核利用效率         |
| 连接容量   | 最大并发连接数       |

### 7.2 架构层面对比

| 特性      | Hical                   | Drogon                 | Nginx                   |
| --------- | ----------------------- | ---------------------- | ----------------------- |
| 语言      | C++20                   | C++17                  | C                       |
| 异步模型  | 协程 (co_await)         | 回调 + 协程            | 事件驱动 (epoll/kqueue) |
| HTTP 解析 | Boost.Beast             | 自研 (Trantor)         | 自研                    |
| 内存管理  | PMR 三层池              | 传统分配器             | slab 分配器             |
| 线程模型  | 1:1 (thread:io_context) | 1:1 (thread:EventLoop) | 多进程 worker           |
| SSL       | 模板化编译期分支        | 运行时分支             | OpenSSL                 |
| 路由      | 哈希表 + 线性           | 基数树                 | 前缀匹配                |

### 7.3 测试方法

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

## 8. 测试工具与复现方法

### 8.1 内置测试工具

| 工具               | 路径                          | 用途                       |
| ------------------ | ----------------------------- | -------------------------- |
| `pmr_poc`          | `examples/pmr_poc.cpp`        | PMR 概念验证和功能测试     |
| `pmr_benchmark`    | `examples/pmr_benchmark.cpp`  | PMR 分配器系统化性能对比   |
| `benchmark`        | `examples/benchmark.cpp`      | TCP Echo Server 压力测试   |
| `http_benchmark`   | `examples/http_benchmark.cpp` | HTTP 服务器 QPS 和延迟测试 |
| `test_router_perf` | `tests/test_router_perf.cpp`  | 路由分发性能测试           |

### 8.2 完整测试流程

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

### 8.3 外部压测工具

```bash
# wrk — 高性能 HTTP 压测
wrk -t4 -c100 -d30s http://localhost:8080/api/status

# ab (Apache Bench)
ab -n 10000 -c 100 http://localhost:8080/api/status

# curl — 功能验证
curl http://localhost:8080/api/status
curl -X POST -d '{"hello":"world"}' http://localhost:8080/api/echo
```

### 8.4 注意事项

1. **编译模式**：务必使用 Release 模式（`-DCMAKE_BUILD_TYPE=Release`），Debug 模式的性能数据无参考价值
2. **预热**：首次请求可能包含初始化开销，建议先发送少量预热请求
3. **资源限制**：高并发测试前检查系统 ulimit（Linux）或 TCP 连接数限制（Windows）
4. **本地回环**：本地测试消除了网络延迟因素，聚焦于框架本身性能
5. **多次取平均**：建议每个场景至少运行 3 次取中位数

---

> 更多信息：[架构设计](architecture.md) | [API 文档](api_reference.md) | [快速上手](quickstart.md)
