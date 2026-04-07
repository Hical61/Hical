# Hical 架构设计文档

> 框架分层架构、核心设计决策与关键模块深度解析

---

## 目录

- [1. 设计目标](#1-设计目标)
- [2. 总体架构](#2-总体架构)
- [3. 分层设计详解](#3-分层设计详解)
- [4. PMR 内存池设计](#4-pmr-内存池设计)
- [5. 协程异步模型](#5-协程异步模型)
- [6. 路由系统设计](#6-路由系统设计)
- [7. 中间件管道设计](#7-中间件管道设计)
- [8. SSL/TLS 模板化设计](#8-ssltls-模板化设计)
- [9. C++ Concepts 后端抽象](#9-c-concepts-后端抽象)
- [10. 反射 API 包装层设计](#10-反射-api-包装层设计)
- [11. 连接管理与生命周期](#11-连接管理与生命周期)
- [12. 线程模型](#12-线程模型)
- [13. 错误处理体系](#13-错误处理体系)
- [14. 设计决策记录](#14-设计决策记录)

---

## 1. 设计目标

Hical 的核心设计目标是构建一个"**现代 C++ 业务逻辑层 + 工业级网络底层**"的混合架构 Web 框架：

| 目标         | 具体措施                                                    |
| ------------ | ----------------------------------------------------------- |
| **高性能**   | PMR 内存池减少碎片、零拷贝 I/O、Scatter-Gather 批量写入     |
| **现代 C++** | C++20 协程取代回调地狱、Concepts 编译期约束、模板化 SSL     |
| **可扩展**   | 抽象接口层 + NetworkBackend Concept，未来可插拔替换网络后端 |
| **开发友好** | 用户只需编写高层业务代码，框架自动处理底层异步和内存管理    |
| **类型安全** | 编译期路由类型检查，Concepts 约束后端接口完整性             |

---

## 2. 总体架构

Hical 采用**两层分离**架构：核心抽象层（`src/core/`）与网络实现层（`src/asio/`）。

```
┌──────────────────────────────────────────────────────┐
│                    用户业务代码                        │
│   server.router().get("/api", handler);              │
│   server.use(middleware);                             │
├──────────────────────────────────────────────────────┤
│              核心层 (src/core/)                        │
│  ┌──────────┐ ┌──────────┐ ┌────────────┐            │
│  │HttpServer│ │  Router  │ │ Middleware  │            │
│  │          │ │          │ │  Pipeline   │            │
│  └─────┬────┘ └─────┬────┘ └──────┬─────┘            │
│        │            │             │                   │
│  ┌─────┴────────────┴─────────────┴─────┐            │
│  │        HttpRequest / HttpResponse     │            │
│  │        WebSocketSession               │            │
│  │        Coroutine (Awaitable<T>)       │            │
│  └──────────────────┬───────────────────┘            │
│                     │                                 │
│  ┌──────────────────┴───────────────────┐            │
│  │         抽象接口层                     │            │
│  │  EventLoop  TcpConnection  Timer     │            │
│  │  MemoryPool  PmrBuffer  SslContext   │            │
│  │  Error  Concepts                      │            │
│  └──────────────────┬───────────────────┘            │
├──────────────────────┼───────────────────────────────┤
│              Asio 适配层 (src/asio/)                   │
│  ┌──────────────────┴───────────────────┐            │
│  │  AsioEventLoop   AsioTimer            │            │
│  │  GenericConnection<SocketType>        │            │
│  │  EventLoopPool   TcpServer            │            │
│  └──────────────────┬───────────────────┘            │
├──────────────────────┼───────────────────────────────┤
│              底层库                                    │
│  Boost.Asio   Boost.Beast   Boost.JSON   OpenSSL     │
└──────────────────────────────────────────────────────┘
```

### 核心设计原则

1. **上层不依赖下层实现** — `src/core/` 定义纯虚接口和 Concepts，不直接引用 Boost.Asio
2. **用户不感知网络细节** — `HttpServer` 封装了全部网络操作，用户只与 Router/Request/Response 交互
3. **内存池贯穿全链路** — 从网络缓冲区到 HTTP 消息体到 JSON 对象，共享 PMR 内存池

---

## 3. 分层设计详解

### 3.1 核心层 (`src/core/`)

核心层包含两类组件：

**用户直接使用的 API：**

| 组件                           | 职责                                       |
| ------------------------------ | ------------------------------------------ |
| `HttpServer`                   | 顶层门面，整合路由+中间件+网络层，一键启动 |
| `Router`                       | 路由注册与分发，静态路由 O(1) + 参数路由   |
| `HttpRequest` / `HttpResponse` | Beast HTTP 消息的 hical 风格封装           |
| `Middleware`                   | 洋葱模型中间件管道                         |
| `WebSocketSession`             | WebSocket 会话封装                         |
| `Coroutine`                    | `Awaitable<T>` 别名和协程工具函数          |

**基础设施（框架内部 + 进阶用户）：**

| 组件            | 职责                              |
| --------------- | --------------------------------- |
| `EventLoop`     | 事件循环纯虚接口                  |
| `TcpConnection` | TCP 连接纯虚接口                  |
| `Timer`         | 定时器纯虚接口                    |
| `MemoryPool`    | 三层 PMR 内存池管理器             |
| `PmrBuffer`     | 基于 pmr 的读写缓冲区             |
| `SslContext`    | SSL/TLS 证书配置                  |
| `Error`         | Boost 错误码到 hical 错误码的映射 |
| `Concepts`      | C++20 Concepts 后端约束           |

### 3.2 Asio 适配层 (`src/asio/`)

适配层是核心抽象接口的 Boost.Asio 具体实现：

| 组件                            | 实现的接口      | 关键技术                            |
| ------------------------------- | --------------- | ----------------------------------- |
| `AsioEventLoop`                 | `EventLoop`     | 1 Thread : 1 `io_context`           |
| `AsioTimer`                     | `Timer`         | `boost::asio::steady_timer`         |
| `GenericConnection<SocketType>` | `TcpConnection` | 模板化，`if constexpr` 区分 TCP/SSL |
| `EventLoopPool`                 | —               | 多线程池，Round-Robin 分发          |
| `TcpServer`                     | —               | Accept 循环 + 连接生命周期管理      |

### 3.3 组件依赖关系

```
HttpServer
├── Router (路由注册和分发)
├── MiddlewarePipeline (中间件管道)
├── HttpRequest / HttpResponse (Beast 封装)
├── WebSocketSession (WebSocket)
├── SslContext (SSL/TLS 配置)
└── Coroutine (协程工具)

TcpServer
├── GenericConnection<SocketType> (连接模板)
├── EventLoopPool (IO 线程池)
├── AsioEventLoop (事件循环)
└── InetAddress (地址封装)

MemoryPool (独立基础设施，贯穿所有层)
├── TrackedResource (统计追踪)
├── synchronized_pool_resource (全局同步池)
├── unsynchronized_pool_resource (线程本地池)
└── monotonic_buffer_resource (请求级池)
```

---

## 4. PMR 内存池设计

### 4.1 设计动机

高并发 Web 服务器的性能瓶颈之一是频繁的内存分配/释放。传统 `new/delete` 存在以下问题：

- **锁竞争** — 多线程频繁分配时，全局堆的锁成为瓶颈
- **内存碎片** — 大量小块分配导致碎片化，降低缓存命中率
- **分配延迟** — 每次分配都需要遍历空闲链表

Hical 采用 C++17 PMR（Polymorphic Memory Resource）标准，设计了三层内存分配策略。

### 4.2 三层架构

```
┌──────────────────────────────────────────────┐
│          第一层：全局同步池                     │
│    synchronized_pool_resource                │
│    ┌────────────────────────────────────┐    │
│    │  TrackedResource (原子计数统计)      │    │
│    │  ↓ 上游: new_delete_resource       │    │
│    └────────────────────────────────────┘    │
│    用途：跨线程共享的全局分配                   │
│    特点：线程安全，有锁                         │
├──────────────────────────────────────────────┤
│          第二层：线程本地池                     │
│    unsynchronized_pool_resource (thread_local)│
│    ┌────────────────────────────────────┐    │
│    │  上游: 全局同步池                    │    │
│    └────────────────────────────────────┘    │
│    用途：每个线程独享的快速分配器               │
│    特点：无锁，零竞争                          │
├──────────────────────────────────────────────┤
│          第三层：请求级单调池                   │
│    monotonic_buffer_resource                 │
│    ┌────────────────────────────────────┐    │
│    │  上游: 线程本地池                    │    │
│    └────────────────────────────────────┘    │
│    用途：单次 HTTP 请求的生命周期分配           │
│    特点：只分配不释放，析构时整体回收            │
└──────────────────────────────────────────────┘
```

### 4.3 各层详细设计

#### 第一层：全局同步池

```cpp
// 追踪层包装 new_delete_resource，做原子计数
TrackedResource trackedResource_(std::pmr::new_delete_resource());

// 全局同步池，上游为追踪层
std::pmr::synchronized_pool_resource globalPool_(&trackedResource_);
```

- **职责**：跨线程共享的全局内存池
- **线程安全**：内部使用互斥锁
- **统计追踪**：通过 `TrackedResource` 记录分配/释放次数和字节数
- **适用场景**：跨线程共享的数据结构、连接池等

#### 第二层：线程本地池

```cpp
// 每个线程首次调用 threadLocalAllocator() 时自动创建
thread_local std::pmr::unsynchronized_pool_resource* threadPool = nullptr;
```

- **职责**：每个事件循环线程独享的高速分配器
- **无锁设计**：`unsynchronized_pool_resource` 无任何锁
- **上游关系**：当线程本地池需要更多内存时，向全局同步池申请大块
- **适用场景**：网络缓冲区（PmrBuffer）、连接内部数据

#### 第三层：请求级单调池

```cpp
auto pool = MemoryPool::instance().createRequestPool(4096);
// 请求处理期间所有分配都使用此 pool
// 请求结束时 pool 析构 → 所有内存一次性释放
```

- **职责**：单次 HTTP 请求生命周期内的分配器
- **零释放开销**：`monotonic_buffer_resource` 只分配不释放，析构时整体回收
- **内存局部性**：请求内的所有数据在连续内存中，缓存友好
- **适用场景**：HTTP 消息体、JSON 解析结果、临时字符串

### 4.4 TrackedResource 统计机制

`TrackedResource` 包装上游 `memory_resource`，在分配/释放时做原子计数：

```cpp
class TrackedResource : public std::pmr::memory_resource
{
    // do_allocate 时：
    totalAllocations_.fetch_add(1);
    currentBytes_.fetch_add(bytes);
    // CAS 更新 peakBytes_

    // do_deallocate 时：
    totalDeallocations_.fetch_add(1);
    currentBytes_.fetch_sub(bytes);
};
```

- 仅使用 `memory_order_relaxed`，性能开销极低
- 峰值追踪使用无锁 CAS（Compare-And-Swap），避免锁竞争

### 4.5 配置参数

```cpp
struct PoolConfig
{
    size_t globalMaxBlocksPerChunk = 128;          // 全局池每次向上游申请的块数
    size_t globalLargestPoolBlock = 1024 * 1024;   // 全局池管理的最大块 (1MB)
    size_t threadLocalMaxBlocksPerChunk = 64;      // 线程本地池每次向上游申请的块数
    size_t threadLocalLargestPoolBlock = 512 * 1024; // 线程本地池管理的最大块 (512KB)
    size_t requestPoolInitialSize = 4096;          // 请求级池初始缓冲区大小
};
```

参数设计原则：
- **全局池块较大** — 减少向系统申请内存的频率
- **线程本地池块适中** — 平衡内存占用与分配效率
- **请求级池 4KB 起步** — 覆盖大多数普通 HTTP 请求

### 4.6 内存流转图

```
HTTP 请求到达
    │
    ▼
TcpConnection 读取数据
    │  使用线程本地池分配 PmrBuffer
    ▼
HttpRequest 解析
    │  使用请求级单调池
    ▼
JSON 解析 (boost::json::value)
    │  使用请求级单调池
    ▼
路由分发 → Handler 执行
    │
    ▼
HttpResponse 构建
    │  使用请求级单调池
    ▼
TcpConnection 发送响应
    │  使用线程本地池的 PmrBuffer
    ▼
请求结束
    │  请求级单调池析构 → 整体释放
    ▼
等待下一个请求
```

---

## 5. 协程异步模型

### 5.1 设计选型

Hical 统一采用 `boost::asio::awaitable<T>` + `co_spawn` 作为协程模型，而非传统的回调或 Future/Promise。

**选型理由：**

| 方案                 | 优点                   | 缺点                   | Hical 选择 |
| -------------------- | ---------------------- | ---------------------- | ---------- |
| 回调                 | 无额外开销             | 回调地狱，代码可读性差 | 否         |
| Future/Promise       | 熟悉的 API             | 额外分配，不够高效     | 否         |
| `asio::awaitable<T>` | 零开销抽象，代码线性化 | 需 C++20 协程支持      | **是**     |

### 5.2 协程在框架中的应用

```cpp
// 路由处理器 — 用户编写的业务代码
RouteHandler = std::function<Awaitable<HttpResponse>(const HttpRequest&)>;

// 中间件 — 洋葱模型链式调用
MiddlewareHandler = std::function<Awaitable<HttpResponse>(const HttpRequest&, MiddlewareNext)>;

// WebSocket 消息回调
WsMessageCallback = std::function<Awaitable<void>(const std::string&, WebSocketSession&)>;

// 内部 — 连接接受循环
Awaitable<void> HttpServer::acceptLoop();

// 内部 — HTTP 会话处理
Awaitable<void> HttpServer::handleSession(tcp::socket socket);

// 内部 — WebSocket 会话处理
Awaitable<void> HttpServer::handleWebSocket(tcp::socket socket, ...);
```

### 5.3 协程执行流程

```
io_context.run()
    │
    ├── co_spawn(acceptLoop)
    │       │
    │       ├── co_await acceptor.async_accept()  ──→ 新连接
    │       │       │
    │       │       └── co_spawn(handleSession)
    │       │               │
    │       │               ├── co_await http::async_read()   ──→ 读取请求
    │       │               ├── middlewarePipeline.execute()   ──→ 中间件链
    │       │               │       ├── co_await next(req)    ──→ 洋葱模型
    │       │               │       └── router.dispatch(req)  ──→ 路由分发
    │       │               │               └── co_await handler(req)
    │       │               └── co_await http::async_write()  ──→ 发送响应
    │       │
    │       └── 循环接受下一个连接
    │
    └── io_context 调度所有协程
```

### 5.4 工具函数

```cpp
namespace hical
{
    template <typename T = void>
    using Awaitable = boost::asio::awaitable<T>;

    // 在当前协程上下文中休眠
    Awaitable<void> sleep(double seconds);
    Awaitable<void> sleep(std::chrono::duration duration);

    // 在指定 io_context 上休眠
    Awaitable<void> sleepFor(io_context& io, double seconds);

    // 启动协程
    void coSpawn(io_context& io, Awaitable<void> coro);
    void coSpawn(io_context& io, Awaitable<void> coro, CompletionHandler handler);
}
```

---

## 6. 路由系统设计

### 6.1 双策略路由

Router 采用**静态路由 + 参数路由**双策略，兼顾查找性能和功能灵活性：

```
请求到达: GET /api/users/42
    │
    ▼
┌─────────────────────────┐
│  静态路由查找 (O(1))      │
│  unordered_map<          │
│    {method, path},       │
│    handler               │
│  >                       │
│                          │
│  查找 {GET, "/api/users/42"} │
│  → 未命中                 │
└───────────┬─────────────┘
            │
            ▼
┌─────────────────────────┐
│  参数路由匹配 (O(n))      │
│  vector<ParamRouteEntry> │
│                          │
│  遍历:                    │
│  {GET, "/users/{id}"}    │
│  → 匹配! id = "42"       │
└───────────┬─────────────┘
            │
            ▼
┌─────────────────────────┐
│  WebSocket 路由检查       │
│  (仅在 HTTP 路由未命中时)  │
└─────────────────────────┘
```

### 6.2 静态路由实现

```cpp
struct RouteKey
{
    HttpMethod method;
    std::string path;
};

struct RouteKeyHash
{
    size_t operator()(const RouteKey& key) const
    {
        auto h1 = std::hash<int>{}(static_cast<int>(key.method));
        auto h2 = std::hash<std::string>{}(key.path);
        return h1 ^ (h2 << 1);  // XOR + 移位组合
    }
};

std::unordered_map<RouteKey, RouteHandler, RouteKeyHash> staticRoutes_;
```

- **O(1) 平均查找** — 哈希表直接定位
- **组合键** — `{method, path}` 作为一个键，同一路径不同方法可注册不同处理器

### 6.3 参数路由匹配

```cpp
// 零分配匹配：使用 string_view 解析路径段
static bool matchParamPath(
    std::string_view pattern,   // "/users/{id}/posts/{pid}"
    std::string_view path,      // "/users/42/posts/100"
    std::unordered_map<std::string, std::string>& params);
```

匹配过程：
1. 按 `/` 分割 pattern 和 path 为段
2. 逐段比较：普通段要求完全相等，`{name}` 段提取参数值
3. 使用 `string_view` 避免中间字符串分配

### 6.4 同步路由自动包装

```cpp
// 用户写同步处理器
router.get("/api/status", [](const HttpRequest&) -> HttpResponse {
    return HttpResponse::ok("ok");
});

// 框架内部自动包装为协程
void Router::route(HttpMethod method, const std::string& path, SyncRouteHandler handler)
{
    route(method, path, [handler = std::move(handler)](const HttpRequest& req)
                            -> Awaitable<HttpResponse> {
        co_return handler(req);
    });
}
```

### 6.5 HICAL_ROUTE 宏

```cpp
#define HICAL_ROUTE(router, method, path, handler) \
    (router).route(::hical::HttpMethod::h##method, path, handler)
```

这是 C++26 反射自动路由注册的降级方案。当反射可用时，将替换为编译期自动发现机制。

---

## 7. 中间件管道设计

### 7.1 洋葱模型

中间件采用经典的洋葱模型（Onion Model），每个中间件包裹下一层：

```
请求 →  ┌─ 中间件 1 (Logger) ─────────────────────────┐
        │                                              │
        │  请求 →  ┌─ 中间件 2 (Auth) ──────────────┐  │
        │          │                                 │  │
        │          │  请求 →  ┌─ 中间件 3 (CORS) ─┐ │  │
        │          │          │                    │ │  │
        │          │          │  路由处理器         │ │  │
        │          │          │  (最终处理)         │ │  │
        │          │          │                    │ │  │
        │          │  响应 ←  └────────────────────┘ │  │
        │          │                                 │  │
        │  响应 ←  └─────────────────────────────────┘  │
        │                                              │
响应 ←  └──────────────────────────────────────────────┘
```

### 7.2 管道构建

```cpp
class MiddlewarePipeline
{
    std::vector<MiddlewareHandler> middlewares_;

    Awaitable<HttpResponse> execute(const HttpRequest& req,
                                    RouteHandler finalHandler)
    {
        // 从最内层（finalHandler）开始，向外逐层包裹中间件
        MiddlewareNext current = [&finalHandler](const HttpRequest& r)
                                     -> Awaitable<HttpResponse> {
            co_return co_await finalHandler(r);
        };

        // 反向遍历：最后注册的中间件最接近 handler
        for (auto it = middlewares_.rbegin(); it != middlewares_.rend(); ++it)
        {
            auto& mw = *it;
            current = [&mw, next = std::move(current)](const HttpRequest& r)
                          -> Awaitable<HttpResponse> {
                co_return co_await mw(r, next);
            };
        }

        co_return co_await current(req);
    }
};
```

### 7.3 中间件能力

每个中间件可以：

1. **前置处理** — 在调用 `next(req)` 之前修改请求或执行逻辑
2. **拦截请求** — 不调用 `next(req)`，直接返回响应（如认证失败）
3. **后置处理** — 在 `next(req)` 返回后修改响应（如添加 CORS 头）
4. **异常处理** — 用 try/catch 包裹 `next(req)`，统一处理异常

---

## 8. SSL/TLS 模板化设计

### 8.1 模板化连接

Hical 使用模板而非虚函数来支持 TCP 和 SSL 两种连接模式：

```cpp
template <typename SocketType>
class GenericConnection : public TcpConnection,
                          public std::enable_shared_from_this<GenericConnection<SocketType>>
{
    SocketType socket_;

    // 编译期分支：SSL 连接需要额外的握手步骤
    Awaitable<void> sslHandshake()
    {
        if constexpr (IsSslStream<SocketType>)
        {
            co_await socket_.async_handshake(
                ssl::stream_base::server, use_awaitable);
        }
        // 普通 TCP 连接：此分支在编译期被消除，零开销
    }
};
```

### 8.2 类型别名

```cpp
// 普通 TCP 连接
using PlainConnection = GenericConnection<boost::asio::ip::tcp::socket>;

// SSL/TLS 连接
using SslConnection = GenericConnection<boost::asio::ssl::stream<tcp::socket>>;
```

### 8.3 SSL 类型检测

```cpp
// 编译期类型特征：检测是否为 SSL 流
template <typename T>
constexpr bool IsSslStream = false;

template <typename T>
constexpr bool IsSslStream<boost::asio::ssl::stream<T>> = true;
```

### 8.4 优势

| 对比项     | 虚函数方案         | 模板方案（Hical）         |
| ---------- | ------------------ | ------------------------- |
| 运行时开销 | 虚函数表间接调用   | 零开销，编译期内联        |
| 代码复用   | 需要两个独立实现类 | 一份模板代码              |
| SSL 分支   | 运行时 if 判断     | `if constexpr` 编译期消除 |
| 二进制大小 | 较小（共享实现）   | 较大（两份实例化）        |

---

## 9. C++ Concepts 后端抽象

### 9.1 四层 Concept 约束

```cpp
// 第一层：事件循环约束
template <typename T>
concept EventLoopLike = requires(T loop, std::function<void()> func, double delay) {
    { loop.run() } -> std::same_as<void>;
    { loop.stop() } -> std::same_as<void>;
    { loop.post(func) } -> std::same_as<void>;
    { loop.dispatch(func) } -> std::same_as<void>;
    { loop.runAfter(delay, func) } -> std::convertible_to<uint64_t>;
    { loop.runEvery(delay, func) } -> std::convertible_to<uint64_t>;
    { loop.allocator() } -> std::same_as<std::pmr::polymorphic_allocator<std::byte>>;
    // ...
};

// 第二层：TCP 连接约束
template <typename T>
concept TcpConnectionLike = requires(T conn, const char* data, size_t len) {
    { conn.send(data, len) } -> std::same_as<void>;
    { conn.shutdown() } -> std::same_as<void>;
    { conn.connected() } -> std::convertible_to<bool>;
    // ...
};

// 第三层：定时器约束
template <typename T>
concept TimerLike = requires(T timer) {
    { timer.cancel() } -> std::same_as<void>;
    { timer.isActive() } -> std::convertible_to<bool>;
    // ...
};

// 第四层：统一后端约束
template <typename T>
concept NetworkBackend = requires {
    typename T::EventLoopType;
    typename T::ConnectionType;
    typename T::TimerType;
} && EventLoopLike<typename T::EventLoopType>
  && TcpConnectionLike<typename T::ConnectionType>
  && TimerLike<typename T::TimerType>;
```

### 9.2 后端定义

```cpp
// 当前默认后端：Boost.Asio
struct AsioBackend
{
    using EventLoopType = AsioEventLoop;
    using ConnectionType = TcpConnection;  // PlainConnection
    using TimerType = AsioTimer;
};

// 未来可添加的轻量后端（示例）
struct IoUringBackend
{
    using EventLoopType = IoUringEventLoop;
    using ConnectionType = IoUringConnection;
    using TimerType = IoUringTimer;
};
```

### 9.3 使用方式

```cpp
template <NetworkBackend Backend>
class GenericServer
{
    using Loop = typename Backend::EventLoopType;
    using Conn = typename Backend::ConnectionType;

    Loop mainLoop_;
    // ...
};

// 使用 Asio 后端
GenericServer<AsioBackend> server;
```

---

## 10. 反射 API 包装层设计

### 10.1 设计目标

C++26 反射特性（`std::meta::info`）将使框架能够：

1. **自动发现** — 通过反射自动提取用户 Handler 类的成员函数签名
2. **自动包装** — 将 `co_await request.readBody()` 等高层调用映射到 Beast 底层异步操作
3. **自动注册** — 编译期生成路由分发表，零运行时开销

### 10.2 当前降级方案

由于 C++26 反射尚未被主流编译器完全支持，Hical 采用**宏 + Concepts**作为降级方案：

```cpp
// 降级方案：HICAL_ROUTE 宏手动注册
HICAL_ROUTE(router, Get, "/api/users", handler);

// 等价于
router.route(HttpMethod::hGet, "/api/users", handler);
```

### 10.3 未来反射方案（C++26）

当编译器支持成熟后，目标 API 形态：

```cpp
// 用户定义带标注的 Handler 结构体
struct UserHandler
{
    // 反射自动提取路由信息
    [[hical::route("/api/users", "GET")]]
    Awaitable<HttpResponse> listUsers(const HttpRequest& req)
    {
        co_return HttpResponse::json({{"users", "..."}});
    }

    [[hical::route("/api/users/{id}", "GET")]]
    Awaitable<HttpResponse> getUser(const HttpRequest& req)
    {
        co_return HttpResponse::json({{"id", req.param("id")}});
    }
};

// 框架通过反射自动注册所有路由
hical::meta::registerRoutes<UserHandler>(router);
```

### 10.4 反射驱动的请求封装

目标：让用户写高层业务代码，框架自动生成底层异步逻辑：

```cpp
// 用户写（未来）
co_await request.readJson<UserDTO>();

// 框架通过反射自动生成
// 1. co_await beast::http::async_read(stream, buffer, req)
// 2. 解析 JSON → 反射自动映射字段到 UserDTO 结构体
// 3. 使用 pmr 分配器分配 UserDTO
```

### 10.5 迁移路径

```
当前 (C++20)                  →  未来 (C++26)
─────────────────────────────────────────────
HICAL_ROUTE 宏手动注册          →  反射自动发现路由
SyncRouteHandler 手动定义       →  反射自动提取签名
手动 JSON 解析                  →  反射自动映射字段
运行时路由表                    →  编译期路由分发表
```

---

## 11. 连接管理与生命周期

### 11.1 连接状态机

```
                    ┌──────────────┐
                    │  hConnecting │
                    └──────┬───────┘
                           │ 连接建立 / SSL 握手完成
                           ▼
                    ┌──────────────┐
            ┌──────│  hConnected  │──────┐
            │      └──────┬───────┘      │
            │             │              │
      shutdown()     数据收发        close()
            │             │              │
            ▼             │              ▼
    ┌───────────────┐     │     ┌──────────────┐
    │hDisconnecting │     │     │hDisconnected │
    └───────┬───────┘     │     └──────────────┘
            │             │
            └─────────────┘
                  对端关闭
                    │
                    ▼
            ┌──────────────┐
            │hDisconnected │
            └──────────────┘
```

### 11.2 生命周期管理

GenericConnection 使用 `shared_from_this` 模式：

```cpp
template <typename SocketType>
class GenericConnection : public TcpConnection,
                          public std::enable_shared_from_this<...>
{
    // readLoop 持有 shared_ptr 引用
    Awaitable<void> readLoop()
    {
        auto self = shared_from_this();  // 保持连接存活
        while (state_ == State::hConnected)
        {
            auto bytesRead = co_await socket_.async_read_some(...);
            // ...
        }
    }
};
```

- **连接创建**：TcpServer::acceptLoop() 创建 `shared_ptr<GenericConnection>`
- **连接存活**：readLoop/writeLoop 协程持有 `shared_ptr`，保证连接不被提前释放
- **连接释放**：所有协程完成 → 引用计数归零 → 连接自动析构

### 11.3 写队列与 Scatter-Gather

```cpp
// 发送数据 → 加入写队列
void sendInLoop(const char* data, size_t len)
{
    writeQueue_.emplace_back(data, data + len);
    tryStartWrite();  // 尝试启动写循环
}

// 写循环：批量发送
Awaitable<void> writeLoop()
{
    // Scatter-Gather I/O：将队列中所有消息合并为一次系统调用
    std::vector<boost::asio::const_buffer> buffers;
    for (auto& msg : writeQueue_)
    {
        buffers.emplace_back(msg.data(), msg.size());
    }
    co_await boost::asio::async_write(socket_, buffers, use_awaitable);
}
```

- **队列化写入**：避免并发写入冲突
- **Scatter-Gather**：多条消息合并为一次 `async_write` 系统调用，减少系统调用次数
- **高水位标记**：队列超过 64MB 时触发回调，防止内存无限增长

---

## 12. 线程模型

### 12.1 1 Thread : 1 io_context

```
┌─────────────────────────────────────────────┐
│                 主线程                        │
│  ┌──────────────────────┐                    │
│  │  Main io_context      │                    │
│  │  - acceptLoop()       │ ← 接受新连接       │
│  │  - 分发到 IO 线程      │                    │
│  └──────────────────────┘                    │
├─────────────────────────────────────────────┤
│                IO 线程池                      │
│                                              │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐   │
│  │ Thread 1 │  │ Thread 2 │  │ Thread N │   │
│  │io_ctx #1 │  │io_ctx #2 │  │io_ctx #N │   │
│  │          │  │          │  │          │   │
│  │ conn A   │  │ conn C   │  │ conn E   │   │
│  │ conn B   │  │ conn D   │  │ conn F   │   │
│  └──────────┘  └──────────┘  └──────────┘   │
│                                              │
│  每个线程独立运行自己的 io_context              │
│  线程间无共享状态，无锁竞争                     │
└─────────────────────────────────────────────┘
```

### 12.2 Round-Robin 连接分发

```cpp
class EventLoopPool
{
    std::atomic<size_t> nextIndex_{0};

    AsioEventLoop* getNextLoop()
    {
        auto idx = nextIndex_.fetch_add(1, std::memory_order_relaxed);
        return loops_[idx % loops_.size()].get();
    }
};
```

新连接通过 Round-Robin 分配到 IO 线程，确保负载均衡。

### 12.3 线程安全策略

| 操作        | 线程安全保证                        |
| ----------- | ----------------------------------- |
| 连接读写    | 绑定到单个 IO 线程，无并发          |
| Timer 管理  | AsioEventLoop 内 mutex 保护         |
| 连接集合    | TcpServer 内 mutex 保护             |
| 内存池-全局 | `synchronized_pool_resource` 内部锁 |
| 内存池-线程 | `thread_local`，无需锁              |
| 统计计数    | `atomic` 操作                       |

---

## 13. 错误处理体系

### 13.1 统一错误码映射

Hical 将 Boost.Asio、系统错误、SSL 错误统一映射为框架内部错误码：

```
Boost.Asio 错误                hical 错误码
──────────────────         ──────────────────
asio::error::eof        →  ErrorCode::hEof
asio::error::timed_out  →  ErrorCode::hTimedOut
SSL handshake failure   →  ErrorCode::hSslHandshakeError
Windows WSAECONNRESET   →  ErrorCode::hConnectionReset
POSIX ECONNREFUSED      →  ErrorCode::hConnectionRefused
```

### 13.2 NetworkError 结构体

```cpp
struct NetworkError
{
    ErrorCode code;
    std::string message;

    operator bool() const;   // 是否有错误
    bool ok() const;         // 是否无错误
    bool isEof() const;      // 是否 EOF
    bool isCancelled() const; // 是否操作取消
};
```

提供语义化的错误检查方法，避免用户直接比较错误码数字。

---

## 14. 设计决策记录

| 决策        | 选择                                   | 理由                                                 |
| ----------- | -------------------------------------- | ---------------------------------------------------- |
| 协程模型    | `asio::awaitable<T>`                   | 与 Boost.Asio 原生集成，零额外开销，代码线性化       |
| HTTP 解析器 | Boost.Beast                            | 工业级成熟度，不重复造轮子                           |
| 内存管理    | C++17 PMR 三层池                       | 标准化接口，高并发低碎片，与 Boost.JSON 天然兼容     |
| SSL 实现    | 模板化 `GenericConnection<SocketType>` | 编译期分支消除，零运行时开销                         |
| 后端抽象    | C++20 Concepts                         | 编译期约束，不引入虚函数开销，面向未来可扩展         |
| 路由查找    | 哈希表 + 线性匹配                      | 静态路由 O(1)，参数路由灵活性                        |
| 中间件模型  | 洋葱模型                               | 前置/后置/拦截能力完整，Koa/Express 验证过的成熟模式 |
| 反射降级    | HICAL_ROUTE 宏                         | C++26 反射尚不成熟，宏方案保持向前兼容               |
| 线程模型    | 1 Thread : 1 io_context                | 线程间无共享状态，天然避免锁竞争                     |

---

> 更多信息：[API 文档](api_reference.md) | [快速上手](quickstart.md) | [性能报告](performance_report.md)
