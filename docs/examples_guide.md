# Hical 使用示例与快速上手指南

> 从零开始，逐步构建完整的 hical Web 服务

---

## 目录

- [快速开始](#快速开始)
- [示例 1：最小 HTTP 服务器](#示例-1最小-http-服务器)
- [示例 2：RESTful API 服务](#示例-2restful-api-服务)
- [示例 3：中间件实战](#示例-3中间件实战)
- [示例 4：WebSocket 实时通信](#示例-4websocket-实时通信)
- [示例 5：SSL/TLS 安全服务](#示例-5ssltls-安全服务)
- [示例 6：协程异步处理](#示例-6协程异步处理)
- [示例 7：PMR 内存池使用](#示例-7pmr-内存池使用)
- [示例 8：完整应用示例](#示例-8完整应用示例)
- [示例 9：数据库中间件](#示例-9数据库中间件)
- [示例 10：OpenAPI 文档自动生成](#示例-10openapi-文档自动生成)
- [示例 11：CORS 与路由分组](#示例-11cors-与路由分组)
- [示例 12：查询参数与表单参数](#示例-12查询参数与表单参数)
- [示例 13：日志系统](#示例-13日志系统)
- [运行内置示例程序](#运行内置示例程序)
- [常见问题](#常见问题)

---

## 快速开始

### 环境准备

**最低要求：**

| 组件       | 版本                                             |
| ---------- | ------------------------------------------------ |
| C++ 编译器 | GCC 14+ / Clang 20+ / MSVC 2022+（需支持 C++20） |
| CMake      | 3.20+                                            |
| Boost      | 1.82+（Asio、JSON）；DB 中间件 >= 1.85              |
| OpenSSL    | 3.0+                                             |

**MSYS2 MINGW64 快速安装：**

```bash
pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-cmake mingw-w64-x86_64-ninja \
          mingw-w64-x86_64-boost mingw-w64-x86_64-openssl mingw-w64-x86_64-gtest
```

**Ubuntu 24.04 快速安装：**

```bash
sudo apt install g++-14 cmake ninja-build libboost-all-dev libssl-dev libgtest-dev
```

### 编译 Hical

```bash
git clone https://github.com/your-repo/hical.git
cd hical

# Linux / macOS
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)

# Windows (MSYS2 MINGW64)
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

### 运行测试验证

```bash
ctest --test-dir build --output-on-failure --timeout 60 -j4
```

---

## 示例 1：最小 HTTP 服务器

最简单的 hical 服务器，3 行核心代码：

```cpp
#include "core/HttpServer.h"

using namespace hical;

int main()
{
    HttpServer server(8080);

    server.router().get("/", [](const HttpRequest&) -> HttpResponse {
        return HttpResponse::ok("Hello, hical!");
    });

    server.start();  // 阻塞运行
    return 0;
}
```

**测试：**

```bash
curl http://localhost:8080/
# 输出: Hello, hical!
```

**要点：**
- `HttpServer(port)` 创建服务器实例
- `router().get(path, handler)` 注册 GET 路由
- 处理器返回 `HttpResponse`，使用工厂方法构建响应
- `start()` 阻塞当前线程，开始监听和处理请求

---

## 示例 2：RESTful API 服务

构建一个简单的用户管理 API：

```cpp
#include "core/HttpServer.h"
#include <iostream>

using namespace hical;

int main()
{
    HttpServer server(8080);
    auto& router = server.router();

    // GET /api/users — 获取用户列表
    router.get("/api/users", [](const HttpRequest&) -> HttpResponse {
        return HttpResponse::json({
            {"users", {
                {{"id", 1}, {"name", "Alice"}},
                {{"id", 2}, {"name", "Bob"}}
            }}
        });
    });

    // GET /api/users/{id} — 获取单个用户（路径参数）
    router.get("/api/users/{id}", [](const HttpRequest& req) -> HttpResponse {
        auto userId = req.param("id");
        if (userId.empty())
        {
            return HttpResponse::badRequest("缺少用户 ID");
        }
        return HttpResponse::json({
            {"id", userId},
            {"name", "User " + userId}
        });
    });

    // POST /api/users — 创建用户（读取 JSON 请求体）
    router.post("/api/users", [](const HttpRequest& req) -> HttpResponse {
        try
        {
            auto json = req.jsonBody();
            auto name = json.at("name").as_string();

            HttpResponse res;
            res.setStatus(HttpStatusCode::hCreated);
            res.setJsonBody({
                {"message", "用户创建成功"},
                {"name", std::string(name)}
            });
            return res;
        }
        catch (const std::exception& e)
        {
            return HttpResponse::badRequest("无效的 JSON: " + std::string(e.what()));
        }
    });

    // PUT /api/users/{id} — 更新用户
    router.put("/api/users/{id}", [](const HttpRequest& req) -> HttpResponse {
        auto userId = req.param("id");
        return HttpResponse::json({
            {"message", "用户 " + userId + " 已更新"},
            {"body", req.body()}
        });
    });

    // DELETE /api/users/{id} — 删除用户
    router.del("/api/users/{id}", [](const HttpRequest& req) -> HttpResponse {
        auto userId = req.param("id");
        return HttpResponse::json({
            {"message", "用户 " + userId + " 已删除"}
        });
    });

    // GET /api/search — 查询参数示例
    router.get("/api/search", [](const HttpRequest& req) -> HttpResponse {
        auto query = req.query();  // "?keyword=hello&page=1" → "keyword=hello&page=1"
        return HttpResponse::json({
            {"query", query},
            {"results", boost::json::array{}}
        });
    });

    std::cout << "RESTful API 服务运行在 http://localhost:8080" << std::endl;
    server.start();
    return 0;
}
```

**测试：**

```bash
# 获取用户列表
curl http://localhost:8080/api/users

# 获取单个用户
curl http://localhost:8080/api/users/42

# 创建用户
curl -X POST -H "Content-Type: application/json" \
     -d '{"name":"Charlie"}' \
     http://localhost:8080/api/users

# 更新用户
curl -X PUT -d '{"name":"Updated"}' http://localhost:8080/api/users/1

# 删除用户
curl -X DELETE http://localhost:8080/api/users/1

# 搜索（带查询参数）
curl "http://localhost:8080/api/search?keyword=hello&page=1"
```

**要点：**
- `req.param("id")` 获取路径参数 `{id}` 的值
- `req.jsonBody()` 解析 JSON 请求体（返回 `boost::json::value`）
- `req.query()` 获取查询字符串（`?` 后面的部分）
- `req.body()` 获取原始请求体
- 工厂方法：`ok()`, `json()`, `notFound()`, `badRequest()`, `serverError()`
- 手动构建：`setStatus()`, `setJsonBody()`, `setHeader()`

---

## 示例 3：中间件实战

### 日志中间件

```cpp
auto logger = [](HttpRequest& req, MiddlewareNext next)
                  -> Awaitable<HttpResponse> {
    auto start = std::chrono::steady_clock::now();

    std::cout << "[REQ] " << httpMethodToString(req.method())
              << " " << req.path() << std::endl;

    auto res = co_await next(req);  // 调用下一层

    auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - start).count();

    std::cout << "[RES] " << static_cast<int>(res.statusCode())
              << " (" << elapsed << " us)" << std::endl;

    co_return res;
};

server.use(logger);
```

### 认证中间件

```cpp
auto auth = [](HttpRequest& req, MiddlewareNext next)
                -> Awaitable<HttpResponse> {
    // 放行公开路径
    if (req.path() == "/" || req.path() == "/api/status")
    {
        co_return co_await next(req);
    }

    // 检查 Authorization 头
    auto token = req.header("Authorization");
    if (token.empty())
    {
        co_return HttpResponse::badRequest("需要认证: 请提供 Authorization 头");
    }

    if (token != "Bearer my-secret-token")
    {
        HttpResponse res;
        res.setStatus(HttpStatusCode::hUnauthorized);
        res.setJsonBody({{"error", "无效的认证令牌"}});
        co_return res;
    }

    // 验证通过，继续执行
    co_return co_await next(req);
};

server.use(auth);
```

### CORS 中间件

```cpp
auto cors = [](HttpRequest& req, MiddlewareNext next)
                -> Awaitable<HttpResponse> {
    // OPTIONS 预检请求
    if (req.method() == HttpMethod::hOptions)
    {
        HttpResponse res;
        res.setStatus(HttpStatusCode::hNoContent);
        res.setHeader("Access-Control-Allow-Origin", "*");
        res.setHeader("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
        res.setHeader("Access-Control-Allow-Headers", "Content-Type, Authorization");
        co_return res;
    }

    auto res = co_await next(req);
    res.setHeader("Access-Control-Allow-Origin", "*");
    co_return res;
};

server.use(cors);
```

### 异常处理中间件

```cpp
auto errorHandler = [](HttpRequest& req, MiddlewareNext next)
                        -> Awaitable<HttpResponse> {
    try
    {
        co_return co_await next(req);
    }
    catch (const std::exception& e)
    {
        std::cerr << "未处理异常: " << e.what() << std::endl;
        co_return HttpResponse::serverError();
    }
};

// 异常处理中间件应最先注册（最外层）
server.use(errorHandler);
server.use(logger);
server.use(auth);
```

**中间件执行顺序：**

```
请求 → errorHandler → logger → auth → 路由处理器
响应 ← errorHandler ← logger ← auth ← 路由处理器
```

---

## 示例 4：WebSocket 实时通信

```cpp
#include "core/HttpServer.h"
#include "core/WebSocket.h"
#include <iostream>

using namespace hical;

int main()
{
    HttpServer server(8080);

    // WebSocket Echo
    server.router().ws("/ws/echo",
        // 消息回调
        [](const std::string& msg, WebSocketSession& ws) -> Awaitable<void> {
            std::cout << "收到消息: " << msg << std::endl;
            co_await ws.send("Echo: " + msg);
        },
        // 连接建立回调（可选）
        [](WebSocketSession& ws) -> Awaitable<void> {
            std::cout << "新 WebSocket 连接建立" << std::endl;
            co_await ws.send("欢迎连接 hical WebSocket!");
        }
    );

    // WebSocket 聊天室（简单示例）
    server.router().ws("/ws/chat",
        [](const std::string& msg, WebSocketSession& ws) -> Awaitable<void> {
            // 处理聊天消息
            std::string response = "[服务器] 收到: " + msg;
            co_await ws.send(response);
        },
        [](WebSocketSession& ws) -> Awaitable<void> {
            co_await ws.send("[系统] 您已加入聊天室");
        }
    );

    // 同时注册 HTTP 路由
    server.router().get("/", [](const HttpRequest&) -> HttpResponse {
        return HttpResponse::ok("WebSocket 服务运行中。连接 /ws/echo 或 /ws/chat");
    });

    std::cout << "WebSocket 服务运行在 ws://localhost:8080" << std::endl;
    server.start();
    return 0;
}
```

**测试 WebSocket：**

```bash
# 方法 1：使用 wscat（需要 Node.js）
npm install -g wscat
wscat -c ws://localhost:8080/ws/echo

# 方法 2：使用 websocat
websocat ws://localhost:8080/ws/echo

# 方法 3：浏览器控制台
# const ws = new WebSocket('ws://localhost:8080/ws/echo');
# ws.onmessage = (e) => console.log(e.data);
# ws.send('Hello!');
```

---

## 示例 5：SSL/TLS 安全服务

### 生成测试证书

```bash
openssl req -x509 -newkey rsa:2048 -keyout server.key -out server.crt \
    -days 365 -nodes -subj "/CN=localhost"
```

### HTTPS 服务器

```cpp
#include "core/HttpServer.h"

using namespace hical;

int main()
{
    HttpServer server(8443);

    // 启用 SSL
    server.enableSsl("server.crt", "server.key");

    server.router().get("/", [](const HttpRequest&) -> HttpResponse {
        return HttpResponse::ok("Hello from HTTPS!");
    });

    server.router().get("/api/secure", [](const HttpRequest&) -> HttpResponse {
        return HttpResponse::json({
            {"secure", true},
            {"protocol", "HTTPS"}
        });
    });

    std::cout << "HTTPS 服务运行在 https://localhost:8443" << std::endl;
    server.start();
    return 0;
}
```

**测试：**

```bash
# -k 跳过证书验证（自签名证书）
curl -k https://localhost:8443/
curl -k https://localhost:8443/api/secure
```

---

## 示例 6：协程异步处理

### 异步路由处理器

```cpp
#include "core/HttpServer.h"
#include "core/Coroutine.h"

using namespace hical;

int main()
{
    HttpServer server(8080);

    // 协程路由：可以使用 co_await
    server.router().get("/async/delay",
        [](const HttpRequest&) -> Awaitable<HttpResponse> {
            // 模拟异步操作（如数据库查询、RPC 调用）
            co_await hical::sleep(0.5);  // 等待 0.5 秒
            co_return HttpResponse::json({
                {"message", "异步处理完成"},
                {"delayed", "500ms"}
            });
        });

    // 使用 chrono 精确控制延迟
    server.router().get("/async/precise",
        [](const HttpRequest&) -> Awaitable<HttpResponse> {
            co_await hical::sleep(std::chrono::milliseconds(100));
            co_return HttpResponse::ok("精确延迟 100ms");
        });

    // 同步路由：无需协程，直接返回
    server.router().get("/sync",
        [](const HttpRequest&) -> HttpResponse {
            return HttpResponse::ok("同步处理，无 co_await");
        });

    server.start();
    return 0;
}
```

**要点：**
- 协程处理器返回类型为 `Awaitable<HttpResponse>`，使用 `co_return`
- 同步处理器返回类型为 `HttpResponse`，使用 `return`
- 两种风格可混用，同步处理器会被框架自动包装为协程
- `hical::sleep()` 在当前协程上下文中等待，不阻塞线程

---

## 示例 7：PMR 内存池使用

### 基本用法

```cpp
#include "core/MemoryPool.h"
#include "core/PmrBuffer.h"
#include <iostream>

using namespace hical;

int main()
{
    // 可选：自定义内存池配置
    PoolConfig config;
    config.requestPoolInitialSize = 8192;
    config.threadLocalMaxBlocksPerChunk = 128;
    MemoryPool::instance().configure(config);

    // 使用线程本地池分配（高性能，无锁）
    auto alloc = MemoryPool::instance().threadLocalAllocator();
    std::pmr::vector<int> numbers(alloc);
    for (int i = 0; i < 1000; ++i)
    {
        numbers.push_back(i);
    }

    // 使用 PmrBuffer 读写数据
    PmrBuffer buffer(alloc);
    buffer.append("Hello, ");
    buffer.append("PMR world!");
    std::cout << "缓冲区: " << buffer.readAll() << std::endl;

    // 模拟请求级池：一次 HTTP 请求的生命周期
    {
        auto requestPool = MemoryPool::instance().createRequestPool(4096);
        std::pmr::polymorphic_allocator<std::byte> reqAlloc(requestPool.get());

        // 请求内所有分配共享同一个池
        std::pmr::vector<char> header(128, reqAlloc);
        std::pmr::vector<char> body(2048, reqAlloc);
        std::pmr::string jsonStr("JSON data", reqAlloc);

        // 做业务处理...

    }  // requestPool 析构 → 所有内存一次性释放

    // 查看统计
    auto stats = MemoryPool::instance().getStats();
    std::cout << "分配次数: " << stats.totalAllocations << std::endl;
    std::cout << "释放次数: " << stats.totalDeallocations << std::endl;
    std::cout << "当前字节: " << stats.currentBytesAllocated << std::endl;
    std::cout << "峰值字节: " << stats.peakBytesAllocated << std::endl;

    return 0;
}
```

### PmrBuffer 高级用法

```cpp
PmrBuffer buf;

// 写入数据
buf.append("GET / HTTP/1.1\r\nHost: localhost\r\n\r\n");

// 查找 CRLF（协议解析场景）
auto crlf = buf.findCRLF();
if (crlf)
{
    // 读取第一行
    size_t lineLen = crlf - buf.peek();
    std::string firstLine = buf.read(lineLen);
    buf.retrieve(2);  // 跳过 \r\n
}

// 确保可写空间
buf.ensureWritableBytes(4096);

// 直接写入
char* writePos = buf.beginWrite();
// ... 写入数据到 writePos ...
buf.hasWritten(bytesWritten);
```

---

## 示例 8：完整应用示例

将以上所有特性组合为一个完整的 Web 服务：

```cpp
#include "core/HttpServer.h"
#include "core/WebSocket.h"
#include "core/MemoryPool.h"
#include "core/Coroutine.h"
#include <iostream>

using namespace hical;

int main(int argc, char* argv[])
{
    try
    {
        // 配置
        auto port = static_cast<uint16_t>(argc >= 2 ? std::atoi(argv[1]) : 8080);
        auto threads = argc >= 3 ? std::atoi(argv[2]) : 1;

        // 内存池配置
        PoolConfig poolConfig;
        poolConfig.requestPoolInitialSize = 8192;
        MemoryPool::instance().configure(poolConfig);

        // 创建多线程 HTTP 服务器
        HttpServer server(port, threads);

        // ========== 中间件 ==========

        // 异常处理（最外层）
        server.use([](HttpRequest& req, MiddlewareNext next)
                       -> Awaitable<HttpResponse> {
            try
            {
                co_return co_await next(req);
            }
            catch (const std::exception& e)
            {
                std::cerr << "[ERROR] " << req.path() << ": " << e.what() << std::endl;
                co_return HttpResponse::serverError();
            }
        });

        // 日志
        server.use([](HttpRequest& req, MiddlewareNext next)
                       -> Awaitable<HttpResponse> {
            std::cout << httpMethodToString(req.method())
                      << " " << req.path() << std::endl;
            auto res = co_await next(req);
            std::cout << "  -> " << static_cast<int>(res.statusCode()) << std::endl;
            co_return res;
        });

        // CORS
        server.use([](HttpRequest& req, MiddlewareNext next)
                       -> Awaitable<HttpResponse> {
            auto res = co_await next(req);
            res.setHeader("Access-Control-Allow-Origin", "*");
            co_return res;
        });

        // ========== HTTP 路由 ==========

        auto& router = server.router();

        // 首页
        router.get("/", [](const HttpRequest&) -> HttpResponse {
            return HttpResponse::ok("hical Web 服务 v1.0.0");
        });

        // 状态接口
        router.get("/api/status", [](const HttpRequest&) -> HttpResponse {
            auto stats = MemoryPool::instance().getStats();
            return HttpResponse::json({
                {"status", "running"},
                {"version", "1.0.0"},
                {"memory", {
                    {"allocated", stats.currentBytesAllocated},
                    {"peak", stats.peakBytesAllocated},
                    {"allocations", stats.totalAllocations}
                }}
            });
        });

        // RESTful 用户 API
        router.get("/api/users/{id}", [](const HttpRequest& req) -> HttpResponse {
            return HttpResponse::json({
                {"id", req.param("id")},
                {"name", "User " + req.param("id")}
            });
        });

        router.post("/api/echo", [](const HttpRequest& req) -> HttpResponse {
            return HttpResponse::ok(req.body());
        });

        // 协程路由
        router.get("/api/async", [](const HttpRequest&) -> Awaitable<HttpResponse> {
            co_await hical::sleep(0.1);
            co_return HttpResponse::json({{"async", true}});
        });

        // ========== WebSocket ==========

        router.ws("/ws/echo",
            [](const std::string& msg, WebSocketSession& ws) -> Awaitable<void> {
                co_await ws.send("Echo: " + msg);
            },
            [](WebSocketSession& ws) -> Awaitable<void> {
                co_await ws.send("Connected!");
            }
        );

        // ========== 启动 ==========

        std::cout << "hical Web 服务 v1.0.0" << std::endl;
        std::cout << "端口: " << port << ", IO 线程: " << threads << std::endl;
        std::cout << "路由:" << std::endl;
        std::cout << "  GET    /              — 首页" << std::endl;
        std::cout << "  GET    /api/status    — 状态查询" << std::endl;
        std::cout << "  GET    /api/users/{id}— 用户查询" << std::endl;
        std::cout << "  POST   /api/echo      — Echo" << std::endl;
        std::cout << "  GET    /api/async     — 异步示例" << std::endl;
        std::cout << "  WS     /ws/echo       — WebSocket" << std::endl;

        server.start();
    }
    catch (const std::exception& e)
    {
        std::cerr << "异常: " << e.what() << std::endl;
    }

    return 0;
}
```

**运行：**

```bash
# 单线程
./my_server 8080

# 4 线程
./my_server 8080 4
```

---

## 示例 9：数据库中间件

> 需要 `HICAL_WITH_DATABASE=ON` 编译选项和 MySQL 数据库

### 基础用法：连接池 + 参数化查询

```cpp
#include "core/HttpServer.h"
#include "db/DbMiddleware.h"
#include "db/MysqlConnection.h"

using namespace hical;
using namespace hical::db;

int main()
{
    HttpServer server(8080);

    // 配置连接池
    DbConfig dbConfig;
    dbConfig.host     = "127.0.0.1";
    dbConfig.port     = 3306;
    dbConfig.database = "mydb";
    dbConfig.user     = "root";
    dbConfig.password = "secret";
    dbConfig.poolSize = 10;           // 连接池大小
    dbConfig.connectTimeoutSec  = 5;  // 连接超时
    dbConfig.idleTimeoutSec     = 60; // 空闲连接回收阈值

    // 初始化连接池（MySQL 工厂）
    auto factory = MysqlConnection::makeFactory(dbConfig);
    auto pool    = std::make_shared<DbConnectionPool>(std::move(factory), dbConfig);

    // 注册数据库中间件：自动为每个请求分配 / 归还连接
    server.use(makeDbMiddleware(pool));

    // 路由中使用参数化查询
    server.router().get("/api/users/{id}",
        [](const HttpRequest& req) -> Awaitable<HttpResponse> {
            auto conn   = getDbConnection(req);  // 获取当前请求的连接
            auto userId = req.param("id");

            // 参数化查询，防止 SQL 注入
            auto rows = co_await conn->query(
                "SELECT id, name, email FROM users WHERE id = ?",
                {userId}
            );

            if (rows.empty())
            {
                co_return HttpResponse::notFound("用户不存在");
            }

            co_return HttpResponse::json({
                {"id",    rows[0]["id"]},
                {"name",  rows[0]["name"]},
                {"email", rows[0]["email"]}
            });
        });

    server.router().post("/api/users",
        [](const HttpRequest& req) -> Awaitable<HttpResponse> {
            auto conn = getDbConnection(req);
            auto json = req.jsonBody();
            auto name  = std::string(json.at("name").as_string());
            auto email = std::string(json.at("email").as_string());

            auto result = co_await conn->execute(
                "INSERT INTO users (name, email) VALUES (?, ?)",
                {name, email}
            );

            HttpResponse res;
            res.setStatus(HttpStatusCode::hCreated);
            res.setJsonBody({
                {"message",  "用户创建成功"},
                {"insertId", result.lastInsertId}
            });
            co_return res;
        });

    server.start();
    return 0;
}
```

**测试：**

```bash
curl http://localhost:8080/api/users/1

curl -X POST -H "Content-Type: application/json" \
     -d '{"name":"Alice","email":"alice@example.com"}' \
     http://localhost:8080/api/users
```

### 自动事务 + 查询日志

```cpp
#include "core/HttpServer.h"
#include "db/DbMiddleware.h"
#include "db/QueryLogMiddleware.h"
#include "db/MysqlConnection.h"

using namespace hical;
using namespace hical::db;

int main()
{
    HttpServer server(8080);

    DbConfig dbConfig;
    dbConfig.host            = "127.0.0.1";
    dbConfig.database        = "mydb";
    dbConfig.user            = "root";
    dbConfig.password        = "secret";
    dbConfig.poolSize        = 10;
    dbConfig.autoTransaction = true;  // 请求成功自动 commit，异常自动 rollback

    auto pool = std::make_shared<DbConnectionPool>(
        MysqlConnection::makeFactory(dbConfig), dbConfig
    );

    // 数据库中间件（先注册）
    server.use(makeDbMiddleware(pool));

    // 查询日志中间件（必须在 makeDbMiddleware 之后注册）
    QueryLogConfig logConfig;
    logConfig.slowQueryThresholdMs = 100;  // 超过 100ms 记为慢查询
    logConfig.onSlowQuery = [](const QueryLogEntry& entry) {
        std::cerr << "[SLOW QUERY] " << entry.sql
                  << " | " << entry.durationMs << "ms" << std::endl;
    };
    server.use(makeQueryLogMiddleware(logConfig));

    // 转账接口：多步操作自动在同一事务内
    server.router().post("/api/transfer",
        [](const HttpRequest& req) -> Awaitable<HttpResponse> {
            auto conn = getDbConnection(req);
            auto json = req.jsonBody();
            auto from   = std::string(json.at("from").as_string());
            auto to     = std::string(json.at("to").as_string());
            auto amount = json.at("amount").as_int64();

            // 扣款
            co_await conn->execute(
                "UPDATE accounts SET balance = balance - ? WHERE id = ?",
                {amount, from}
            );

            // 入账
            co_await conn->execute(
                "UPDATE accounts SET balance = balance + ? WHERE id = ?",
                {amount, to}
            );

            // autoTransaction=true：两步都成功则 commit，任意步抛异常则 rollback
            co_return HttpResponse::json({{"message", "转账成功"}, {"amount", amount}});
        });

    // onRequestComplete 回调：请求结束时记录汇总信息
    server.setOnRequestComplete([](const HttpRequest& req, const HttpResponse& res) {
        auto log = getQueryLog(req);
        if (log && !log->entries.empty())
        {
            std::cout << req.path() << " 执行了 " << log->entries.size()
                      << " 条查询" << std::endl;
        }
    });

    server.start();
    return 0;
}
```

### 要点

- `DbConfig` 配置连接池大小、超时、健康检查等参数
- `MysqlConnection::makeFactory()` 创建 MySQL 连接工厂
- `makeDbMiddleware()` 自动为每个请求获取/归还连接
- `autoTransaction = true` 时，请求成功自动 commit，异常自动 rollback
- `makeQueryLogMiddleware()` 必须注册在 `makeDbMiddleware()` 之后
- `getDbConnection(req)` 获取当前请求的数据库连接
- 参数化查询使用 `conn->query(sql, {params})`，防止 SQL 注入
- 不带参数的 `query(sql)` 和 `execute(sql)` 已标记 `[[deprecated]]`

### 构建与运行

```bash
# 启用数据库中间件编译
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DHICAL_WITH_DATABASE=ON
cmake --build build

# 配置数据库连接（也可直接写在代码中）
export DB_HOST=127.0.0.1
export DB_PORT=3306
export DB_NAME=mydb
export DB_USER=root
export DB_PASS=secret

./build/examples/db_example
```

---

## 示例 10：OpenAPI 文档自动生成

> 需要 `HICAL_WITH_OPENAPI=ON`（默认已启用），**无需额外依赖**，复用 Boost.JSON。
>
> 源文件：`examples/openapi_server.cpp`

### 功能演示

该示例包含：
- 4 个 DTO 结构体（`UserDTO`、`CreateUserRequest`、`UpdateUserRequest`、`ErrorResponse`），通过 `HICAL_SCHEMA_NAME` 注册 `$ref` 名
- 5 条路由（GET /api/users、GET /api/users/{id}、POST /api/users、PUT /api/users/{id}、DELETE /api/users/{id}）
- 每条路由使用 `HICAL_API()` + `builder::*` 进行完整标注（summary、tags、requestBody、responses）
- `registerRoutesWithOpenApi()` 同步注册路由与 API 元数据
- `serveOpenApi()` 一行暴露 `/openapi.json` + `/docs`（Swagger UI）

### 编译与运行

```bash
# 确认 HICAL_WITH_OPENAPI 已启用（默认 ON）
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --target openapi_server

# 启动服务器
./build/examples/openapi_server 8080        # Linux / macOS
./build/examples/openapi_server.exe 8080    # Windows
```

### 访问端点

| 端点                                     | 说明                                      |
| ---------------------------------------- | ----------------------------------------- |
| `GET http://localhost:8080/openapi.json` | OpenAPI 3.0 JSON 规范，可直接导入 Postman |
| `GET http://localhost:8080/docs`         | Swagger UI 交互式文档（CDN 加载）         |
| `GET http://localhost:8080/api/users`    | 获取用户列表（示例路由）                  |
| `GET http://localhost:8080/api/users/42` | 获取单个用户（路径参数示例）              |

```bash
# 获取 OpenAPI 规范
curl http://localhost:8080/openapi.json | python -m json.tool

# 访问浏览器查看 Swagger UI
# http://localhost:8080/docs
```

### 关键代码结构

```cpp
// 1. 定义 DTO + 注册 schema 名
HICAL_SCHEMA_NAME(UserDTO, "User")
struct UserDTO { HICAL_JSON(UserDTO, REQUIRED(id), name); int id; std::string name; };

// 2. Handler 中标注 API（routeApiTable 静态方法）
static std::vector<RouteApiInfo> routeApiTable()
{
    return {
        HICAL_API(
            builder::summary("获取用户"),
            builder::tag("用户管理"),
            builder::response<UserDTO>(200),
            builder::response(404, "用户不存在")
        ),
        // ...
    };
}
HICAL_ROUTES_WITH_API(MyHandler, getUser, createUser, ...)

// 3. 注册路由 + 元数据 + 暴露文档
registerRoutesWithOpenApi(router, handler, *registry);
doc->addSchemas(schemas);
serveOpenApi(router, doc);
```

---

## 示例 11：CORS 与路由分组

> CORS 中间件 + RouteGroup 路由前缀分组

### CORS 中间件

```cpp
#include "core/HttpServer.h"
#include "core/Cors.h"

using namespace hical;

int main()
{
    HttpServer server(8080);

    // 使用默认配置（允许所有源）
    server.use(makeCorsMiddleware());

    // 或者精确配置
    CorsOptions corsOpts;
    corsOpts.allowedOrigins = {"https://example.com", "https://app.example.com"};
    corsOpts.allowCredentials = true;
    corsOpts.allowedHeaders = {"Content-Type", "Authorization", "X-Custom"};
    corsOpts.maxAge = 3600;
    server.use(makeCorsMiddleware(corsOpts));

    server.router().get("/api/data", [](const HttpRequest&) -> HttpResponse {
        return HttpResponse::json({{"data", "hello"}});
    });

    server.start();
    return 0;
}
```

### 路由分组

```cpp
#include "core/HttpServer.h"
#include "core/RouteGroup.h"

using namespace hical;

int main()
{
    HttpServer server(8080);
    auto& router = server.router();

    // 创建 /api/v1 路由组
    auto api = router.group("/api/v1");

    // 组级中间件（仅对组内路由生效）
    api.use([](HttpRequest& req, MiddlewareNext next) -> Awaitable<HttpResponse> {
        auto token = req.header("Authorization");
        if (token.empty())
        {
            co_return HttpResponse::badRequest("需要认证");
        }
        co_return co_await next(req);
    });

    // 组内路由（自动添加 /api/v1 前缀）
    api.get("/users", [](const HttpRequest&) -> HttpResponse {
        return HttpResponse::json({{"users", boost::json::array{}}});
    });                                                    // → GET /api/v1/users

    api.post("/users", [](const HttpRequest& req) -> HttpResponse {
        return HttpResponse::json({{"created", true}});
    });                                                    // → POST /api/v1/users

    api.get("/users/{id}", [](const HttpRequest& req) -> HttpResponse {
        return HttpResponse::json({{"id", req.param("id")}});
    });                                                    // → GET /api/v1/users/{id}

    // 嵌套子组
    auto admin = api.group("/admin");
    admin.get("/stats", [](const HttpRequest&) -> HttpResponse {
        return HttpResponse::json({{"activeUsers", 42}});
    });                                                    // → GET /api/v1/admin/stats

    server.start();
    return 0;
}
```

**测试：**

```bash
# CORS 预检请求
curl -X OPTIONS -H "Origin: https://example.com" http://localhost:8080/api/data

# 路由组
curl http://localhost:8080/api/v1/users
curl http://localhost:8080/api/v1/users/42
curl http://localhost:8080/api/v1/admin/stats
```

**要点：**
- `makeCorsMiddleware()` 一行启用 CORS，自动处理 OPTIONS 预检
- `router.group("/prefix")` 创建路由组，组内路由自动添加前缀
- `RouteGroup::use()` 添加组级中间件，仅对该组路由生效
- 支持多层嵌套：`api.group("/admin")` 继承父组前缀和中间件

---

## 示例 12：查询参数与表单参数

> 查询参数、表单参数解析 API、重定向响应

```cpp
#include "core/HttpServer.h"

using namespace hical;

int main()
{
    HttpServer server(8080);
    auto& router = server.router();

    // 查询参数
    router.get("/search", [](const HttpRequest& req) -> HttpResponse {
        // 获取单个查询参数
        auto keyword = req.queryParam("keyword");  // ?keyword=hello → "hello"
        auto page = req.queryParam("page");        // ?page=2 → "2"

        if (!keyword)
        {
            return HttpResponse::badRequest("缺少 keyword 参数");
        }

        return HttpResponse::json({
            {"keyword", *keyword},
            {"page", page.value_or("1")}
        });
    });

    // 表单参数（application/x-www-form-urlencoded）
    router.post("/login", [](const HttpRequest& req) -> HttpResponse {
        auto username = req.formParam("username");
        auto password = req.formParam("password");

        if (!username || !password)
        {
            return HttpResponse::badRequest("缺少用户名或密码");
        }

        // 业务逻辑...
        return HttpResponse::json({{"message", "登录成功"}, {"user", *username}});
    });

    // 重定向
    router.get("/old-page", [](const HttpRequest&) -> HttpResponse {
        return HttpResponse::redirect("/new-page");           // 302 临时重定向
    });

    router.get("/moved", [](const HttpRequest&) -> HttpResponse {
        return HttpResponse::redirect("/new-location",
            HttpStatusCode::hMovedPermanently);               // 301 永久重定向
    });

    // 全局错误处理器
    server.setErrorHandler([](const std::exception& e, const HttpRequest& req) {
        return HttpResponse::json({
            {"error", e.what()},
            {"path", std::string(req.path())}
        });
    });

    server.start();
    return 0;
}
```

**测试：**

```bash
# 查询参数
curl "http://localhost:8080/search?keyword=hello&page=2"

# 表单参数
curl -X POST -d "username=alice&password=secret" http://localhost:8080/login

# 重定向
curl -v http://localhost:8080/old-page
# 返回 302 + Location: /new-page
```

**要点：**
- `req.queryParam("name")` 返回 `optional<string>`，不存在返回 `nullopt`
- `req.formParam("name")` 解析 `application/x-www-form-urlencoded` 请求体
- 两者均为惰性解析 + 缓存，首次访问时解析
- `HttpResponse::redirect(url)` 默认 302，可指定 301/307/308
- `setErrorHandler()` 捕获路由处理器中未处理的异常

---

## 示例 13：日志系统

> 日志系统

### 基本用法

```cpp
#include "core/Log.h"

using namespace hical;

int main()
{
    // 配置日志
    auto& logger = Logger::instance();
    logger.setLevel(LogLevel::hInfo);          // 最低输出级别
    logger.setFlushLevel(LogLevel::hWarn);      // Warn 及以上自动刷盘
    logger.addSink(std::make_shared<StderrSink>());

    // std::format 风格
    HICAL_LOG_INFO("服务器启动, port={}, threads={}", 8080, 4);
    HICAL_LOG_ERROR("连接失败: {}", "timeout");

    // 流式 API
    HICAL_LOG_INFO_STREAM << "处理完成, 耗时: " << 42 << "ms";

    // 条件宏
    int latency = 150;
    HICAL_LOG_WARN_IF(latency > 100, "慢请求: {}ms", latency);

    // 结构化字段
    HICAL_LOG_INFO_F("用户登录", {{"userId", 42}, {"ip", "192.168.1.1"}});

    return 0;
}
```

### 文件日志 + 轮转

```cpp
#include "core/Log.h"
#include "core/LogSink.h"
#include "core/AsyncFileSink.h"

using namespace hical;

int main()
{
    auto& logger = Logger::instance();

    // 同步文件日志（带轮转：100MB 单文件，保留 10 个历史文件）
    auto fileSink = std::make_shared<FileSink>("./logs/app.log", 100 * 1024 * 1024, 10);
    logger.addSink(fileSink);

    // 异步文件日志（推荐高吞吐场景）
    AsyncFileSink::Options asyncOpts;
    asyncOpts.file.basePath = "./logs/async.log";
    asyncOpts.file.maxFileSize = 100 * 1024 * 1024;
    asyncOpts.file.maxFiles = 10;
    logger.addSink(std::make_shared<AsyncFileSink>(asyncOpts));

    HICAL_LOG_INFO("日志写入文件");
    return 0;
}
```

### 命名通道

```cpp
#include "core/Log.h"
#include "core/LogChannel.h"

using namespace hical;

int main()
{
    // 创建专用通道
    auto& registry = LogChannelRegistry::instance();
    auto accessChannel = std::make_shared<LogChannel>("access");
    accessChannel->setLevel(LogLevel::hInfo);
    accessChannel->setFormatter(std::make_shared<JsonFormatter>());
    accessChannel->addSink(std::make_shared<FileSink>("./logs/access.log"));
    registry.add("access", accessChannel);

    // 使用通道路由宏
    HICAL_LOG_TO("access", Info, "GET /api/users 200 12ms");

    return 0;
}
```

### HTTP 日志中间件 + 动态级别管理

```cpp
#include "core/HttpServer.h"
#include "core/Log.h"
#include "core/LogMiddleware.h"
#include "core/LogAdmin.h"

using namespace hical;

int main()
{
    HttpServer server(8080);

    // 日志中间件（自动 trace-id + 访问日志）
    server.use(makeLogMiddleware());

    // 动态级别管理端点
    registerLogAdmin(server.router());

    server.router().get("/api/data", [](const HttpRequest& req) -> HttpResponse {
        // 中间件已注入 trace-id
        auto traceId = req.getAttribute<std::string>("hical.trace_id");
        return HttpResponse::json({{"traceId", traceId.value_or("")}});
    });

    server.start();
    return 0;
}
```

**测试：**

```bash
# 查看当前日志级别
curl http://localhost:8080/admin/log-level

# 动态调整日志级别
curl -X PUT -H "Content-Type: application/json" \
     -d '{"level":"debug"}' \
     http://localhost:8080/admin/log-level

# 调整通道级别
curl -X PUT -H "Content-Type: application/json" \
     -d '{"channel":"access","level":"warn"}' \
     http://localhost:8080/admin/log-level
```

**要点：**
- `HICAL_LOG_*` 宏支持 `std::format` 语法（`{}`/`{0}`/`{:.2f}`）
- NDEBUG 编译下 `HICAL_LOG_TRACE` 完全消除（零开销）
- `AsyncFileSink` 使用后台线程 + 双缓冲，不阻塞业务线程
- `makeLogMiddleware()` 自动生成 128 位 trace-id 并记录访问日志
- `registerLogAdmin()` 支持运行时动态调整日志级别

---

## 运行内置示例程序

Hical 项目自带 7 个示例程序：

```bash
# 编译所有示例
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

### Echo Server（底层 TCP）

```bash
./build/examples/echo_server 8888
# 测试: telnet localhost 8888
```

### HTTP Server（完整示例）

```bash
./build/examples/http_server 8080
# 测试:
#   curl http://localhost:8080/
#   curl http://localhost:8080/api/status
#   curl -X POST -d 'hello' http://localhost:8080/api/echo
#   curl http://localhost:8080/users/42
#   wscat -c ws://localhost:8080/ws/echo
```

### OpenAPI Server（文档自动生成）

```bash
./build/examples/openapi_server 8080
# 访问:
#   http://localhost:8080/openapi.json  — OpenAPI 3.0 JSON 规范
#   http://localhost:8080/docs          — Swagger UI 交互式文档
#   curl http://localhost:8080/api/users
```

### PMR PoC（内存池验证）

```bash
./build/examples/pmr_poc
# 输出: 缓冲区复用、批量分配、PmrBuffer 功能、多线程并发的性能数据
```

### PMR Benchmark（内存池基准测试）

```bash
./build/examples/pmr_benchmark
# 输出: 不同分配策略在不同块大小和线程数下的性能对比
```

### HTTP Benchmark（HTTP 压测工具）

```bash
# 先启动 http_server
./build/examples/http_server 8080

# 压测
./build/examples/http_benchmark localhost 8080 50 1000 /api/status GET
# 输出: QPS、延迟分布 (P50/P90/P95/P99)
```

### TCP Benchmark（Echo 压测工具）

```bash
# 先启动 echo_server
./build/examples/echo_server 8888

# 压测
./build/examples/benchmark localhost 8888 100 1000
# 输出: QPS、总耗时、成功/失败数
```

---

## 常见问题

### Q: 协程处理器和同步处理器该选哪个？

**同步处理器**：无需异步操作时使用，代码更简洁：
```cpp
router.get("/api/hello", [](const HttpRequest&) -> HttpResponse {
    return HttpResponse::ok("Hello!");
});
```

**协程处理器**：需要 `co_await` 异步操作时使用：
```cpp
router.get("/api/async", [](const HttpRequest&) -> Awaitable<HttpResponse> {
    co_await hical::sleep(0.1);
    co_return HttpResponse::ok("Done");
});
```

框架会自动将同步处理器包装为协程，性能无差别。

### Q: 中间件的注册顺序重要吗？

重要。中间件按注册顺序从外到内执行。建议顺序：
1. 异常处理中间件（最外层，兜底所有异常）
2. 日志中间件
3. CORS 中间件
4. 认证/鉴权中间件（最内层，最接近路由处理器）

### Q: 路径参数和查询参数有什么区别？

- **路径参数**：`/users/{id}` → `req.param("id")` 获取
- **查询参数**：`/search?keyword=hello` → `req.query()` 获取整个查询字符串

### Q: 如何配置多线程？

```cpp
// 构造函数第二个参数指定 IO 线程数
HttpServer server(8080, 4);  // 4 个 IO 线程
```

推荐设为 CPU 核数：`HttpServer server(8080, std::thread::hardware_concurrency());`

### Q: 内存池需要手动管理吗？

不需要。Hical 的 PMR 内存池自动管理：
- 线程本地池：线程启动时自动创建
- 请求级池：请求结束时自动释放
- 全局池：进程退出时自动清理

如需自定义配置，在创建 `HttpServer` 之前调用：
```cpp
PoolConfig config;
config.requestPoolInitialSize = 8192;
MemoryPool::instance().configure(config);
```

### Q: 如何启用 SSL？

```cpp
server.enableSsl("server.crt", "server.key");
```

详见 [示例 5：SSL/TLS 安全服务](#示例-5ssltls-安全服务)。

### Q: 如何配置日志输出？

```cpp
#include "core/Log.h"

auto& logger = hical::Logger::instance();
logger.setLevel(hical::LogLevel::hInfo);
logger.addSink(std::make_shared<hical::StderrSink>());       // 控制台
hical::AsyncFileSink::Options asyncOpts;
asyncOpts.file.basePath = "./logs/app.log";
asyncOpts.file.maxFileSize = 100 * 1024 * 1024;
asyncOpts.file.maxFiles = 10;
logger.addSink(std::make_shared<hical::AsyncFileSink>(asyncOpts));  // 异步文件
```

推荐高吞吐场景使用 `AsyncFileSink`，开发环境使用 `StderrSink`。

### Q: CORS 中间件和手写 CORS 有什么区别？

`makeCorsMiddleware()` 自动处理 OPTIONS 预检请求、`Vary: Origin` 缓存头、凭证模式安全校验。手写 CORS 容易遗漏这些细节。

### Q: 路由组中间件和全局中间件有什么区别？

全局中间件（`server.use()`）对所有路由生效。组级中间件（`group.use()`）仅对该组及子组的路由生效，不影响其他路由。

---

> 更多信息：[API 文档](api_reference.md) | [架构设计](architecture.md) | [性能报告](performance_report.md) | [编译指南](build_and_test_guide.md)
