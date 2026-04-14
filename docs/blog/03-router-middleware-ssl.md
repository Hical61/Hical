# 从零构建现代C++ Web服务器（三）：路由、中间件与 SSL

> **系列导航**：[第一篇：设计理念](01-design-philosophy.md) | [第二篇：协程与内存池](02-coroutine-and-memory.md) | [第三篇：路由、中间件与SSL](#)（本文） | [第四篇：实战与性能](04-practice-and-performance.md) | [第五篇：Cookie、Session与文件服务](05-cookies-sessions-fileservices.md)

## 前置知识

- 阅读过本系列前两篇（架构分层、协程基础、PMR 内存池）
- 了解 HTTP 请求/响应基本结构
- 了解中间件（Middleware）的概念（如 Express/Koa 的洋葱模型）

---

## 目录

- [1. 路由系统设计](#1-路由系统设计)
- [2. 洋葱模型中间件管道](#2-洋葱模型中间件管道)
- [3. 模板化 SSL：编译期零开销](#3-模板化-ssl编译期零开销)
- [4. WebSocket 集成](#4-websocket-集成)
- [5. 组装：HttpServer 门面](#5-组装httpserver-门面)
- [6. 总结](#6-总结)

---

## 1. 路由系统设计

### 1.1 双策略路由：快速与灵活的平衡

Web 框架的路由系统需要处理两类路径：

- **静态路由**：`/api/status`、`/api/users` — 路径固定，可以精确匹配
- **参数路由**：`/users/{id}`、`/posts/{pid}/comments/{cid}` — 路径含变量，需要模式匹配

hical 采用**双策略**设计：

```
请求到达: GET /api/users/42
    │
    ▼
┌─────────────────────────────┐
│  1. 静态路由查找 (O(1))       │
│  unordered_map 哈希表         │
│  查找 {GET, "/api/users/42"} │
│  → 未命中                     │
└───────────┬─────────────────┘
            │
            ▼
┌─────────────────────────────┐
│  2. 参数路由匹配 (线性扫描)    │
│  遍历 paramRoutes_:          │
│  {GET, "/users/{id}"}        │
│  → 匹配！提取 id = "42"      │
└───────────┬─────────────────┘
            │
            ▼
        执行 handler
```

为什么不统一用 Trie 树？因为绝大多数实际应用中，静态路由占比远大于参数路由。用哈希表处理静态路由是 O(1)，比 Trie 的 O(path_length) 更快。参数路由数量通常很少（几十个），线性扫描完全可以接受。

### 1.2 教学代码：从零写一个支持路径参数的 Router

```cpp
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

enum class HttpMethod { Get, Post, Put, Delete };

using RouteHandler = std::function<std::string(const std::string& path,
    const std::vector<std::pair<std::string, std::string>>& params)>;

class SimpleRouter
{
public:
    void route(HttpMethod method, const std::string& path, RouteHandler handler)
    {
        if (isParamRoute(path))
        {
            paramRoutes_.push_back({method, path, std::move(handler)});
        }
        else
        {
            staticRoutes_[{method, path}] = std::move(handler);
        }
    }

    std::string dispatch(HttpMethod method, const std::string& path)
    {
        // 1. 静态路由 O(1) 查找
        auto it = staticRoutes_.find({method, path});
        if (it != staticRoutes_.end())
        {
            return it->second(path, {});
        }

        // 2. 参数路由线性匹配
        for (const auto& entry : paramRoutes_)
        {
            if (entry.method != method) continue;

            std::vector<std::pair<std::string, std::string>> params;
            if (matchParamPath(entry.path, path, params))
            {
                return entry.handler(path, params);
            }
        }

        return "404 Not Found";
    }

private:
    // RouteKey：method + path 组合键
    struct RouteKey
    {
        HttpMethod method;
        std::string path;

        bool operator==(const RouteKey& other) const
        {
            return method == other.method && path == other.path;
        }
    };

    // 组合哈希：boost::hash_combine 风格
    struct RouteKeyHash
    {
        size_t operator()(const RouteKey& key) const
        {
            auto h1 = std::hash<int>{}(static_cast<int>(key.method));
            auto h2 = std::hash<std::string>{}(key.path);
            h1 ^= h2 + 0x9e3779b9 + (h1 << 6) + (h1 >> 2);
            return h1;
        }
    };

    std::unordered_map<RouteKey, RouteHandler, RouteKeyHash> staticRoutes_;

    struct ParamRouteEntry
    {
        HttpMethod method;
        std::string path;
        RouteHandler handler;
    };
    std::vector<ParamRouteEntry> paramRoutes_;

    static bool isParamRoute(const std::string& path)
    {
        return path.find('{') != std::string::npos;
    }

    // 零分配匹配：使用 string_view 逐段比较
    static bool matchParamPath(
        std::string_view pattern,
        std::string_view path,
        std::vector<std::pair<std::string, std::string>>& params)
    {
        // 跳过前导 '/'
        if (!pattern.empty() && pattern.front() == '/') pattern.remove_prefix(1);
        if (!path.empty() && path.front() == '/') path.remove_prefix(1);

        while (!pattern.empty() && !path.empty())
        {
            // 提取当前段
            auto pSlash = pattern.find('/');
            auto rSlash = path.find('/');
            auto patSeg = pattern.substr(0, pSlash);
            auto reqSeg = path.substr(0, rSlash);

            // 推进到下一段
            pattern = (pSlash == std::string_view::npos)
                ? std::string_view{} : pattern.substr(pSlash + 1);
            path = (rSlash == std::string_view::npos)
                ? std::string_view{} : path.substr(rSlash + 1);

            if (patSeg.size() >= 3 && patSeg.front() == '{' && patSeg.back() == '}')
            {
                // 参数段：{id} → 提取参数名和值
                auto paramName = patSeg.substr(1, patSeg.size() - 2);
                params.emplace_back(std::string(paramName), std::string(reqSeg));
            }
            else if (patSeg != reqSeg)
            {
                params.clear();
                return false;  // 静态段不匹配
            }
        }

        if (!pattern.empty() || !path.empty())
        {
            params.clear();
            return false;  // 段数不同
        }

        return true;
    }
};
```

### 1.3 hical 实际实现的额外安全措施

上面的教学代码展示了核心逻辑，hical 的实际实现还增加了多项安全防护：

```cpp
class Router
{
public:
    // 路径深度限制，防止超深路径 DoS
    static constexpr size_t hMaxPathSegments = 32;
    // 参数值长度限制
    static constexpr size_t hMaxParamValueLength = 1024;

    Awaitable<HttpResponse> dispatch(HttpRequest& req)
    {
        auto reqPath = urlDecode(req.path());  // URL 解码

        // 路径深度快速检查
        size_t segmentCount = 0;
        for (char c : reqPath)
            if (c == '/') ++segmentCount;
        if (segmentCount > hMaxPathSegments)
            co_return HttpResponse::badRequest("Path too deep");

        // ... 路由匹配逻辑 ...
    }

    // URL 解码：%20 → 空格，+ → 空格
    static std::string urlDecode(std::string_view encoded);
};
```

### 1.4 同步路由自动包装

hical 支持两种风格的处理器：

```cpp
// 风格 1：协程处理器（适合需要 co_await 的场景）
router.get("/api/data",
    [](const HttpRequest& req) -> Awaitable<HttpResponse> {
        auto data = co_await fetchFromDatabase();
        co_return HttpResponse::json(data);
    });

// 风格 2：同步处理器（简单场景更方便）
router.get("/api/status",
    [](const HttpRequest& req) -> HttpResponse {
        return HttpResponse::ok("running");
    });
```

同步处理器在注册时会被自动包装为协程：

```cpp
void Router::route(HttpMethod method, const std::string& path,
                   SyncRouteHandler handler)
{
    // 自动包装：同步 → 协程
    auto asyncHandler = [h = std::move(handler)](const HttpRequest& req)
        -> Awaitable<HttpResponse>
    {
        co_return h(req);  // 用 co_return 包装同步调用
    };
    route(method, path, std::move(asyncHandler));
}
```

---

## 2. 洋葱模型中间件管道

### 2.1 设计原理

中间件采用经典的洋葱模型（Onion Model）—— 请求从外到内穿过每层中间件，响应再从内到外穿回来：

```
请求 →  ┌─ 中间件 1 (Logger) ─────────────────────────┐
        │                                              │
        │  请求 →  ┌─ 中间件 2 (Auth) ──────────────┐  │
        │          │                                 │  │
        │          │  请求 →  ┌─ 路由处理器 ────────┐ │  │
        │          │          │                    │ │  │
        │          │          │  处理业务逻辑       │ │  │
        │          │          │                    │ │  │
        │          │  响应 ←  └────────────────────┘ │  │
        │          │                                 │  │
        │  响应 ←  └─────────────────────────────────┘  │
        │                                              │
响应 ←  └──────────────────────────────────────────────┘
```

每个中间件可以：
1. **前置处理**：在调用 `next(req)` 前执行（如记录请求日志）
2. **拦截请求**：不调用 `next(req)`，直接返回响应（如认证失败）
3. **后置处理**：在 `next(req)` 返回后修改响应（如添加 CORS 头）
4. **异常处理**：用 try/catch 包裹 `next(req)`

### 2.2 教学代码：从零实现 MiddlewarePipeline

```cpp
// 中间件的 next 回调类型
using MiddlewareNext = std::function<Awaitable<HttpResponse>(HttpRequest&)>;

// 中间件处理器类型
using MiddlewareHandler = std::function<
    Awaitable<HttpResponse>(HttpRequest&, MiddlewareNext)>;

class MiddlewarePipeline
{
public:
    // 添加中间件
    void use(MiddlewareHandler middleware)
    {
        middlewares_.push_back(std::move(middleware));
    }

    // 预构建调用链（只构建一次，所有请求复用）
    void build(MiddlewareNext finalHandler)
    {
        if (middlewares_.empty())
        {
            cachedChain_ = std::move(finalHandler);
            return;
        }

        // 从最内层向外构建链
        MiddlewareNext current = std::move(finalHandler);

        for (int i = static_cast<int>(middlewares_.size()) - 1; i >= 0; --i)
        {
            // 关键优化：按 const 引用捕获中间件
            const auto& mw = middlewares_[i];
            current = [&mw, next = std::move(current)](HttpRequest& r)
                -> Awaitable<HttpResponse>
            {
                co_return co_await mw(r, next);
            };
        }

        cachedChain_ = std::move(current);
    }

    // 执行管道
    Awaitable<HttpResponse> execute(HttpRequest& req, MiddlewareNext fallback)
    {
        if (cachedChain_)
        {
            co_return co_await cachedChain_(req);  // 使用预构建缓存
        }
        co_return co_await fallback(req);  // 无中间件时直接执行
    }

private:
    std::vector<MiddlewareHandler> middlewares_;
    MiddlewareNext cachedChain_;  // 预构建的调用链
};
```

### 2.3 两个关键优化

**优化 1：预构建链（build once, execute many）**

传统做法是每次请求都构建调用链：

```cpp
// 慢：每次请求重建
Awaitable<HttpResponse> execute(HttpRequest& req)
{
    MiddlewareNext current = finalHandler;
    for (auto it = middlewares_.rbegin(); ...) {
        current = [&mw, next = std::move(current)](...) { ... };
        // 每次构建都创建 std::function，触发堆分配
    }
    return current(req);
}
```

hical 在 `server.start()` 时调用 `build()` 一次性构建链，之后所有请求直接调用 `cachedChain_`：

```cpp
void HttpServer::start()
{
    // 启动时一次性构建
    middlewarePipeline_.build(
        [this](HttpRequest& req) -> Awaitable<HttpResponse> {
            co_return co_await router_.dispatch(req);
        });

    // ... 启动服务器
}
```

**优化 2：const 引用捕获避免堆分配**

```cpp
// 慢：值捕获 — 每次构建都拷贝 std::function，触发堆分配
current = [mw, next = std::move(current)](...) { ... };

// 快：const 引用捕获 — 零拷贝
const auto& mw = middlewares_[i];
current = [&mw, next = std::move(current)](...) { ... };
```

这是安全的，因为 `build()` 之后 `middlewares_` 不再修改（`started_` 标志保护），且 `MiddlewarePipeline` 的生命周期覆盖所有请求处理。

### 2.4 中间件使用示例

```cpp
// 日志中间件
server.use([](HttpRequest& req, MiddlewareNext next) -> Awaitable<HttpResponse> {
    std::cout << req.method() << " " << req.path() << std::endl;  // 前置
    auto res = co_await next(req);                                  // 继续
    std::cout << "  -> " << res.statusCode() << std::endl;          // 后置
    co_return res;
});

// 认证中间件（可拦截）
server.use([](HttpRequest& req, MiddlewareNext next) -> Awaitable<HttpResponse> {
    if (req.header("Authorization").empty())
    {
        co_return HttpResponse::unauthorized();  // 拦截，不调用 next
    }
    co_return co_await next(req);  // 认证通过，继续
});
```

---

## 3. 模板化 SSL：编译期零开销

### 3.1 问题：如何同时支持 TCP 和 SSL？

一个 Web 框架需要同时支持普通 TCP 连接和 SSL/TLS 加密连接。传统做法有两种：

**方案 A：运行时 if 分支**
```cpp
class Connection {
    bool isSsl_;
    tcp::socket plainSocket_;
    ssl::stream<tcp::socket> sslSocket_;

    void doRead() {
        if (isSsl_)
            sslSocket_.async_read_some(...);  // 运行时判断
        else
            plainSocket_.async_read_some(...);
    }
};
```
问题：每次 I/O 都有分支判断开销，且同一对象持有两种 socket（浪费内存）。

**方案 B：虚函数多态**
```cpp
class Connection { virtual void doRead() = 0; };
class PlainConnection : public Connection { ... };
class SslConnection : public Connection { ... };
```
问题：虚函数调用有间接跳转开销，热路径上的性能损失不可忽视。

**hical 的方案：模板 + constexpr if（编译期分支，零运行时开销）**

### 3.2 教学代码：模板化连接类

```cpp
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <type_traits>

namespace asio = boost::asio;
using tcp = asio::ip::tcp;

// 类型萃取：检测是否为 SSL 流
template <typename T>
struct IsSslStream : std::false_type {};

template <typename T>
struct IsSslStream<asio::ssl::stream<T>> : std::true_type {};

template <typename T>
inline constexpr bool isSslStream = IsSslStream<T>::value;

// 模板化连接：SocketType 决定 TCP 还是 SSL
template <typename SocketType>
class GenericConnection
{
public:
    GenericConnection(asio::io_context& io, SocketType socket)
        : socket_(std::move(socket)) {}

    // 编译期确定是否为 SSL 连接
    static constexpr bool isSsl() { return isSslStream<SocketType>; }

    // 获取最底层 socket（用于设置选项）
    auto& lowestLayerSocket()
    {
        if constexpr (isSslStream<SocketType>)
            return socket_.lowest_layer();  // SSL: 解包获取底层 tcp::socket
        else
            return socket_;                 // TCP: 直接返回
    }

    // 连接建立
    asio::awaitable<void> connectEstablished()
    {
        if constexpr (isSslStream<SocketType>)
        {
            // SSL 连接：自动执行 TLS 握手
            co_await socket_.async_handshake(
                asio::ssl::stream_base::server,
                asio::use_awaitable);
        }
        // 普通 TCP：此分支在编译期被完全消除
        co_return;
    }

    // 读取数据 — TCP 和 SSL 共享同一套逻辑
    asio::awaitable<size_t> read(char* buf, size_t len)
    {
        // async_read_some 对 tcp::socket 和 ssl::stream 都适用
        co_return co_await socket_.async_read_some(
            asio::buffer(buf, len), asio::use_awaitable);
    }

    // 关闭连接
    void shutdown()
    {
        if constexpr (isSslStream<SocketType>)
        {
            // SSL：先发送 TLS close_notify
            boost::system::error_code ec;
            // 注：实际代码中应用协程版本
            socket_.lowest_layer().shutdown(
                tcp::socket::shutdown_send, ec);
        }
        else
        {
            boost::system::error_code ec;
            socket_.shutdown(tcp::socket::shutdown_send, ec);
        }
    }

private:
    SocketType socket_;
};

// 类型别名 — 用户使用时不需要关心模板参数
using PlainConnection = GenericConnection<tcp::socket>;
using SslConnection = GenericConnection<asio::ssl::stream<tcp::socket>>;
```

### 3.3 constexpr if 的魔力

```cpp
if constexpr (isSslStream<SocketType>)
{
    // 这段代码只在 SocketType = ssl::stream<tcp::socket> 时编译
    co_await socket_.async_handshake(...);
}
// else 分支在 SSL 实例化时被编译器完全丢弃
```

**关键区别：**

| 语法                     | 时机   | 效果                               |
| ------------------------ | ------ | ---------------------------------- |
| `if (isSsl)`             | 运行时 | 两个分支都会被编译，运行时每次判断 |
| `if constexpr (isSsl())` | 编译期 | 只编译匹配的分支，另一个被丢弃     |

对于 `PlainConnection`（TCP），SSL 握手的代码**根本不存在于最终二进制文件中**。这就是"零开销抽象"。

### 3.4 hical 实际的读写循环

hical 的 `GenericConnection` 使用协程实现了完整的异步读写循环：

```cpp
// 读循环协程 — TCP 和 SSL 共享同一份代码
template <typename SocketType>
asio::awaitable<void> GenericConnection<SocketType>::readLoop()
{
    try
    {
        while (reading_ && state_ == State::hConnected)
        {
            // 零拷贝：直接读入 PmrBuffer 的可写区域
            inputBuffer_.ensureWritableBytes(4096);
            auto bytesRead = co_await socket_.async_read_some(
                asio::buffer(inputBuffer_.beginWrite(),
                             inputBuffer_.writableBytes()),
                asio::use_awaitable);

            bytesReceived_ += bytesRead;
            inputBuffer_.hasWritten(bytesRead);

            // 触发消息回调
            if (messageCallback_)
                messageCallback_(shared_from_this(), &inputBuffer_);
        }
    }
    catch (const boost::system::system_error& e)
    {
        if (e.code() != asio::error::operation_aborted)
            handleClose();
    }
}

// 写循环协程 — 支持 Scatter-Gather 批量写入
template <typename SocketType>
asio::awaitable<void> GenericConnection<SocketType>::writeLoop()
{
    while (true)
    {
        // 批量取出所有待发送消息
        std::deque<std::shared_ptr<std::string>> batch;
        {
            std::lock_guard lock(writeMutex_);
            if (writeQueue_.empty()) {
                writing_.store(false);
                co_return;
            }
            batch.swap(writeQueue_);
        }

        if (batch.size() == 1)
        {
            // 单条消息直接发送
            co_await asio::async_write(socket_,
                asio::buffer(*batch.front()), asio::use_awaitable);
        }
        else
        {
            // Scatter-Gather：多条消息合并为一次系统调用
            std::vector<asio::const_buffer> buffers;
            for (const auto& msg : batch)
                buffers.emplace_back(asio::buffer(*msg));

            co_await asio::async_write(socket_, buffers,
                asio::use_awaitable);
        }
    }
}
```

**Scatter-Gather I/O**：多条小消息不需要先拷贝到一个大 buffer 再发送。通过 `async_write(socket, vector<buffer>)`，操作系统可以一次系统调用发送多块不连续内存的数据。

---

## 4. WebSocket 集成

### 4.1 协程化 WebSocket 会话

hical 将 Boost.Beast 的 WebSocket 封装为简洁的协程接口：

```cpp
class WebSocketSession
{
public:
    using WsStream = beast::websocket::stream<tcp::socket>;

    explicit WebSocketSession(WsStream stream,
                              size_t maxMessageSize = 1024 * 1024)
        : stream_(std::move(stream)), open_(true)
    {
        stream_.read_message_max(maxMessageSize);
    }

    // 发送文本消息
    Awaitable<void> send(const std::string& msg)
    {
        if (!open_.load()) co_return;
        stream_.text(true);
        co_await stream_.async_write(
            asio::buffer(msg), asio::use_awaitable);
    }

    // 接收消息
    Awaitable<std::optional<std::string>> receive()
    {
        if (!open_.load()) co_return std::nullopt;

        beast::flat_buffer buffer;
        co_await stream_.async_read(buffer, asio::use_awaitable);

        co_return beast::buffers_to_string(buffer.data());
    }

    // 协程式安全关闭
    Awaitable<void> closeAsync()
    {
        // 原子 CAS：确保只关闭一次
        bool expected = true;
        if (!open_.compare_exchange_strong(expected, false))
            co_return;  // 已经关闭

        co_await stream_.async_close(
            beast::websocket::close_code::normal,
            asio::use_awaitable);
    }

    bool isOpen() const { return open_.load(); }

private:
    WsStream stream_;
    std::atomic<bool> open_{true};  // 原子标志防止并发关闭
};
```

### 4.2 原子 CAS 关闭协调

为什么需要原子标志？因为关闭可能从多个路径触发：

```
路径 1：客户端主动关闭 → receive() 抛出异常 → 需要清理
路径 2：服务端主动关闭 → 调用 closeAsync()
路径 3：空闲超时 → 定时器触发关闭
```

```cpp
// 无论哪个路径先到达，CAS 保证 close 只执行一次
bool expected = true;
if (!open_.compare_exchange_strong(expected, false))
    co_return;  // 另一个路径已经执行了关闭
```

### 4.3 WebSocket 路由注册

```cpp
// 在 Router 中注册 WebSocket 路由
server.router().ws("/ws/echo",
    // 消息回调
    [](const std::string& msg, WebSocketSession& ws) -> Awaitable<void> {
        co_await ws.send("Echo: " + msg);
    },
    // 连接建立回调（可选）
    [](WebSocketSession& ws) -> Awaitable<void> {
        co_await ws.send("Connected to hical WebSocket!");
    });
```

HttpServer 在 HTTP 解析阶段自动检测 WebSocket 升级请求：

```cpp
// handleSession 中的 WebSocket 升级检测
auto beastReq = parser.release();

if (beast::websocket::is_upgrade(beastReq))
{
    auto* wsRoute = router_.findWsRoute(reqPath);
    if (wsRoute)
    {
        co_await handleWebSocket(std::move(socket),
                                  std::move(beastReq), *wsRoute);
        co_return;  // 连接已升级，退出 HTTP 循环
    }
}
```

---

## 5. 组装：HttpServer 门面

### 5.1 设计哲学

`HttpServer` 是框架的**门面类**（Facade Pattern），将路由、中间件、网络层、SSL 等组件整合为一个简洁的 API：

```cpp
class HttpServer
{
public:
    explicit HttpServer(uint16_t port, size_t ioThreads = 1);

    // 路由注册
    Router& router();

    // 中间件注册
    void use(MiddlewareHandler middleware);

    // SSL 配置
    void enableSsl(const std::string& certFile, const std::string& keyFile);

    // 安全配置
    void setMaxBodySize(size_t bytes);       // 默认 1MB
    void setMaxHeaderSize(size_t bytes);     // 默认 8KB
    void setMaxConnections(size_t maxConns); // 默认 10000
    void setIdleTimeout(double seconds);     // 默认 60s

    // 启停
    void start();  // 阻塞
    void stop();
};
```

### 5.2 完整使用示例

```cpp
#include "core/HttpServer.h"

using namespace hical;

int main()
{
    HttpServer server(8080);

    // 中间件
    server.use([](HttpRequest& req, MiddlewareNext next)
        -> Awaitable<HttpResponse>
    {
        std::cout << httpMethodToString(req.method())
                  << " " << req.path() << std::endl;
        auto res = co_await next(req);
        std::cout << "  -> " << static_cast<int>(res.statusCode())
                  << std::endl;
        co_return res;
    });

    // 路由
    server.router().get("/", [](const HttpRequest&) -> HttpResponse {
        return HttpResponse::ok("Welcome to hical!");
    });

    server.router().get("/api/status", [](const HttpRequest&) -> HttpResponse {
        return HttpResponse::json({
            {"status", "running"},
            {"version", "0.2.0"}
        });
    });

    server.router().get("/users/{id}",
        [](const HttpRequest& req) -> HttpResponse
    {
        return HttpResponse::json({
            {"userId", req.param("id")},
            {"name", "User " + req.param("id")}
        });
    });

    // WebSocket
    server.router().ws("/ws/echo",
        [](const std::string& msg, WebSocketSession& ws) -> Awaitable<void> {
            co_await ws.send("Echo: " + msg);
        });

    server.start();  // 阻塞运行
}
```

### 5.3 start() 内部流程

```cpp
void HttpServer::start()
{
    started_ = true;  // 锁定配置，防止运行时修改

    // 1. 预构建中间件调用链
    middlewarePipeline_.build(
        [this](HttpRequest& req) -> Awaitable<HttpResponse> {
            co_return co_await router_.dispatch(req);
        });

    // 2. 创建监听 socket
    acceptor_ = std::make_unique<tcp::acceptor>(ioContext_);
    acceptor_->bind(tcp::endpoint(tcp::v4(), port_));
    acceptor_->listen();

    // 3. 启动 accept 协程
    coSpawn(ioContext_, acceptLoop());

    // 4. 多线程运行 io_context
    std::vector<std::thread> threads;
    for (size_t i = 1; i < ioThreads_; ++i)
        threads.emplace_back([this]() { ioContext_.run(); });

    ioContext_.run();  // 主线程也参与（阻塞）

    // 5. 等待所有线程结束
    for (auto& t : threads)
        if (t.joinable()) t.join();
}
```

**配置锁定**：`started_` 设为 `true` 后，调用 `router()` 或 `use()` 会抛出 `std::logic_error`。这防止了运行期修改路由/中间件导致的数据竞争。

---

## 6. 总结

本篇实现了框架的核心功能组件：

| 组件           | 设计                     | 关键技术                         |
| -------------- | ------------------------ | -------------------------------- |
| **路由系统**   | 静态 O(1) + 参数线性     | 组合哈希、string_view 零分配匹配 |
| **中间件管道** | 洋葱模型                 | 预构建链、const 引用捕获         |
| **SSL 支持**   | 模板化 GenericConnection | `if constexpr` 编译期分支消除    |
| **WebSocket**  | 协程化会话               | 原子 CAS 关闭协调                |
| **HttpServer** | 门面模式                 | 配置锁定、RAII 资源管理          |

### 核心要点

1. **双策略路由**是务实的选择 — 静态路由哈希表快，参数路由灵活，两者互补
2. **中间件预构建链**是性能关键 — 构建一次，避免每请求重复创建 std::function
3. **`if constexpr` 不是语法糖** — 它让编译器在编译期消除不需要的代码路径，实现真正的零开销
4. **配置锁定防竞态** — 简单的 `started_` 标志比复杂的读写锁更实用

### 下篇预告

在最后一篇中，我们将把所有组件串联为完整的应用：

1. **RESTful API 实战** — 从零构建用户管理 CRUD
2. **反射宏系统** — HICAL_JSON 自动序列化 + HICAL_ROUTES 自动路由注册
3. **性能调优** — IO 线程数、PMR 配置、连接参数
4. **安全加固** — 防 Slowloris、请求体限制、SSL 配置

---

> **hical** — 基于 C++26 的现代高性能 Web 框架 | [GitHub](https://github.com/user/hical)
