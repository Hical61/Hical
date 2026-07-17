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
- [15. OpenAPI 元数据层设计](#15-openapi-元数据层设计)  
- [16. 日志系统设计](#16-日志系统设计)
- [17. HTTP 核心增强](#17-http-核心增强)
- [18. WebSocket 增强设计](#18-websocket-增强设计)
- [19. HTTP 热路径优化](#19-http-热路径优化)

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
│  │  Log       │ │   Cors     │ │ RouteGroup │            │
│  │  JwtAuth   │ │  ConfigLoader │ │  Session   │            │
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
├──────────────────────────────────────────────────────┤
│        DB 中间件层 (src/db/，可选 HICAL_WITH_DATABASE)  │
│  ┌──────────────────────────────────────────────────┐ │
│  │  DbMiddleware   DbQueryLog                        │ │
│  │  DbConnectionPool                                 │ │
│  │  DbConnection (纯虚)   MysqlConnection            │ │
│  │  StmtCache (LRU PreparedStatement)                │ │
│  └──────────────────────────────────────────────────┘ │
├──────────────────────────────────────────────────────┤
│    OpenAPI 元数据层 (src/core/OpenApi*.h/cpp，可选     │
│                     HICAL_WITH_OPENAPI，默认 ON)       │
│  ┌──────────────────────────────────────────────────┐ │
│  │  OpenApiSchema   OpenApiRegistry                  │ │
│  │  OpenApiDocument   OpenApiEndpoint                │ │
│  └──────────────────────────────────────────────────┘ │
├──────────────────────────────────────────────────────┤
│              底层库                                    │
│  Boost.Asio   Boost.JSON   OpenSSL                 │
│  Boost.MySQL                                          │
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

| 组件                           | 职责                                                       |
| ------------------------------ | ---------------------------------------------------------- |
| `HttpServer`                   | 顶层门面，整合路由+中间件+网络层，一键启动                 |
| `Router`                       | 路由注册与分发，静态路由 O(1) + 参数路由                   |
| `HttpRequest` / `HttpResponse` | 原生 HTTP 消息的 hical 风格封装                            |
| `Middleware`                   | 洋葱模型中间件管道                                         |
| `WebSocketSession`             | WebSocket 会话封装                                         |
| `Coroutine`                    | `Awaitable<T>` 别名和协程工具函数                          |
| `CORS`                         | `makeCorsMiddleware()`，W3C CORS 规范，Preflight 自动应答  |
| `RouteGroup`                   | 路由前缀分组 + 组级中间件继承 + 嵌套子组                   |
| `JwtAuth`                      | JWT HS256 签发/验证 + `makeJwtAuthMiddleware()`            |
| `ConfigLoader`                 | JSON 配置加载（层级 key + 环境变量覆盖）                   |
| `Log`                          | 6 级日志系统，`std::format` + 流式 + 条件 + 结构化字段 API |

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
| `EventLoopPool`                 | —               | 多线程池，Least-Connections 分发    |
| `TcpServer`                     | —               | Accept 循环 + 连接生命周期管理      |

### 3.3 数据库中间件层 (`src/db/`)

DB 层是**可选模块**，通过 CMake 选项 `HICAL_WITH_DATABASE=ON` 启用，不影响核心库的零开销原则。所有符号位于命名空间 `hical::db`。

#### 四层架构

```
HTTP 中间件层      DbMiddleware / DbQueryLog（洋葱模型集成，属性注入连接池）
      │
连接池层          DbConnectionPool（协程化信号量调度，max_connections 限流）
      │
抽象接口层        DbConnection（纯虚，后端可扩展）
      │
MySQL 后端层      MysqlConnection（Boost.MySQL async 实现）+ StmtCache（LRU 缓存）
```

#### 核心组件

| 组件               | 职责                                                   |
| ------------------ | ------------------------------------------------------ |
| `DbConfig`         | 连接参数（host/port/user/password/db/pool_size 等）    |
| `DbResult`         | 查询结果封装，行列迭代器，类型安全字段访问             |
| `DbConnection`     | 纯虚接口：`query()` / `execute()` / `ping()`           |
| `DbConnectionPool` | 协程安全连接池，`acquire()` 挂起等待，`release()` 唤醒 |
| `DbMiddleware`     | HTTP 中间件，将连接池引用注入 `HttpRequest` 属性       |
| `DbQueryLog`       | 装饰器模式，透明记录 SQL、耗时、错误                   |
| `MysqlConnection`  | Boost.MySQL 后端实现 `DbConnection`                    |
| `StmtCache`        | 每连接 LRU PreparedStatement 缓存，避免重复 prepare    |

### 3.4 组件依赖关系

```
HttpServer
├── Router (路由注册和分发)
│   └── RouteGroup (路由前缀分组)
├── MiddlewarePipeline (中间件管道)
│   └── Cors (makeCorsMiddleware)
├── HttpRequest / HttpResponse (原生 HTTP 栈封装)
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

Log 日志系统（独立基础设施）
├── Logger (单例，级别过滤，Sink 分发)
├── LogFormatter (TextFormatter / JsonFormatter)
├── LogSink (StderrSink / FileSink / AsyncFileSink / OStreamSink)
│   └── LogFile (文件轮转引擎)
│       └── AsyncFileSink (jthread 双缓冲)
├── LogChannel + LogChannelRegistry (命名通道)
├── LogMiddleware (trace-id + 结构化访问日志)
└── LogAdmin (动态级别管理端点)

DbMiddleware / DbQueryLog          [可选，HICAL_WITH_DATABASE=ON]
├── DbConnectionPool (协程化连接池)
│   ├── DbConnection (纯虚接口)
│   │   └── MysqlConnection (Boost.MySQL 后端)
│   │       └── StmtCache (LRU PreparedStatement 缓存)
│   └── steady_timer (协程信号量)
├── HttpRequest (属性注入)
└── Middleware (洋葱模型集成)
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
MiddlewareHandler = std::function<Awaitable<HttpResponse>(HttpRequest&, MiddlewareNext)>;

// WebSocket 消息回调
WsMessageCallback = std::function<Awaitable<void>(const std::string&, WebSocketSession&)>;

// 内部 — 连接接受循环（每个 worker loop 各一个）
Awaitable<void> HttpServer::acceptLoop(AsioEventLoop& loop);

// 内部 — HTTP 会话处理（HttpSessionImpl.cpp 编译防火墙）
Awaitable<void> handleSession(shared_ptr<GenericConnection<SocketType>> conn, ...);

// 内部 — WebSocket 会话处理
Awaitable<void> handleWebSocket(shared_ptr<GenericConnection<SocketType>> conn, ...);
```

### 5.3 协程执行流程

```
io_context.run()
    │
    ├── co_spawn(acceptLoop)          ← 每个 worker loop 各一个（SO_REUSEPORT）
    │       │
    │       ├── co_await acceptor.async_accept()  ──→ 新连接
    │       │       │
    │       │       └── co_spawn(handleSession)
    │       │               │
    │       │               ├── co_await socket.async_read_some()  ──→ 读取原始数据
    │       │               ├── picohttpparser 解析请求（零拷贝 string_view）
    │       │               ├── dispatchSync(req)                  ──→ 同步快速路径
    │       │               │   └── 有值？直接返回（零协程帧）
    │       │               ├── middlewarePipeline.execute()        ──→ 中间件链
    │       │               │       ├── co_await next(req)         ──→ 洋葱模型
    │       │               │       └── router.dispatch(req)       ──→ 路由分发
    │       │               │               └── co_await handler(req)
    │       │               └── co_await async_write(FixedBuffer)  ──→ 单次系统调用发送
    │       │
    │       └── 循环接受下一个连接（keep-alive 借还 readBuf，空闲不持有）
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

Router 采用**静态路由 + 参数路由**双策略，兼顾查找性能和功能灵活性。参数路由按 HTTP 方法分桶存储，仅扫描对应方法的路由子集：

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
│  参数路由匹配              │
│  按方法分桶:               │
│  unordered_map<HttpMethod,│
│    vector<ParamRouteEntry>│
│  >                        │
│                           │
│  查找 GET 桶:              │
│  {GET, "/users/{id}"}     │
│  → 匹配! id = "42"        │
└───────────┬──────────────┘
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
    std::vector<std::pair<std::string, std::string>>& params);
```

匹配过程：
1. 按 `/` 分割 pattern 和 path 为段
2. 逐段比较：普通段要求完全相等，`{name}` 段提取参数值
3. 使用 `string_view` 避免中间字符串分配

### 6.4 同步路由快速路径（零协程帧）

```cpp
// 用户写同步处理器
router.get("/api/status", [](const HttpRequest&) -> HttpResponse {
    return HttpResponse::ok("ok");
});

// 框架内部：同步 handler 只存 syncHandler，不创建 asyncHandler wrapper
struct RouteEntry
{
    RouteHandler asyncHandler;              // 协程处理器（可选）
    std::optional<SyncRouteHandler> syncHandler;  // 同步处理器（可选）
};

// dispatch 时优先走同步快速路径
std::optional<HttpResponse> Router::dispatchSync(HttpRequest& req)
{
    auto* entry = resolveRoute(req);
    if (entry && entry->syncHandler)
    {
        return (*entry->syncHandler)(req);  // 零协程帧，直接调用返回
    }
    return std::nullopt;  // 需要 fallback 到 co_await dispatch()
}

// HttpSessionImpl 主路径：
// 1. 先尝试 dispatchSync()，有值直接发送（零协程帧开销）
// 2. nullopt 时 fallback 到 co_await dispatch()（经中间件链）
```

同步快速路径节省约 40-130ns/req 的协程帧分配开销。`dispatch()` 内部也优先检查 `syncHandler`，有值时 `co_return syncHandler(req)` 跳过 `co_await asyncHandler(req)`。

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

    Awaitable<HttpResponse> execute(HttpRequest& req,
                                    RouteHandler finalHandler)
    {
        // 从最内层（finalHandler）开始，向外逐层包裹中间件
        MiddlewareNext current = [&finalHandler](HttpRequest& r)
                                     -> Awaitable<HttpResponse> {
            co_return co_await finalHandler(r);
        };

        // 反向遍历：最后注册的中间件最接近 handler
        for (auto it = middlewares_.rbegin(); it != middlewares_.rend(); ++it)
        {
            auto& mw = *it;
            current = [&mw, next = std::move(current)](HttpRequest& r)
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
2. **自动包装** — 将 `co_await request.readBody()` 等高层调用映射到底层异步操作
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
// 1. co_await async_read(stream, buffer, req)
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
// 写队列使用标签分发的 WriteEntry（去虚函数化快速路径）
struct WriteEntry
{
    enum class Type : uint8_t { hMemory, hNode };

    Type type;
    std::shared_ptr<std::string> memData; // hMemory 时有效（零虚函数开销）
    std::shared_ptr<WriteNode> node;      // hNode 时有效（PmrBuffer/File 慢路径）

    static WriteEntry fromMemory(std::shared_ptr<std::string> data);
    static WriteEntry fromNode(std::shared_ptr<WriteNode> n);
};

// Vyukov Intrusive MPSC Queue（无锁写队列）
// wait-free O(1) push，摊销 O(1) pop
struct MpscNode
{
    std::atomic<MpscNode*> next{nullptr};
};

struct MpscQueue
{
    std::atomic<MpscNode*> head_;  // producers push here (wait-free)
    MpscNode* tail_;               // consumer pops here (single-thread)
    MpscNode stub_;                // sentinel node

    void push(MpscNode* node);     // wait-free O(1)
    MpscNode* pop();               // amortized O(1), single consumer
};

// WriteEntry 继承 MpscNode，直接入队零额外分配
struct WriteEntry : MpscNode
{
    enum class Type : uint8_t { hMemory, hNode };
    Type type;
    std::shared_ptr<std::string> memData; // hMemory 时有效
    std::shared_ptr<WriteNode> node;      // hNode 时有效
};

MpscQueue writeQueue_;  // alignas(64) 消除 false sharing

// 发送内存数据 → 快速路径，直接存 shared_ptr<string>
void send(const char* data, size_t len)
{
    auto* entry = allocateNode(WriteEntry{...});  // thread_local 对象池，热路径零 malloc
    writeQueue_.push(entry);  // wait-free, 任意线程可调用
}

// 写循环：批量 drain + seq_cst 反饥饿 re-check
Awaitable<void> writeLoop()
{
    // 批量 drain MPSC 队列
    std::vector<WriteEntry*> batch;
    while (auto* node = writeQueue_.pop())
        batch.push_back(static_cast<WriteEntry*>(node));

    // Memory 快速路径: Scatter-Gather I/O 批量发送
    std::vector<boost::asio::const_buffer> buffers;
    for (auto* entry : batch)
    {
        if (entry->type == WriteEntry::Type::hMemory)
            buffers.emplace_back(entry->memData->data(), entry->memData->size());
    }
    co_await boost::asio::async_write(socket_, buffers, use_awaitable);

    // seq_cst 反饥饿 re-check：防止 drain 后遗漏并发 push
}
```

- **MPSC 无锁队列**：写队列从 `mutex` + `deque` 升级为 Vyukov Intrusive MPSC Queue，`enqueueEntry` 移除 `isInLoopThread` 分支和 `lock_guard`，任意线程 wait-free push
- **MpscNode 对象池**：thread_local free list（上限 128 节点），`allocateNode()` 从池中取内存做 placement new，`deallocateNode()` 析构后归还到池中。同线程 send + writeLoop 场景下分配/释放均为 O(1) 纯用户态操作，消除热路径 malloc/free
- **标签分发去虚函数化**：`WriteEntry::hMemory` 快速路径直接存 `shared_ptr<string>`，消除热路径上的虚函数调用开销；`hNode` 保留多态仅用于低频的文件/PmrBuffer 场景
- **批量 drain + 反饥饿**：`writeLoop` 一次性 drain 所有就绪节点，drain 后 `seq_cst` re-check 防止遗漏并发 push
- **Scatter-Gather**：连续的内存条目合并为一次 `async_write` 系统调用
- **异步文件发送**：`sendFileNode()` 使用 `boost::asio::random_access_file`（`BOOST_ASIO_HAS_FILE`）异步读取，无此特性时回退到 `std::ifstream`
- **`alignas(64)` 缓存行隔离**：写队列和相关原子变量做缓存行对齐，消除多线程 false sharing

### 11.4 空闲连接扫描（IdleScanner）

每个 io_context 一个 `IdleScanner` 实例，用侵入式双向链表 + 单一 `steady_timer` 替代 per-connection 的 timer 协程：

```
HttpServer::start()
  └─ 每个 io_context 创建 IdleScanner，coSpawn run()

handleSession()                          IdleScanner::run()
  ├─ Entry 在协程栈上（零堆分配）        ├─ 设 thread_local 指针
  ├─ Guard 构造 → registerEntry()        ├─ 循环: timer sleep → 遍历链表
  ├─ 请求处理中 Entry::touch()           │   超时的 → socket.close()
  └─ Guard 析构 → unregisterEntry()      └─ stop() 时 cancel timer 退出
```

- **零额外堆分配**：`Entry` 嵌在协程栈帧上，包含 `atomic<int64_t> lastActiveMs` + `socket*` + prev/next 链表指针
- **Guard 接受 nullptr**：`idleTimeout_ == 0` 时 `currentThreadIdleScanner()` 返回 nullptr，Guard 构造/析构均为 no-op
- **单线程无锁**：registerEntry/unregisterEntry 都在同一 io_context 线程内调用，双向链表操作无需任何锁
- **扫描间隔**：`max(1s, timeout/4)`，与 TcpServer::idleCheckLoop 策略一致
- **socket.close() 安全性**：scanner 和 handleSession 在同一 io_context 线程，close() 取消 pending 的 async_read，completion handler 在下一次 dispatch 时才执行，扫描循环提前保存的 `next` 指针在整个循环内有效

---

## 12. 线程模型

### 12.1 SO_REUSEPORT 多 Acceptor（1 Thread : 1 io_context）

每个 worker loop 拥有独立的 acceptor，accept 与 I/O 在同一线程完成，零跨线程调度：

```
┌─────────────────────────────────────────────────┐
│          SO_REUSEPORT 多 Acceptor 架构            │
│                                                  │
│  ┌──────────────┐ ┌──────────────┐ ┌──────────────┐
│  │  Worker #1   │ │  Worker #2   │ │  Worker #N   │
│  │  io_ctx #1   │ │  io_ctx #2   │ │  io_ctx #N   │
│  │              │ │              │ │              │
│  │  acceptor #1 │ │  acceptor #2 │ │  acceptor #N │
│  │  ↓ accept    │ │  ↓ accept    │ │  ↓ accept    │
│  │  conn A      │ │  conn C      │ │  conn E      │
│  │  conn B      │ │  conn D      │ │  conn F      │
│  └──────────────┘ └──────────────┘ └──────────────┘
│                                                  │
│  内核通过 SO_REUSEPORT 在多 acceptor 间负载均衡    │
│  每个连接从 accept 到 I/O 全生命周期在同一线程      │
│  线程间无共享状态，无跨线程 dispatch               │
│                                                  │
│  Windows 自动回退为单 acceptor + Round-Robin 分发  │
└─────────────────────────────────────────────────┘
```

### 12.2 EventLoopPool 与连接分发

```cpp
class EventLoopPool
{
    AsioEventLoop* getNextLoop()
    {
        // Least-Connections：选当前连接数最少的 loop
        AsioEventLoop* best = loops_[0].get();
        size_t minCount = best->connectionCount();
        for (size_t i = 1; i < loops_.size(); ++i)
        {
            size_t count = loops_[i]->connectionCount();
            if (count < minCount)
            {
                minCount = count;
                best = loops_[i].get();
            }
        }
        return best;
    }
};
```

- `AsioEventLoop` 维护 `atomic<size_t> connectionCount_`（relaxed 序），`TcpServer::addConnection()`/`removeConnection()` 负责增减
- **Linux / macOS**：SO_REUSEPORT 模式下，每个 worker loop 运行独立 `acceptLoop()`，内核自动在多个 acceptor 间做负载均衡，`EventLoopPool::getNextLoop()` 不参与 accept 分发。
- **Windows**：无 SO_REUSEPORT，回退为单 acceptor + Least-Connections 分发到 worker loop。

### 12.2.1 Worker 线程 CPU 亲和性（Linux）

`EventLoopPool::start()` 启动 worker 线程时，使用 `pthread_setaffinity_np` 将第 i 个线程绑定到第 `i % hardware_concurrency()` 个 CPU 核心：

```cpp
// Linux only — Windows 侧 (void)i 静默忽略
cpu_set_t cpuset;
CPU_ZERO(&cpuset);
CPU_SET(i % std::thread::hardware_concurrency(), &cpuset);
pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
```

**绑核的收益**：
- 消除内核随机迁移线程导致的 TLB flush（10K 连接场景下每次迁移约 2-5μs 惩罚）
- 减少跨核 IPI（Inter-Processor Interrupt），配合 RPS/RFS 让收包 softirq 和处理线程在同一核心
- L1/L2 cache 命中率提升，worker 线程的热数据不会被其他核心的线程替换掉

### 12.3 线程安全策略

| 操作        | 线程安全保证                                  |
| ----------- | --------------------------------------------- |
| 连接读写    | 绑定到单个 IO 线程，无并发                    |
| Timer 管理  | AsioEventLoop 内 mutex 保护                   |
| 连接集合    | TcpServer per-loop `LoopShard` 分片，无全局锁 |
| 内存池-全局 | `synchronized_pool_resource` 内部锁           |
| 内存池-线程 | `thread_local`，无需锁                        |
| 统计计数    | `atomic` 操作                                 |

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

| 决策                   | 选择                                             | 理由                                                           |
| ---------------------- | ------------------------------------------------ | -------------------------------------------------------------- |
| 协程模型               | `asio::awaitable<T>`                             | 与 Boost.Asio 原生集成，零额外开销，代码线性化                 |
| HTTP 解析器            | picohttpparser（原生栈集成）                     | 轻量零拷贝，性能优于 Beast                                     |
| 内存管理               | C++17 PMR 三层池                                 | 标准化接口，高并发低碎片，与 Boost.JSON 天然兼容               |
| SSL 实现               | 模板化 `GenericConnection<SocketType>`           | 编译期分支消除，零运行时开销                                   |
| 后端抽象               | C++20 Concepts                                   | 编译期约束，不引入虚函数开销，面向未来可扩展                   |
| 路由查找               | 哈希表 + 按方法分桶线性匹配                      | 静态路由 O(1)，参数路由按方法子集匹配                          |
| 中间件模型             | 洋葱模型                                         | 前置/后置/拦截能力完整，Koa/Express 验证过的成熟模式           |
| 反射降级               | HICAL_ROUTE 宏                                   | C++26 反射尚不成熟，宏方案保持向前兼容                         |
| 线程模型               | 1 Thread : 1 io_context                          | 线程间无共享状态，天然避免锁竞争                               |
| 连接池信号量           | `steady_timer` 作协程信号量                      | 协程不能用 `condition_variable`，`timer.cancel()` 唤醒挂起协程 |
| 查询日志               | 装饰器模式                                       | 透明拦截所有查询，不修改连接池和业务代码                       |
| PreparedStatement 缓存 | 每连接 LRU                                       | 避免重复 prepare，非线程安全但每连接独占无锁开销               |
| DB 模块化              | 可选编译 `HICAL_WITH_DATABASE`                   | 不影响核心库，零开销，后端可扩展                               |
| 日志系统               | `std::format` + 宏 + Sink 插件                   | 零开销编译期消除，可插拔后端，不引入第三方日志库               |
| CORS 中间件            | 工厂函数 `makeCorsMiddleware`                    | 一行启用，凭证模式安全校验，预检自动应答                       |
| 路由分组               | `RouteGroup` 值对象                              | 组级中间件局部生效，不影响全局中间件链                         |
| 日志异步写盘           | `AsyncFileSink` jthread 双缓冲                   | 背压保护（丢弃 + 计数），不阻塞业务线程                        |
| 写队列                 | Vyukov Intrusive MPSC Queue                      | wait-free push，消除写路径 mutex 竞争，摊销 O(1) pop           |
| 连接表分片             | per-loop `LoopShard`                             | idle 扫描/增删全程无锁，消除 TcpServer 全局 mutex              |
| PMR requestPool        | `threadLocalPool` 作为 upstream                  | 扩容零锁竞争，避免 globalPool 同步开销                         |
| JWT 认证               | HMAC-SHA256 + SyncBeforeHandler                  | OpenSSL EVP 自实现，零第三方 JWT 库依赖                        |
| JSON 配置              | `ConfigLoader` + 环境变量覆盖                    | 层级 key 访问，env 覆写优先，支持多种 C++ 类型                 |
| DTO 校验               | 编译期装饰器（MIN/MAX/PATTERN/NOT_EMPTY/LENGTH） | 反序列化时自动校验，描述性错误信息，零运行时开销               |

---

## 15. OpenAPI 元数据层设计

### 15.1 设计目标

| 目标                | 具体措施                                                                  |
| ------------------- | ------------------------------------------------------------------------- |
| **自动化**          | 从 `HICAL_JSON` 宏的 FieldDescriptor tuple 直接推导 JSON Schema，无需手写 |
| **类型安全**        | 全程使用 C++20 模板，编译期检测字段类型，不依赖运行时字符串拼接           |
| **实时同步**        | 路由注册与 API 标注同步完成，文档与代码一一对应，不会漂移                 |
| **Swagger UI 集成** | 一行 `serveOpenApi()` 暴露 `/openapi.json` + `/docs`，CDN 加载 Swagger UI |

### 15.2 四层架构

```
┌──────────────────────────────────────────────────────────┐
│              用户业务代码                                   │
│  HICAL_JSON(UserDTO, name, age)                          │
│  HICAL_API(summary("获取用户"), ...)                      │
│  HICAL_ROUTES_WITH_API(MyHandler, getUser, createUser)   │
├──────────────────────────────────────────────────────────┤
│  第一层：Schema 生成 (OpenApiSchema.h)                     │
│  jsonSchema<T>()  ──→  boost::json::object               │
│  collectSchemas<T>()  ──→  递归收集嵌套类型 schema        │
│  HICAL_SCHEMA_NAME 宏  ──→  注册 $ref 引用名              │
├──────────────────────────────────────────────────────────┤
│  第二层：路由元数据注册表 (OpenApiRegistry.h/cpp)           │
│  RouteApiInfo { summary, tags, requestBody, responses }  │
│  OpenApiRegistry  ──→  mutex 保护，快照返回               │
│  HICAL_API() 宏 + builder::* 辅助函数                    │
├──────────────────────────────────────────────────────────┤
│  第三层：文档组装 (OpenApiDocument.h/cpp)                  │
│  OpenApiDocument { config, registry, schemaMap }         │
│  惰性生成 + mutex 缓存完整 JSON 文档                       │
│  自动提取 {param} → path parameter                       │
│  同路径不同 method 合并为同一 Path Item                    │
├──────────────────────────────────────────────────────────┤
│  第四层：端点暴露 (OpenApiEndpoint.h)                      │
│  serveOpenApi(router, doc)                               │
│  GET /openapi.json  ──→  完整 OpenAPI 3.0 JSON spec      │
│  GET /docs          ──→  Swagger UI CDN HTML 页面         │
└──────────────────────────────────────────────────────────┘
```

### 15.3 关键组件

| 组件              | 文件                    | 关键 API / 宏                                                              |
| ----------------- | ----------------------- | -------------------------------------------------------------------------- |
| `OpenApiSchema`   | `OpenApiSchema.h`       | `jsonSchema<T>()`、`collectSchemas<T>()`、`HICAL_SCHEMA_NAME`              |
| `OpenApiRegistry` | `OpenApiRegistry.h/cpp` | `RouteApiInfo`、`HICAL_API()`、`builder::*`、`registerRoutesWithOpenApi()` |
| `OpenApiDocument` | `OpenApiDocument.h/cpp` | `OpenApiDocument`、`OpenApiConfig`、`generate()`                           |
| `OpenApiEndpoint` | `OpenApiEndpoint.h`     | `serveOpenApi(router, doc)`                                                |

### 15.4 线程安全说明

- **OpenApiRegistry**：内部使用 `std::mutex`，`getAll()` 以快照方式返回 `vector<RouteApiInfo>` 副本，避免长时间持锁。
- **OpenApiDocument**：使用 `std::mutex` + `bool generated_` 标志实现惰性缓存，首次调用 `generateString()` 时锁内生成并缓存，后续调用按值返回缓存副本。

### 15.5 CMake 编译开关

| 变量                 | 默认值 | 说明                                                            |
| -------------------- | ------ | --------------------------------------------------------------- |
| `HICAL_WITH_OPENAPI` | `ON`   | 是否编译 OpenAPI 模块                                           |
| `HICAL_HAS_OPENAPI`  | —      | 由 CMake 根据 `HICAL_WITH_OPENAPI` 自动定义，用于 `#ifdef` 保护 |

```bash
# 关闭 OpenAPI 模块（极端体积敏感场景）
cmake -B build -DHICAL_WITH_OPENAPI=OFF
```

---

## 16. 日志系统设计

### 16.1 设计目标

| 目标            | 具体措施                                                                 |
| --------------- | ------------------------------------------------------------------------ |
| **零开销**      | NDEBUG 下 TRACE 级编译期消除，`thread_local` 缓存消除锁竞争              |
| **多 API 风格** | `std::format` 格式化 / 流式 `<<` / 条件宏 / 结构化字段，适配不同使用场景 |
| **可插拔后端**  | `LogSink` 抽象接口，支持 stderr / 文件 / 异步文件 / ostream 等后端       |
| **生产级特性**  | 文件轮转、异步双缓冲、背压保护、命名通道、动态级别调整                   |

### 16.2 分层架构

```
┌──────────────────────────────────────────────────────────┐
│              用户代码                                       │
│  HICAL_LOG_INFO("port={}", 8080)                         │
│  HICAL_LOG_TO("access", Info, "...")                     │
├──────────────────────────────────────────────────────────┤
│  第一层：宏 API (Log.h)                                    │
│  HICAL_LOG_* 宏 → Logger::instance().log(LogRecord)      │
│  级别过滤（编译期 TRACE 消除 + 运行时级别检查）             │
├──────────────────────────────────────────────────────────┤
│  第二层：格式化 (LogFormatter.h/cpp)                       │
│  TextFormatter（人类可读）/ JsonFormatter（结构化）         │
│  thread_local 时间戳缓存（秒级复用）                       │
├──────────────────────────────────────────────────────────┤
│  第三层：输出后端 (LogSink.h/cpp)                          │
│  StderrSink (fprintf) / FileSink (同步 fwrite + 轮转)    │
│  AsyncFileSink (jthread 双缓冲) / OStreamSink (兼容桥接)  │
├──────────────────────────────────────────────────────────┤
│  第四层：通道路由 (LogChannel.h/cpp)                       │
│  LogChannel（独立级别/格式化器/后端）                      │
│  LogChannelRegistry（shared_mutex 读多写少）               │
├──────────────────────────────────────────────────────────┤
│  第五层：HTTP 集成                                         │
│  LogMiddleware（trace-id 自动注入 + 结构化访问日志）       │
│  LogAdmin（GET/PUT /admin/log-level 动态调整）            │
└──────────────────────────────────────────────────────────┘
```

### 16.3 关键组件

| 组件            | 文件                  | 关键设计                                                              |
| --------------- | --------------------- | --------------------------------------------------------------------- |
| `Logger`        | `Log.h/cpp`           | 单例，Sink 列表分发，`thread_local` 线程 ID + 时间戳缓存              |
| `LogRecord`     | `LogRecord.h`         | 结构化条目，level/timestamp/threadId/file/line/message/fields/traceId |
| `TextFormatter` | `LogFormatter.h/cpp`  | `[2026-05-01 12:00:00.123] [INFO] [tid:1234] message` 格式            |
| `JsonFormatter` | `LogFormatter.h/cpp`  | JSON Lines 格式，UTC 时间戳，`boost::json::serialize`                 |
| `StderrSink`    | `LogSink.h/cpp`       | `fprintf(stderr, ...)` 直接输出                                       |
| `FileSink`      | `LogSink.h/cpp`       | 同步 `fwrite` + `LogFile` 轮转引擎                                    |
| `AsyncFileSink` | `AsyncFileSink.h/cpp` | `std::jthread` + 4MB 前后缓冲交换 + 背压保护                          |
| `LogFile`       | `LogFile.h/cpp`       | 按大小轮转（默认 100MB）、时间戳序列命名、严格文件名匹配清理          |
| `FixedBuffer`   | `FixedBuffer.h`       | 4KB 栈缓冲 + `std::to_chars` 格式化 + 堆 fallback                     |
| `LogChannel`    | `LogChannel.h/cpp`    | 命名通道，独立级别/格式化器/Sink 列表                                 |
| `LogMiddleware` | `LogMiddleware.h/cpp` | OpenSSL RAND_bytes 128 位 trace-id，洋葱模型                          |
| `LogAdmin`      | `LogAdmin.h/cpp`      | GET/PUT 端点，运行时级别调整                                          |

### 16.4 线程安全

- **Logger**：Sink 列表修改需外部同步（启动时配置），日志写入时 Sink 列表只读
- **LogChannel**：`shared_mutex` 读写锁，读取路径共享锁
- **AsyncFileSink**：`condition_variable_any` + `stop_token` 安全停止
- **LogFile**：非线程安全（由 FileSink/AsyncFileSink 保证单线程访问）

---

## 17. HTTP 核心增强

### 17.1 CORS 中间件

`makeCorsMiddleware(CorsOptions)` 工厂函数创建符合 W3C 规范的 CORS 中间件：
- **Preflight 自动应答**：OPTIONS 请求直接返回 204，不经过路由层
- **Vary: Origin**：非通配符模式添加 `Vary: Origin` 缓存提示
- **凭证安全**：`allowCredentials=true` 时禁止 `"*"` 通配符（浏览器安全要求）

### 17.2 路由组

`Router::group(prefix)` 创建 `RouteGroup`，支持：
- **前缀拼接**：组内路由自动添加前缀（如 `/api/v1` + `/users` → `/api/v1/users`）
- **组级中间件**：`RouteGroup::use()` 添加仅对组内路由生效的中间件
- **嵌套子组**：`RouteGroup::group()` 创建子组，继承父组的前缀和中间件

### 17.3 查询参数与表单参数

`HttpRequest` 新增 URL 编码参数解析：
- `queryParam(name)` / `queryParams()` — 查询字符串参数（`?key=value`）
- `formParam(name)` / `formParams()` — 表单体参数（`application/x-www-form-urlencoded`）
- 惰性解析 + 缓存，首次访问时解析，后续直接返回缓存

### 17.4 重定向响应

`HttpResponse::redirect(location, code)` 工厂方法，默认 302 Found。Location 头经 `setHeader()` 内部 CRLF 注入防护。

### 17.5 全局错误处理器

`HttpServer::setErrorHandler(handler)` 设置未捕获异常的统一处理函数：

```cpp
server.setErrorHandler([](const std::exception& e, const HttpRequest& req) {
    return HttpResponse::json({{"error", e.what()}, {"path", std::string(req.path())}});
});
```

---

## 18. WebSocket 增强设计

### 18.1 自研 WebSocket 栈（RFC 6455 完整实现）

Hical 使用完全自研的 WebSocket 实现（不依赖 Beast），核心模块：

| 组件               | 文件              | 职责                                                                |
| ------------------ | ----------------- | ------------------------------------------------------------------- |
| `WsFrame`          | `WsFrame.h`       | 帧解析/构造：data/control 帧统一、客户端掩码强制、RSV 位验证        |
| `WsHandshake`      | `WsHandshake.h`   | 握手协议：`Sec-WebSocket-Key`/`Accept` 计算、扩展/子协议协商        |
| `WsDeflate`        | `WsDeflate.h/cpp` | permessage-deflate 压缩（RFC 7692）、pimpl 封装 zlib、zip bomb 防护 |
| `WebSocketSession` | `WebSocket.h/cpp` | 会话管理：发送/接收/心跳/子协议/上下文存储                          |
| `WsHub`            | `WsHub.h/cpp`     | 连接管理器：房间/广播/单播，线程安全                                |

### 18.2 WsOptions 配置

```cpp
struct WsOptions
{
    // CSWSH 防护
    std::unordered_set<std::string> allowedOrigins;

    // permessage-deflate 压缩
    bool enableCompression = false;
    int serverMaxWindowBits = 15;
    int clientMaxWindowBits = 15;
    bool serverNoContextTakeover = false;

    // 心跳保活
    std::chrono::seconds pingInterval {0};   // 0 = 禁用
    uint32_t maxMissedPongs = 3;
    std::string pingPayload;                  // 最大 125 字节

    // 子协议协商
    std::vector<std::string> subprotocols;
};
```

### 18.3 WebSocketSession 关键 API

| 方法                       | 返回值                             | 说明                                |
| -------------------------- | ---------------------------------- | ----------------------------------- |
| `send(msg)`                | `Awaitable<void>`                  | 发送文本帧                          |
| `sendBinary(data)`         | `Awaitable<void>`                  | 发送二进制帧                        |
| `receive()`                | `Awaitable<std::string>`           | 接收文本消息（向后兼容）            |
| `receiveMessage()`         | `Awaitable<optional<WsMessage>>`   | 接收 typed 消息（区分 Text/Binary） |
| `sendPing(payload)`        | `Awaitable<void>`                  | 手动发送 Ping                       |
| `closeAsync(code, reason)` | `Awaitable<void>`                  | 优雅关闭（RFC 6455 Close 帧）       |
| `setContext<T>(ptr)`       | `void`                             | 设置 per-connection 类型化上下文    |
| `getContext<T>()`          | `shared_ptr<T>`                    | 获取上下文                          |
| `subprotocol()`            | `std::string_view`                 | 协商后的子协议                      |
| `lastPongTime()`           | `chrono::steady_clock::time_point` | 最后一次收到 Pong 的时间            |

### 18.4 WsHub 广播管理器

线程安全的连接注册与广播中心，适用于聊天室、实时推送等多连接场景：

```
┌────────────────────────────────────────────────────┐
│                     WsHub                           │
│                                                    │
│  m_connections: map<WsConnectionId, weak_ptr>      │
│  m_rooms: map<string, vector<RoomMember>>          │
│                                                    │
│  add(session) → id        remove(id)               │
│  join(id, room)           leave(id, room)          │
│  broadcast(room, msg)     broadcastBinary(room, d) │
│  broadcastAll(msg)        sendTo(id, msg)          │
│  roomSize(room)           connectionCount()        │
│                                                    │
│  内部使用 shared_mutex，广播通过 coSpawn 到各连接   │
│  所属 executor 实现跨线程安全写入                    │
│  存储 weak_ptr 不延长 session 生命周期              │
└────────────────────────────────────────────────────┘
```

**设计要点**：
- **weak_ptr 存储**：Hub 不延长 WebSocketSession 生命周期，连接断开后自然失效
- **coSpawn 跨线程广播**：每条广播消息通过 `coSpawn` 投递到目标连接所属的 executor，保证写入线程安全
- **RoomMember 缓存行优化**：冗余存储 `weak_ptr` 消除广播时的 `m_connections.find(id)` 指针追踪
- **用户需在 onDisconnect 中调用 `remove(id)`**：Hub 不自动清理，dead entries 在广播时通过 `weak_ptr::lock()` 跳过

### 18.5 消息类型回调

```cpp
// 传统文本回调（向后兼容）
using WsMessageCallback = std::function<Awaitable<void>(const std::string&, WebSocketSession&)>;

// 类型化回调（区分 Text/Binary），优先于 onMessage
using WsTypedMessageCallback = std::function<Awaitable<void>(const WsMessage&, WebSocketSession&)>;

struct WsMessage
{
    WsOpcode type = WsOpcode::hText;  // hText 或 hBinary
    std::string data;
};
```

`WsTypedMessageCallback` 存储在 `WsRoute::onTypedMessage` 字段中，运行时优先于传统 `onMessage` 调用。当前通过内部设置（非公开 `ws()` 重载），用户如需区分二进制消息可使用 `receiveMessage()` 循环。

---

## 19. HTTP 热路径优化

### 19.1 零拷贝请求解析

```
for(;;) 循环顶部
     ├── 有 pipeline 残留 → 直接借 8KB readBuf（ReadBufferPool）
     └── 空闲等待请求   → 256B 栈缓冲做 speculative read
                            async_read_some 走 Asio 投机路径，零 epoll_ctl(MOD)
                            │ 有数据 → acquire(8KB) + memcpy 栈→堆
                            └ 无数据 → 挂起等 epoll 通知（EPOLLIN 已注册，无 MOD）

        readBuf（借来的 8KB 缓冲区，idle 时不持有）
              │
              ▼
        picohttpparser (栈上 phr_header[64]，零堆分配)
              │
              ▼
        NativeRequest (string_view 引用 readBuf，零拷贝)
              │
              ▼
        HttpRequest 封装 (惰性解析：jsonBody/queryParam/cookie 首次访问才解析)
              │
              ▼ 响应写完毕
        readBuf 归还回池（响应写完 + pipeline 残留暂存后立即 release，不等协程析构）；
        空闲连接仅 +256B 栈缓冲（coroutine frame 内，非堆内存）
```

### 19.2 单缓冲区响应序列化 + 前缀模板 + 乐观同步写

```
HttpResponse (handler 设置 Content-Type / body)
    │
    ▼
NativeResponse::serializeHeadTo(FixedBuffer<512>, prefix, prefixLen)
    │  状态行 + 用户头部 + 预构建通用头部（一次 memcpy ~90B）
    ▼
head + body 合并为单次 buffer（≤512 时）
    │
    ├── tryOptimisticWrite(socket, buffer)  ← 同步写，写完直接返回
    │       │  成功 → co_return（零协程挂起、零完成队列）
    │       └  失败 → async_write            ← would_block/partial 回退
    ▼
一次 writev 系统调用
```

**乐观同步写**：socket 已经 `non_blocking(true)`，小响应（head+body ≤ 512）的 `write_some` 在 localhost/内网下近乎必中。写完直接 `co_return`——不挂协程、不进 reactor 完成队列，高并发下大幅降低完成队列排队延迟。`would_block` / partial / 硬错误时回退 `async_write`，只多了几十纳秒检查开销。

**`socket.non_blocking(true)` 是硬前提**：Asio 的 `sync_send` 只检查 `user_set_non_blocking` 标志位（不认 `internal_non_blocking`）。如果 fd 被恢复为阻塞模式，`write_some` 在 EAGAIN 时掉进 `poll_write(s, 0, -1)` 无限等 POLLOUT，直接卡死 io 线程。`handleSession` 入口调一次 `socket.non_blocking(true)` 设上 `user_set` 标志后，后续异步操作怎么折腾也清不掉它。 

**连接级响应前缀模板**：`handleSession` 在 `for(;;)` 循环外预拼 `Server` / `Connection` / `Date` 三个通用头部的 wire bytes 到 `responsePrefix[128]`。keep-alive 连接中每个请求只需：

1. 检查 `time_t` 是否变化（vDSO 快速路径）
2. 过期时 `memcpy(29B)` 更新 Date 值
3. 序列化时追加整段前缀到 FixedBuffer（替代 3 次 `HeaderMap::insert` + for 循环逐条格式化）

`Connection: close` 路径（连接最后一个请求）回退到原始 insert 路径，不影响通用头部正确性。

### 19.3 HTTP Date 头 `thread_local` 缓存

RFC 7231 要求有 Date 响应头。Hical 使用 `thread_local` 每秒更新一次格式化后的 Date 字符串，热路径零格式化开销：

```cpp
thread_local DateCache dateTlsCache; // {cachedSec, buf[30], len}
// 每次响应：秒数未变 → 直接返回 string_view{buf, len}
```

### 19.4 HTTP Pipelining 优化

- **parse-before-read 快速路径**：若 `pipelineSpill` 中有上一请求的粘包残留，先把数据拷入新借的 readBuf 并尝试解析，解析成功则跳过本轮 `async_read_some`
- **延迟 memmove**：只在 readBuf 前半空间耗尽时才 memmove 紧凑数据，而非每次请求后立即移动
- **ReadBufferPool 借还**：每请求借一块缓冲区，响应写完后归还；粘包残留暂存在 `pipelineSpill`（`for(;;)` 外的 `std::string`，大多数连接为空，SSO 不分配堆内存）
- **栈缓冲 speculative read**：空闲连接不用 `async_wait(wait_read)` 等着（有 MOD 开销），改用 256B 栈数组 `async_read_some`。Asio 投机路径下有数据直返、无数据挂起，都不产生 `epoll_ctl(MOD)`。实测 10K 并发下 epoll_ctl 调用从 32,563 降到 9,173（降 71.8%），总 CPU 降 ~16-20%

### 19.5 同步中间件零协程帧 + 异步转发帧消除

`buildOptimizedChain()` 算法将连续的 `SyncBeforeHandler` / `SyncAfterHandler` 合并为单个协程帧：

```
10 层同步中间件 = 1 次协程帧堆分配（而非 10 次）
性能：仅比无中间件低 2.1%
```

异步中间件包装 lambda 原来用 `co_return co_await mw(r, next)`——lambda 自己是协程，每层多一个独立堆帧。改为 `return mw(r, next)` 后 lambda 退化为普通函数，mw10 场景从 20 帧/请求（10 用户帧 + 10 包装帧）降到 10 帧。

### 19.6 TcpCorkGuard 文件响应合并小包

`writeFileResponse()` 先发头部（~200B）再分块发文件（64KB/块），TCP_NODELAY 下头部会立即作为独立小 TCP 段发出。`TcpCorkGuard` RAII 守卫在函数入口 cork socket，让内核缓存数据直到 uncork：

```
TcpCorkGuard 构造 → setsockopt(TCP_CORK, 1)
    │
    ├── async_write(头部 ~200B)     ← 内核暂不发
    ├── async_write(chunk 64KB)    ← 合并为 ~64.2KB TCP 段
    │
TcpCorkGuard 析构 → setsockopt(TCP_CORK, 0) → flush
```

跨平台适配：Linux `TCP_CORK`，macOS `TCP_NOPUSH`，Windows no-op（已有应用层 scatter-gather）。

---

> 更多信息：[API 文档](api_reference.md) | [快速上手](quickstart_cn.md) | [性能报告](performance_report.md)
