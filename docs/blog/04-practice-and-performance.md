# 从零构建现代C++ Web服务器（四）：实战案例与性能调优

> **系列导航**：[第一篇：设计理念](01-design-philosophy.md) | [第二篇：协程与内存池](02-coroutine-and-memory.md) | [第三篇：路由、中间件与SSL](03-router-middleware-ssl.md) | [第四篇：实战与性能](#)（本文）

## 前置知识

- 阅读过本系列前三篇
- 了解 RESTful API 基本概念
- 了解 JSON 序列化/反序列化

---

## 目录

- [1. 完整案例：RESTful API 服务](#1-完整案例restful-api-服务)
- [2. 完整案例：WebSocket 实时通信](#2-完整案例websocket-实时通信)
- [3. 反射宏系统](#3-反射宏系统)
- [4. 性能调优实战](#4-性能调优实战)
- [5. 安全加固清单](#5-安全加固清单)
- [6. 错误处理体系](#6-错误处理体系)
- [7. 系列总结](#7-系列总结)

---

## 1. 完整案例：RESTful API 服务

### 1.1 从零构建用户管理 API

让我们用 hical 构建一个完整的用户管理 REST API，包含路由注册、中间件、JSON 请求/响应和路径参数。

```cpp
#include "core/HttpServer.h"
#include <boost/json.hpp>
#include <iostream>

using namespace hical;
namespace json = boost::json;

int main()
{
    HttpServer server(8080);

    // ============ 中间件 ============

    // 日志中间件：记录请求方法、路径和响应状态码
    server.use([](HttpRequest& req, MiddlewareNext next)
        -> Awaitable<HttpResponse>
    {
        auto start = std::chrono::steady_clock::now();
        std::cout << httpMethodToString(req.method())
                  << " " << req.path() << std::endl;

        auto res = co_await next(req);

        auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - start).count();
        std::cout << "  -> " << static_cast<int>(res.statusCode())
                  << " (" << elapsed << "us)" << std::endl;
        co_return res;
    });

    // 认证中间件（简化版）
    server.use([](HttpRequest& req, MiddlewareNext next)
        -> Awaitable<HttpResponse>
    {
        // 公开路由不需要认证
        if (req.path() == "/" || req.path() == "/api/status")
        {
            co_return co_await next(req);
        }

        auto authHeader = req.header("Authorization");
        if (authHeader.empty())
        {
            co_return HttpResponse::badRequest("Missing Authorization header");
        }

        co_return co_await next(req);
    });

    // ============ 路由 ============

    // GET / — 首页
    server.router().get("/",
        [](const HttpRequest&) -> HttpResponse
    {
        return HttpResponse::ok("User Management API v1.0");
    });

    // GET /api/status — 状态查询
    server.router().get("/api/status",
        [](const HttpRequest&) -> HttpResponse
    {
        return HttpResponse::json({
            {"status", "running"},
            {"version", "0.2.0"},
            {"framework", "hical"}
        });
    });

    // GET /api/users — 用户列表
    server.router().get("/api/users",
        [](const HttpRequest& req) -> HttpResponse
    {
        // 查询参数示例
        auto query = req.query();

        json::array users;
        users.push_back(json::object{
            {"id", 1}, {"name", "Alice"}, {"email", "alice@example.com"}});
        users.push_back(json::object{
            {"id", 2}, {"name", "Bob"}, {"email", "bob@example.com"}});

        return HttpResponse::json({{"users", users}, {"total", 2}});
    });

    // GET /users/{id} — 查询单个用户（路径参数）
    server.router().get("/users/{id}",
        [](const HttpRequest& req) -> HttpResponse
    {
        auto userId = req.param("id");
        return HttpResponse::json({
            {"id", userId},
            {"name", "User " + userId},
            {"email", userId + "@example.com"}
        });
    });

    // POST /api/users — 创建用户（JSON 请求体）
    server.router().post("/api/users",
        [](const HttpRequest& req) -> HttpResponse
    {
        try
        {
            auto body = req.jsonBody();
            auto& obj = body.as_object();

            // 提取字段
            auto name = std::string(obj.at("name").as_string());
            auto email = std::string(obj.at("email").as_string());

            return HttpResponse::json({
                {"message", "User created"},
                {"name", name},
                {"email", email}
            });
        }
        catch (const std::exception& e)
        {
            return HttpResponse::badRequest(
                std::string("Invalid JSON: ") + e.what());
        }
    });

    // PUT /users/{id} — 更新用户
    server.router().put("/users/{id}",
        [](const HttpRequest& req) -> HttpResponse
    {
        auto userId = req.param("id");
        auto body = req.jsonBody();

        return HttpResponse::json({
            {"message", "User " + userId + " updated"},
            {"data", body}
        });
    });

    // DELETE /users/{id} — 删除用户
    server.router().del("/users/{id}",
        [](const HttpRequest& req) -> HttpResponse
    {
        auto userId = req.param("id");
        return HttpResponse::json({
            {"message", "User " + userId + " deleted"}
        });
    });

    // ============ 启动 ============
    std::cout << "hical User API Server listening on :8080" << std::endl;
    server.start();
}
```

### 1.2 测试 API

```bash
# 状态查询
curl http://localhost:8080/api/status

# 用户列表
curl -H "Authorization: Bearer token" http://localhost:8080/api/users

# 查询单个用户
curl -H "Authorization: Bearer token" http://localhost:8080/users/42

# 创建用户
curl -X POST http://localhost:8080/api/users \
     -H "Authorization: Bearer token" \
     -H "Content-Type: application/json" \
     -d '{"name":"Charlie","email":"charlie@example.com"}'

# 删除用户
curl -X DELETE -H "Authorization: Bearer token" http://localhost:8080/users/42
```

---

## 2. 完整案例：WebSocket 实时通信

### 2.1 WebSocket Echo + 广播

```cpp
#include "core/HttpServer.h"
#include "core/WebSocket.h"
#include <iostream>
#include <mutex>
#include <set>

using namespace hical;

// 简单的连接管理器
struct ConnectionManager
{
    std::mutex mutex;
    std::set<WebSocketSession*> sessions;

    void add(WebSocketSession* s)
    {
        std::lock_guard lock(mutex);
        sessions.insert(s);
    }

    void remove(WebSocketSession* s)
    {
        std::lock_guard lock(mutex);
        sessions.erase(s);
    }
};

int main()
{
    HttpServer server(8080);
    ConnectionManager conns;

    // HTTP 路由
    server.router().get("/",
        [](const HttpRequest&) -> HttpResponse
    {
        return HttpResponse::ok("WebSocket Server - connect to /ws/echo or /ws/chat");
    });

    // WebSocket Echo
    server.router().ws("/ws/echo",
        [](const std::string& msg, WebSocketSession& ws) -> Awaitable<void>
    {
        co_await ws.send("Echo: " + msg);
    },
        [](WebSocketSession& ws) -> Awaitable<void>
    {
        co_await ws.send("Connected to echo service!");
    });

    // WebSocket Chat（广播）
    server.router().ws("/ws/chat",
        // 消息回调：广播给所有连接
        [&conns](const std::string& msg, WebSocketSession& ws) -> Awaitable<void>
    {
        std::lock_guard lock(conns.mutex);
        for (auto* session : conns.sessions)
        {
            if (session != &ws && session->isOpen())
            {
                co_await session->send(msg);
            }
        }
    },
        // 连接回调：注册到管理器
        [&conns](WebSocketSession& ws) -> Awaitable<void>
    {
        conns.add(&ws);
        co_await ws.send("Welcome to the chat room!");
    });

    std::cout << "WebSocket Server on :8080" << std::endl;
    server.start();
}
```

---

## 3. 反射宏系统

### 3.1 HICAL_JSON：自动 DTO 序列化

手动写 JSON 序列化代码很繁琐。hical 提供了 `HICAL_JSON` 宏，一行代码实现结构体到 JSON 的自动转换：

```cpp
#include "core/MetaJson.h"

// 定义 DTO 结构体
struct UserDTO
{
    std::string name;
    int age;
    std::string email;

    // 一行宏：自动生成 JSON 序列化/反序列化支持
    HICAL_JSON(UserDTO, name, age, email)
};

// 使用
UserDTO user{"Alice", 30, "alice@example.com"};

// 序列化：结构体 → JSON
boost::json::object json = hical::meta::toJson(user);
// 结果：{"name":"Alice","age":30,"email":"alice@example.com"}

// 反序列化：JSON → 结构体
auto parsed = hical::meta::fromJson<UserDTO>(jsonValue);
// parsed.name == "Alice", parsed.age == 30
```

### 3.2 HICAL_JSON 宏展开后的实际代码

`HICAL_JSON(UserDTO, name, age, email)` 展开为：

```cpp
static auto hicalJsonFields()
{
    return std::make_tuple(
        hical::meta::detail::makeField<UserDTO>("name", &UserDTO::name),
        hical::meta::detail::makeField<UserDTO>("age", &UserDTO::age),
        hical::meta::detail::makeField<UserDTO>("email", &UserDTO::email)
    );
}
```

每个 `FieldDescriptor` 包含**字段名（string_view）**和**成员指针**。序列化时遍历 tuple，用成员指针读取值；反序列化时用成员指针写入值。整个过程在编译期确定偏移量，运行时零反射开销。

### 3.3 HICAL_HANDLER / HICAL_ROUTES：自动路由注册

传统方式需要手动逐个注册路由：

```cpp
// 传统方式：手动注册，容易遗漏
router.get("/api/status", handler.getStatus);
router.get("/api/users", handler.listUsers);
router.get("/api/users/{id}", handler.getUser);
router.post("/api/users", handler.createUser);
```

hical 的反射宏方案：

```cpp
struct ApiHandler
{
    // 声明处理器并标注路由信息
    HttpResponse getStatus(const HttpRequest&)
    {
        StatusDTO status{"running", "0.2.0", "hical"};
        return HttpResponse::json(meta::toJson(status));
    }
    HICAL_HANDLER(Get, "/api/status", getStatus)

    HttpResponse listUsers(const HttpRequest&) { /* ... */ }
    HICAL_HANDLER(Get, "/api/users", listUsers)

    HttpResponse getUser(const HttpRequest& req) { /* ... */ }
    HICAL_HANDLER(Get, "/api/users/{id}", getUser)

    HttpResponse createUser(const HttpRequest& req)
    {
        // 自动反序列化请求体
        auto user = req.readJson<UserDTO>();
        return HttpResponse::json(meta::toJson(user));
    }
    HICAL_HANDLER(Post, "/api/users", createUser)

    // 收集所有路由 — 一行宏自动注册
    HICAL_ROUTES(ApiHandler, getStatus, listUsers, getUser, createUser)
};

// 使用
ApiHandler handler;
meta::registerRoutes(server.router(), handler);  // 一行注册全部路由
```

### 3.4 C++26 反射：双路线策略

hical 采用**双路线**设计：当编译器支持 C++26 反射（`__cpp_reflection >= 202306L`）时使用原生反射语法，否则回退到宏方案。两种路线对用户提供相同的 API：

```
回退路线 (C++20 宏)                当前/目标路线 (C++26 反射)
──────────────────────────────────────────────────
HICAL_JSON(Type, field1, field2)  →  自动反射所有字段，无需标注
HICAL_HANDLER(Get, "/path", fn)   →  [[hical::route("/path","GET")]] 属性
HICAL_ROUTES(Type, fn1, fn2)      →  自动发现所有标注了路由的方法
registerRoutes(router, handler)   →  registerRoutes<Handler>(router)
```

C++26 版本的序列化不需要任何宏标注：

```cpp
// C++26 反射版本 — 完全自动
template <typename T>
boost::json::object toJson(const T& obj)
{
    boost::json::object jsonObj;
    template for (constexpr auto member :
                  std::meta::nonstatic_data_members_of(^^T))
    {
        constexpr auto name = std::meta::identifier_of(member);
        jsonObj[name] = valueToJson(obj.[:member:]);
    }
    return jsonObj;
}
```

hical 已经在代码中通过 `#if HICAL_HAS_REFLECTION` 预埋了 C++26 分支，一旦编译器支持，可以无缝切换。

---

## 4. 性能调优实战

### 4.1 IO 线程数调优

```cpp
// 创建服务器时指定 IO 线程数
HttpServer server(8080, numThreads);
```

| 线程数       | 适用场景   | 说明                         |
| ------------ | ---------- | ---------------------------- |
| 1            | 开发/调试  | 单线程，便于调试，无并发问题 |
| CPU 核数     | CPU 密集型 | 每个核一个线程，充分利用多核 |
| CPU 核数 × 2 | I/O 密集型 | I/O 等待时切换，提高吞吐     |

经验法则：**先用 CPU 核数开始测试，再根据实际负载调整**。

### 4.2 PMR 池配置

```cpp
#include "core/MemoryPool.h"

hical::PoolConfig config;
config.globalMaxBlocksPerChunk = 128;       // 全局池每次申请块数
config.globalLargestPoolBlock = 1024 * 1024; // 全局池最大块 1MB
config.threadLocalMaxBlocksPerChunk = 64;    // 线程池每次申请块数
config.threadLocalLargestPoolBlock = 512 * 1024; // 线程池最大块 512KB
config.requestPoolInitialSize = 4096;        // 请求池初始 4KB

// 必须在 server.start() 之前调用
hical::MemoryPool::instance().configure(config);
```

调优建议：

| 参数                          | 增大效果             | 减小效果             |
| ----------------------------- | -------------------- | -------------------- |
| `requestPoolInitialSize`      | 减少大请求的扩容次数 | 减少小请求的内存浪费 |
| `threadLocalLargestPoolBlock` | 池化更大的分配       | 减少每线程内存占用   |
| `globalMaxBlocksPerChunk`     | 减少向系统申请频率   | 减少预分配内存       |

### 4.3 连接参数调优

```cpp
HttpServer server(8080, 4);

// 安全相关
server.setMaxBodySize(10 * 1024 * 1024);  // 请求体上限 10MB
server.setMaxHeaderSize(16 * 1024);        // 请求头上限 16KB
server.setMaxConnections(50000);           // 最大并发连接
server.setIdleTimeout(30.0);              // 空闲超时 30 秒
```

### 4.4 使用内置 Benchmark 工具

hical 附带了 HTTP 压测工具 `http_benchmark`：

```bash
# 编译
cmake --build build --target http_benchmark

# 运行压测
# 格式：http_benchmark <host> <port> <并发数> <每连接请求数> [path] [method]
./build/examples/http_benchmark localhost 8080 50 1000 /api/status GET
```

输出示例：

```
========== hical HTTP 基准测试 ==========
目标: localhost:8080/api/status
方法: GET
并发连接: 50
每连接请求: 1000
总请求数: 50000
=========================================
运行中...

========== 测试结果 ==========
  总请求数:    50000
  成功请求:    50000
  失败请求:    0
  总耗时:      1234.5 ms
  QPS:         40493 req/s

  延迟分布:
    平均:  1.23 ms
    P50:   0.89 ms
    P90:   2.10 ms
    P95:   3.45 ms
    P99:   8.12 ms
    最小:  0.12 ms
    最大:  15.67 ms
==============================
```

### 4.5 PMR Benchmark

```bash
# PMR 内存池性能测试
./build/examples/pmr_benchmark

# PMR 功能验证
./build/examples/pmr_poc
```

PMR benchmark 对比：

```
=== 单线程分配/释放 (1000000 次, 256 字节) ===
  new/delete:       45.2 ms
  pmr sync_pool:    28.1 ms
  pmr unsync_pool:  12.3 ms
  pmr monotonic:     3.8 ms
  hical threadLocal: 11.8 ms

=== 多线程并发分配 (8 线程, 500000 次/线程, 256 字节) ===
  new/delete:       89.3 ms
  hical threadLocal: 24.7 ms
    全局池分配次数: 32
    峰值分配:       1048576 bytes
```

---

## 5. 安全加固清单

### 5.1 连接层防护

```cpp
// 1. 并发连接数限制 — 防止资源耗尽
server.setMaxConnections(10000);

// 2. 空闲连接超时 — 防止 Slowloris 攻击
server.setIdleTimeout(60.0);

// 实现原理：每次读取前启动定时器
boost::asio::steady_timer deadline(socket.get_executor(),
    std::chrono::milliseconds(static_cast<int64_t>(idleTimeout_ * 1000)));
deadline.async_wait([&socket](const auto& ec) {
    if (!ec) socket.close();  // 超时关闭
});
co_await http::async_read(socket, buffer, parser, use_awaitable);
deadline.cancel();  // 数据到达，取消定时器
```

### 5.2 请求层防护

```cpp
// 3. 请求体大小限制 — 防止 OOM
server.setMaxBodySize(1024 * 1024);  // 1MB

// 4. 请求头大小限制
server.setMaxHeaderSize(8192);  // 8KB

// 实现原理：Beast parser 内置限制
http::request_parser<http::string_body> parser;
parser.body_limit(maxBodySize_);
parser.header_limit(maxHeaderSize_);
// 超限时 async_read 抛出 http::error::body_limit
```

### 5.3 路由层防护

```cpp
// 5. URL 路径深度限制 — 防止超深路径 DoS
static constexpr size_t hMaxPathSegments = 32;

// 6. 参数值长度限制 — 防止超长参数占用内存
static constexpr size_t hMaxParamValueLength = 1024;

// 7. URL 解码 — 防止编码绕过
auto decodedPath = Router::urlDecode(req.path());
```

### 5.4 SSL/TLS 配置

```cpp
// 启用 SSL
server.enableSsl("cert.pem", "key.pem");
```

---

## 6. 错误处理体系

### 6.1 跨平台错误码映射

hical 将 Boost.Asio、Windows、POSIX 错误码统一映射为框架内部错误码：

```cpp
enum class ErrorCode : uint32_t
{
    hNoError = 0,

    // 连接错误
    hEof,                  // 对端正常关闭
    hConnectionReset,      // 连接被重置
    hConnectionRefused,    // 连接被拒绝
    hTimedOut,             // 连接超时

    // 操作错误
    hOperationAborted,     // 操作被取消
    hBrokenPipe,           // 管道破裂
    hTooManyOpenFiles,     // 文件描述符不足

    // SSL 错误
    hSslHandshakeError,    // SSL 握手失败
    hSslInvalidCertificate, // SSL 证书无效

    hUnknown = 0xFFFF
};
```

### 6.2 映射逻辑

```cpp
ErrorCode fromBoostError(const boost::system::error_code& ec)
{
    // Boost.Asio 标准错误码
    if (ec == boost::asio::error::eof)
        return ErrorCode::hEof;
    if (ec == boost::asio::error::connection_reset)
        return ErrorCode::hConnectionReset;

    // 平台特定错误码
#ifdef _WIN32
    switch (ec.value()) {
        case WSAECONNRESET:   return ErrorCode::hConnectionReset;
        case WSAECONNREFUSED: return ErrorCode::hConnectionRefused;
        case WSAETIMEDOUT:    return ErrorCode::hTimedOut;
        // ...
    }
#else
    switch (ec.value()) {
        case ECONNRESET:   return ErrorCode::hConnectionReset;
        case ECONNREFUSED: return ErrorCode::hConnectionRefused;
        case ETIMEDOUT:    return ErrorCode::hTimedOut;
        // ...
    }
#endif

    return ErrorCode::hUnknown;
}
```

### 6.3 语义化错误检查

```cpp
// NetworkError 提供语义化方法
struct NetworkError
{
    ErrorCode code;
    std::string message;

    explicit operator bool() const { return code != ErrorCode::hNoError; }
    bool ok() const { return code == ErrorCode::hNoError; }
    bool isEof() const { return code == ErrorCode::hEof; }
    bool isCancelled() const { return code == ErrorCode::hOperationAborted; }
};

// 使用
auto err = toNetworkError(ec);
if (err.isEof())
{
    // 对端正常关闭，不需要记录错误
}
else if (err)
{
    std::cerr << "Error: " << err.message << std::endl;
}
```

---

## 7. 系列总结

四篇文章，我们从零构建了一个现代 C++ Web 服务器框架的核心模块：

### 知识图谱

```
第一篇：设计理念与架构
├── 两层架构（core 抽象 + asio 实现）
├── C++20 Concepts 后端抽象 + C++26 反射自动化
└── 1 Thread : 1 io_context 线程模型

第二篇：协程与内存池
├── co_await 替代回调地狱
├── Awaitable<T> 协程工具
└── PMR 三层池（全局同步 → 线程无锁 → 请求单调）

第三篇：路由、中间件与 SSL
├── 双策略路由（哈希 O(1) + 参数匹配）
├── 洋葱模型中间件（预构建链优化）
├── 模板化 SSL（if constexpr 零开销）
└── WebSocket 协程集成

第四篇：实战与性能
├── RESTful API 完整案例
├── HICAL_JSON / HICAL_ROUTES 反射宏
├── 性能调优（线程数、PMR 配置、连接参数）
└── 安全加固与错误处理
```

### 核心设计决策回顾

| 决策      | 选择                    | 理由                               |
| --------- | ----------------------- | ---------------------------------- |
| 协程模型  | `asio::awaitable<T>`    | 原生集成 Boost.Asio，零额外开销    |
| HTTP 解析 | Boost.Beast             | 工业级成熟度，不重复造轮子         |
| 内存管理  | C++17 PMR 三层池        | 标准化，高并发低碎片               |
| SSL 实现  | 模板化 `if constexpr`   | 编译期分支消除，零运行时开销       |
| 后端抽象  | C++20 Concepts          | 编译期约束，不引入 vtable 开销     |
| 路由查找  | 哈希表 + 线性匹配       | 静态路由 O(1)，参数路由灵活        |
| 中间件    | 洋葱模型 + 预构建       | 前置/后置/拦截完整，Koa 验证的模式 |
| 反射      | C++26 反射（双路线）    | 核心特性，宏方案为编译器适配的回退 |
| 线程模型  | 1:1 (Thread:io_context) | 线程间无共享，天然无锁             |

### 现代 C++ Web 框架的价值

hical 的性能优势主要体现在三个维度：

1. **内存管理**：三级 PMR 内存池使内存分配开销降至 O(1) 复杂度
2. **控制流优化**：协程模型避免了回调地狱，使代码逻辑更线性
3. **编译时优化**：C++26 反射实现路由自动注册，减少运行时开销

这个系列不仅仅是关于 hical 框架本身。更重要的是，它展示了现代 C++ 如何改变了服务器编程的体验：

- **协程让异步不再痛苦** — `co_await` 让异步代码像同步一样可读、可维护
- **PMR 让内存管理可控** — 三层池策略让内存分配行为可预测、可调优
- **Concepts 让模板更安全** — 编译期约束取代运行时断言，错误信息清晰可读
- **C++26 反射让样板代码消失** — 自动序列化、自动路由注册，编译期零开销

C++20/26 不是让 C++ 更复杂，而是让"写出好的 C++ 代码"变得更容易。

---

> **hical** — 基于 C++26 的现代高性能 Web 框架
>
> 如果这个系列对你有帮助，欢迎 Star 支持！
