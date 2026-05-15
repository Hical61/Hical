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
| `EventLoopPool`                 | —               | 多线程池，Round-Robin 分发          |
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
├── HttpRequest / HttpResponse (自研 HTTP 栈封装)
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
// 写队列使用多态节点，支持内存数据和文件数据
std::deque<std::shared_ptr<WriteNode>> writeQueue_;

// 发送内存数据 → 加入写队列
void send(const char* data, size_t len)
{
    enqueueNode(std::make_shared<MemoryWriteNode>(
        std::make_shared<std::string>(data, len)));
}

// 发送文件数据 → 加入写队列
void sendFile(const std::filesystem::path& path, int64_t offset, int64_t length)
{
    enqueueNode(std::make_shared<FileWriteNode>(path, offset, length));
}

// 写循环：按节点类型分批处理
Awaitable<void> writeLoop()
{
    // MemoryWriteNode: Scatter-Gather I/O 批量发送
    std::vector<boost::asio::const_buffer> buffers;
    for (auto& node : memoryBatch)
    {
        buffers.emplace_back(static_cast<MemoryWriteNode&>(*node).buffer());
    }
    co_await boost::asio::async_write(socket_, buffers, use_awaitable);

    // FileWriteNode: 异步分块读取 + 发送（64KB 每块）
    co_await sendFileNode(fileNode);
}
```

- **队列化写入**：避免并发写入冲突
- **多态节点**：`MemoryWriteNode`（内存缓冲区）和 `FileWriteNode`（文件路径+偏移+长度）
- **Scatter-Gather**：连续的内存节点合并为一次 `async_write` 系统调用
- **异步文件发送**：`sendFileNode()` 使用 `boost::asio::random_access_file`（`BOOST_ASIO_HAS_FILE`）异步读取，无此特性时回退到 `std::ifstream`
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

| 决策                   | 选择                                   | 理由                                                           |
| ---------------------- | -------------------------------------- | -------------------------------------------------------------- |
| 协程模型               | `asio::awaitable<T>`                   | 与 Boost.Asio 原生集成，零额外开销，代码线性化                 |
| HTTP 解析器            | picohttpparser（自研栈）               | 轻量零拷贝，性能优于 Beast                                     |
| 内存管理               | C++17 PMR 三层池                       | 标准化接口，高并发低碎片，与 Boost.JSON 天然兼容               |
| SSL 实现               | 模板化 `GenericConnection<SocketType>` | 编译期分支消除，零运行时开销                                   |
| 后端抽象               | C++20 Concepts                         | 编译期约束，不引入虚函数开销，面向未来可扩展                   |
| 路由查找               | 哈希表 + 按方法分桶线性匹配            | 静态路由 O(1)，参数路由按方法子集匹配                          |
| 中间件模型             | 洋葱模型                               | 前置/后置/拦截能力完整，Koa/Express 验证过的成熟模式           |
| 反射降级               | HICAL_ROUTE 宏                         | C++26 反射尚不成熟，宏方案保持向前兼容                         |
| 线程模型               | 1 Thread : 1 io_context                | 线程间无共享状态，天然避免锁竞争                               |
| 连接池信号量           | `steady_timer` 作协程信号量            | 协程不能用 `condition_variable`，`timer.cancel()` 唤醒挂起协程 |
| 查询日志               | 装饰器模式                             | 透明拦截所有查询，不修改连接池和业务代码                       |
| PreparedStatement 缓存 | 每连接 LRU                             | 避免重复 prepare，非线程安全但每连接独占无锁开销               |
| DB 模块化              | 可选编译 `HICAL_WITH_DATABASE`         | 不影响核心库，零开销，后端可扩展                               |
| 日志系统               | `std::format` + 宏 + Sink 插件         | 零开销编译期消除，可插拔后端，不引入第三方日志库               |
| CORS 中间件            | 工厂函数 `makeCorsMiddleware`          | 一行启用，凭证模式安全校验，预检自动应答                       |
| 路由分组               | `RouteGroup` 值对象                    | 组级中间件局部生效，不影响全局中间件链                         |
| 日志异步写盘           | `AsyncFileSink` jthread 双缓冲         | 背压保护（丢弃 + 计数），不阻塞业务线程                        |

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
- **OpenApiDocument**：使用 `std::mutex` + `bool generated_` 标志实现惰性缓存，首次调用 `generate()` 时锁内生成，后续调用直接返回缓存，无锁开销。

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

> 更多信息：[API 文档](api_reference.md) | [快速上手](quickstart.md) | [性能报告](performance_report.md)
