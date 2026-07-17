# Hical API 参考文档

> 完整的 hical 框架公共 API 说明（与源码 [src/core/](../src/core/)、[src/db/](../src/db/)、[src/asio/](../src/asio/) 同步）

---

## 全局约定

> 阅读各模块 API 前，请先了解这些贯穿全框架的契约。

### 线程模型
- `HttpServer` 启动后会派生 `ioThreads` 个独立 `io_context`（Linux/macOS 走 SO_REUSEPORT 多 acceptor，Windows 走单 acceptor + least-connections 分发）。每个连接固定绑定到其所属的 `io_context` 线程，**所有 I/O 与回调均在该线程内串行执行**，连接级状态无需加锁。
- 跨连接共享的数据结构（`SessionManager`、`WsHub`、`LogChannelRegistry`、`DbConnectionPool`、`OpenApiRegistry`）由框架内部以 `shared_mutex` / 原子操作保护，用户调用安全。
- 用户自定义的"广播/批处理"逻辑若跨连接读写自有数据，需自行加锁。

### 协程生命周期
- 路由处理器返回 `Awaitable<HttpResponse>`（即 `boost::asio::awaitable<T>`）。
- **`co_await` 不得跨越 `this` 销毁**。若处理器/中间件捕获了堆对象，必须用 `shared_from_this()` 或 `std::shared_ptr` 捕获，禁止裸 `this` 捕获。
- `WebSocketSession` 在 `onMessage`/`onConnect` 回调外的访问需通过 `WsHub::sendTo()` 等线程安全 API。

### PMR 内存池作用域
- 请求级 `monotonic_buffer_resource`（`MemoryPool::createRequestPool()`）**严禁逃逸请求生命周期** —— 任何指向该池的指针/引用必须在响应发送后失效。
- 线程本地池 `threadLocalAllocator()` 跨请求复用但不跨线程，禁止把分配的对象传给其他线程。
- 全局同步池 `globalAllocator()` 可任意跨线程，但有同步开销。

### 错误模型
不同 API 采用不同错误传递方式，按"调用者直接动作"分三类：

| 方式                       | 适用 API                                                                                                                                     | 示例                            |
| -------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------- |
| 抛 C++ 异常                | `meta::fromJson<T>` / `req.readJson<T>()` / `DbConnection::query` / `DbConnectionPool::acquire`（超时）                                      | JSON 解析失败、SQL 错误、池超时 |
| 返回 `std::optional<T>`    | `req.queryParam` / `req.formParam` / `MultipartParser::parse`（256 part 限制） / `Session::get<T>` / `WebSocketSession::receive`（连接关闭） | 缺失字段/数据                   |
| 返回 `NetworkError` 结构体 | `Error.h` 内 `toNetworkError(ec)` 等转换函数                                                                                                 | Asio 错误码统一映射             |

未由路由处理器捕获的异常会冒泡到 `HttpServer::setErrorHandler()` 指定的全局 handler（默认返回 500）。

### 版本与稳定性
- 本文档随 `CMakeLists.txt` 的 `project(VERSION)` 同步，当前版本见 [Version.h](../src/core/Version.h.in)（构建后生成）。
- 带 `[[deprecated]]` 的方法（如 `DbConnection::query(sql)` 无参数重载）将在后续大版本移除。
- `src/asio/` 视为实现细节，跨小版本可能变动；用户应通过 `Concepts.h` 的 `NetworkBackend` 概念替换后端，而非直接依赖 `AsioBackend`。

---

## 目录

**核心 API（用户直接使用）**
- [HttpServer](#httpserver) — HTTP 服务器
- [Router](#router) — 路由管理
- [RouteGroup](#routegroup) — 路由分组
- [HttpRequest](#httprequest) — HTTP 请求
- [HttpResponse](#httpresponse) — HTTP 响应
- [HeaderMap](#headermap) — HTTP 头部容器
- [HttpTypes](#httptypes) — HTTP 类型定义
- [Middleware](#middleware) — 中间件
- [MetaJson](#metajson) — JSON 反射序列化
- [MetaRoutes](#metaroutes) — 反射路由注册
- [Cookie](#cookie) — Cookie 解析与设置
- [Session](#session) — Session 会话管理
- [CORS 中间件](#cors-中间件) — 跨域资源共享
- [StaticFiles](#staticfiles) — 静态文件服务
- [Multipart](#multipart) — 文件上传解析
- [JWT Auth](#jwt-auth) — JWT 认证中间件
- [ConfigLoader](#configloader) — JSON 配置加载器
- [WebSocketSession](#websocketsession) — WebSocket 会话
- [WsHub](#wshub) — WebSocket 广播管理器
- [WsOptions](#wsoptions) — WebSocket 路由选项

**基础设施 API（进阶用户）**
- [Coroutine](#coroutine) — 协程工具
- [MemoryPool](#memorypool) — pmr 内存池
- [SslContext](#sslcontext) — SSL/TLS 配置
- [Error](#error) — 错误处理
- [InetAddress](#inetaddress) — IP/端口封装

**底层抽象（仅摘要）**
- [PmrBuffer](#pmrbuffer) — pmr 缓冲区
- [EventLoop](#eventloop) — 事件循环接口
- [TcpConnection](#tcpconnection) — TCP 连接接口
- [Timer](#timer) — 定时器接口
- [WriteNode](#writenode) — 写缓冲节点
- [WsFrame / WsHandshake / WsDeflate](#websocket-协议层) — WebSocket 协议层
- [Concepts](#concepts) — 后端约束
- [Asio 适配层](#asio-适配层) — Boost.Asio 适配

**日志系统 API**
- [Log](#log) — 日志核心（Logger 单例 + 宏 API）
- [LogRecord](#logrecord) — 结构化日志条目
- [LogFormatter](#logformatter) — 日志格式化器
- [LogSink](#logsink) — 日志输出后端
- [LogFile](#logfile) — 日志文件轮转
- [AsyncFileSink](#asyncfilesink) — 异步文件 Sink
- [FixedBuffer](#fixedbuffer) — 栈上固定缓冲区
- [LogChannel](#logchannel) — 命名日志通道
- [LogMiddleware](#logmiddleware) — 日志中间件
- [LogAdmin](#logadmin) — 动态日志级别管理

**数据库中间件 API（可选，需 `HICAL_WITH_DATABASE=ON`）**
- [DbConfig](#dbconfig) — 数据库连接配置
- [DbResult](#dbresult) — 查询结果
- [DbConnection](#dbconnection) — 数据库连接接口
- [DbConnectionPool](#dbconnectionpool) — 协程化连接池
- [DbMiddleware](#dbmiddleware) — HTTP 数据库中间件
- [DbQueryLog](#dbquerylog) — 查询日志中间件
- [MysqlConnection](#mysqlconnection) — MySQL 后端
- [StmtCache](#stmtcache) — PreparedStatement LRU 缓存

**OpenAPI 元数据 API（可选，需 `HICAL_WITH_OPENAPI=ON`，默认启用）**
- [OpenApiSchema](#openapischema) — JSON Schema 生成
- [OpenApiRegistry](#openapiregistry) — 路由元数据注册表
- [OpenApiDocument](#openapidocument) — 文档组装
- [OpenApiEndpoint](#openapiendpoint) — 端点暴露

**附录**
- [类型别名速查](#类型别名速查)
- [回调类型速查](#回调类型速查)

---

## 核心 API

### HttpServer

HTTP 服务器，整合路由、中间件和网络层，提供一键启动的高层封装。

**头文件：** `<hical/core/HttpServer.h>`

#### 构造函数

| 方法                                              | 参数                                   | 说明             |
| ------------------------------------------------- | -------------------------------------- | ---------------- |
| `HttpServer(uint16_t port, size_t ioThreads = 1)` | port: 监听端口<br>ioThreads: IO 线程数 | 创建 HTTP 服务器 |

#### 公共方法

| 方法                           | 参数                                            | 返回值     | 说明                         |
| ------------------------------ | ----------------------------------------------- | ---------- | ---------------------------- |
| `router()`                     | 无                                              | `Router&`  | 获取路由器引用，用于注册路由 |
| `use(MiddlewareHandler)`       | middleware: 中间件处理器                        | `void`     | 添加中间件到管道             |
| `enableSsl(certFile, keyFile)` | certFile: 证书文件路径<br>keyFile: 私钥文件路径 | `void`     | 启用 SSL/TLS                 |
| `start()`                      | 无                                              | `void`     | 启动服务器（阻塞）           |
| `stop()`                       | 无                                              | `void`     | 停止服务器                   |
| `isRunning()`                  | 无                                              | `bool`     | 服务器是否正在运行           |
| `port()`                       | 无                                              | `uint16_t` | 获取监听端口                 |
| `setErrorHandler(handler)`     | handler: `ErrorHandler`                         | `void`     | 设置全局错误处理器           |
| `setGcInterval(seconds)`       | seconds: GC 间隔（秒）                          | `void`     | 设置内存池 GC 间隔           |

#### 类型定义

```cpp
using ErrorHandler = std::function<HttpResponse(const std::exception& e, const HttpRequest& req)>;
```

#### 示例

```cpp
#include <hical/core/HttpServer.h>

using namespace hical;

int main()
{
    // 创建服务器，4 个 IO 线程
    HttpServer server(8080, 4);
    
    // 注册路由
    server.router().get("/", [](const HttpRequest&) -> HttpResponse {
        return HttpResponse::ok("Hello, hical!");
    });
    
    // 添加日志中间件
    server.use([](HttpRequest& req, MiddlewareNext next)
                   -> Awaitable<HttpResponse> {
        std::cout << req.path() << std::endl;
        co_return co_await next(req);
    });
    
    // 启用 SSL（可选）
    server.enableSsl("server.crt", "server.key");
    
    // 启动服务器
    server.start();
    return 0;
}
```

---

### Router

路由管理器，负责路由注册和请求分发。静态路由使用透明哈希表 O(1) 查找（`RouteKeyView` + `is_transparent` 实现零分配 `string_view` 查找），参数路由按 HTTP 方法分桶存储。

**头文件：** `<hical/core/Router.h>`

#### 公共方法

| 方法                                                    | 参数                                                                             | 返回值                    | 说明                           |
| ------------------------------------------------------- | -------------------------------------------------------------------------------- | ------------------------- | ------------------------------ |
| `route(method, path, handler)`                          | method: HTTP 方法<br>path: 路由路径<br>handler: 协程处理器                       | `void`                    | 注册协程路由                   |
| `route(method, path, handler)`                          | method: HTTP 方法<br>path: 路由路径<br>handler: 同步处理器                       | `void`                    | 注册同步路由（自动包装为协程） |
| `get(path, handler)`                                    | path: 路由路径<br>handler: 处理器                                                | `void`                    | 注册 GET 路由                  |
| `post(path, handler)`                                   | path: 路由路径<br>handler: 处理器                                                | `void`                    | 注册 POST 路由                 |
| `put(path, handler)`                                    | path: 路由路径<br>handler: 处理器                                                | `void`                    | 注册 PUT 路由                  |
| `del(path, handler)`                                    | path: 路由路径<br>handler: 处理器                                                | `void`                    | 注册 DELETE 路由               |
| `ws(path, onMessage, onConnect)`                        | path: 路由路径<br>onMessage: 消息回调<br>onConnect: 连接回调（可选）             | `void`                    | 注册 WebSocket 路由            |
| `ws(path, options, onMessage, onConnect, onDisconnect)` | path: 路由路径<br>options: `WsOptions`<br>onMessage/onConnect/onDisconnect: 回调 | `void`                    | 注册带选项的 WebSocket 路由    |
| `dispatch(req)`                                         | req: HTTP 请求                                                                   | `Awaitable<HttpResponse>` | 分发请求到匹配的路由           |
| `routeCount()`                                          | 无                                                                               | `size_t`                  | 获取已注册路由数量             |
| `group(prefix)`                                         | prefix: 路由前缀                                                                 | `RouteGroup`              | 创建路由组（前缀分组）         |

#### 路径参数

路径中使用 `{参数名}` 定义参数，如 `/users/{id}`。匹配时参数会被提取到 `HttpRequest` 中。

#### 示例

```cpp
#include <hical/core/HttpServer.h>

using namespace hical;

Router router;

// 静态路由
router.get("/api/status", [](const HttpRequest&) -> HttpResponse {
    return HttpResponse::json({{"status", "ok"}});
});

// 参数路由
router.get("/users/{id}", [](const HttpRequest& req) -> HttpResponse {
    auto userId = req.param("id");
    return HttpResponse::json({{"userId", userId}});
});

// 协程路由
router.post("/async", [](const HttpRequest& req) -> Awaitable<HttpResponse> {
    co_await hical::sleep(0.1);
    co_return HttpResponse::ok("Done");
});

// WebSocket 路由
router.ws("/ws/chat",
    [](const std::string& msg, WebSocketSession& ws) -> Awaitable<void> {
        co_await ws.send("Echo: " + msg);
    }
);

// WebSocket 路由（带 Origin 白名单）
WsOptions wsOpts;
wsOpts.allowedOrigins = {"https://example.com"};
router.ws("/ws/chat", wsOpts,
    [](const std::string& msg, WebSocketSession& ws) -> Awaitable<void> {
        co_await ws.send("Echo: " + msg);
    }
);
```

---

### RouteGroup

路由组，为一组路由设置公共前缀和组级中间件。支持多层嵌套。

**头文件：** `<hical/core/RouteGroup.h>`

#### 公共方法

| 方法                           | 参数                     | 返回值       | 说明                                 |
| ------------------------------ | ------------------------ | ------------ | ------------------------------------ |
| `use(middleware)`              | middleware: 中间件处理器 | `void`       | 添加组级中间件（仅对组内路由生效）   |
| `group(subPrefix)`             | subPrefix: 子前缀        | `RouteGroup` | 创建嵌套子组（继承父组中间件和前缀） |
| `route(method, path, handler)` | method/path/handler      | `void`       | 注册路由（协程/同步）                |
| `get(path, handler)`           | path/handler             | `void`       | 注册 GET 路由                        |
| `post(path, handler)`          | path/handler             | `void`       | 注册 POST 路由                       |
| `put(path, handler)`           | path/handler             | `void`       | 注册 PUT 路由                        |
| `del(path, handler)`           | path/handler             | `void`       | 注册 DELETE 路由                     |

#### 示例

```cpp
#include <hical/core/RouteGroup.h>

auto api = server.router().group("/api/v1");
api.use(authMiddleware);      // 仅对 /api/v1/* 生效

api.get("/users", listUsers);           // → GET /api/v1/users
api.post("/users", createUser);         // → POST /api/v1/users
api.get("/users/{id}", getUser);        // → GET /api/v1/users/{id}

// 嵌套子组
auto admin = api.group("/admin");
admin.use(adminAuthMiddleware);
admin.get("/stats", getStats);          // → GET /api/v1/admin/stats
```

---

### HttpRequest

HTTP 请求封装，对原生 HTTP 解析结果的 hical 风格封装。

**头文件：** `<hical/core/HttpRequest.h>`

#### 公共方法

| 方法             | 参数             | 返回值                      | 说明                                        |
| ---------------- | ---------------- | --------------------------- | ------------------------------------------- |
| `method()`       | 无               | `HttpMethod`                | 获取 HTTP 方法                              |
| `path()`         | 无               | `std::string_view`          | 获取请求路径（不含查询参数）                |
| `target()`       | 无               | `std::string_view`          | 获取完整 URI（含查询参数）                  |
| `query()`        | 无               | `std::string_view`          | 获取查询字符串（? 后面的部分）              |
| `header(name)`   | name: 头部字段名 | `std::string_view`          | 获取指定头部字段值                          |
| `body()`         | 无               | `const std::string&`        | 获取消息体                                  |
| `jsonBody()`     | 无               | `const boost::json::value&` | 将消息体解析为 JSON（多次调用返回缓存引用） |
| `readJson<T>()`  | 无               | `T`                         | 将消息体反序列化为 T（需 `HICAL_JSON`）     |
| `contentType()`  | 无               | `std::string_view`          | 获取 Content-Type 头                        |
| `param(name)`    | name: 参数名     | `const std::string&`        | 获取路径参数                                |
| `hasParam(name)` | name: 参数名     | `bool`                      | 是否有指定路径参数                          |
| `native()`       | 无               | `NativeRequest&`            | 获取底层原生请求引用                        |

#### 查询参数 API

| 方法                  | 参数         | 返回值                                                     | 说明                                |
| --------------------- | ------------ | ---------------------------------------------------------- | ----------------------------------- |
| `queryParam(name)`    | name: 参数名 | `std::optional<std::string>`                               | 获取指定查询参数值（惰性解析+缓存） |
| `queryParams()`       | 无           | `const std::unordered_multimap<std::string, std::string>&` | 获取所有查询参数                    |
| `hasQueryParam(name)` | name: 参数名 | `bool`                                                     | 是否有指定查询参数                  |

#### 表单参数 API

| 方法                 | 参数         | 返回值                                                     | 说明                                                    |
| -------------------- | ------------ | ---------------------------------------------------------- | ------------------------------------------------------- |
| `formParam(name)`    | name: 参数名 | `std::optional<std::string>`                               | 获取指定表单参数值（application/x-www-form-urlencoded） |
| `formParams()`       | 无           | `const std::unordered_multimap<std::string, std::string>&` | 获取所有表单参数                                        |
| `hasFormParam(name)` | name: 参数名 | `bool`                                                     | 是否有指定表单参数                                      |

#### Cookie API

| 方法              | 参数            | 返回值                                               | 说明                              |
| ----------------- | --------------- | ---------------------------------------------------- | --------------------------------- |
| `cookie(name)`    | name: Cookie 名 | `const std::string&`                                 | 获取指定 Cookie 值（懒解析+缓存） |
| `cookies()`       | 无              | `const std::unordered_map<std::string,std::string>&` | 获取所有 Cookie（同名取第一个值） |
| `hasCookie(name)` | name: Cookie 名 | `bool`                                               | 是否存在指定 Cookie               |

#### 请求级属性 API（中间件间传递数据）

| 方法                   | 参数                                | 返回值                    | 说明                                                         |
| ---------------------- | ----------------------------------- | ------------------------- | ------------------------------------------------------------ |
| `setAttribute(key, v)` | key: `string_view`<br>v: 任意类型值 | `void`                    | 设置请求级属性（中间件间数据传递，如 trace_id、db 连接等）   |
| `getAttribute<T>(key)` | key: `string_view`                  | `std::optional<T>`        | 获取类型安全的请求属性（类型不匹配返回 `nullopt`，不走异常） |
| `getAttribute(key)`    | key: `string_view`                  | `std::optional<std::any>` | 获取原始 `std::any` 请求属性                                 |

**框架预留的属性键：**

| 键                    | 类型                                          | 设置者                   |
| --------------------- | --------------------------------------------- | ------------------------ |
| `"hical.session"`     | `std::shared_ptr<Session>`                    | `makeSessionMiddleware`  |
| `"hical.trace_id"`    | `std::string`                                 | `makeLogMiddleware`      |
| `"hical.db.conn"`     | `std::shared_ptr<DbConnection>`               | `makeDbMiddleware`       |
| `"hical.db.pool"`     | `std::shared_ptr<DbConnectionPool>`           | `makeDbMiddleware`       |
| `"hical.db.queryLog"` | `std::shared_ptr<std::vector<QueryLogEntry>>` | `makeQueryLogMiddleware` |
| `"jwt.payload"`       | `boost::json::object`                         | `makeJwtAuthMiddleware`  |

#### 构建请求的方法

| 方法                     | 参数                                               | 返回值 | 说明                               |
| ------------------------ | -------------------------------------------------- | ------ | ---------------------------------- |
| `setMethod(method)`      | method: HTTP 方法                                  | `void` | 设置 HTTP 方法                     |
| `setTarget(target)`      | target: 目标 URI                                   | `void` | 设置请求路径                       |
| `setHeader(name, value)` | name: 字段名<br>value: 字段值 (`std::string_view`) | `void` | 设置头部字段（拒绝含 CR/LF 的值）  |
| `setBody(body)`          | body: 消息体                                       | `void` | 设置消息体（不动 Content-Type 头） |
| `setParam(name, value)`  | name: 参数名<br>value: 参数值                      | `void` | 设置路径参数（由 Router 内部调用） |

#### 示例

```cpp
void handler(const HttpRequest& req)
{
    auto method = req.method();
    auto path = req.path();
    auto auth = req.header("Authorization");
    
    // 解析 JSON 请求体为结构体
    auto user = req.readJson<UserDTO>();
    
    // 读取路径参数
    if (req.hasParam("id")) {
        auto userId = req.param("id");
    }
    
    // 查询参数
    auto page = req.queryParam("page");  // std::optional<std::string>
}
```

---

### HttpResponse

HTTP 响应封装，对原生 HTTP 响应的 hical 风格封装。

**头文件：** `<hical/core/HttpResponse.h>`

#### 公共方法

| 方法                           | 参数                                                       | 返回值               | 说明                          |
| ------------------------------ | ---------------------------------------------------------- | -------------------- | ----------------------------- |
| `statusCode()`                 | 无                                                         | `HttpStatusCode`     | 获取状态码                    |
| `setStatus(code)`              | code: 状态码                                               | `void`               | 设置状态码                    |
| `header(name)`                 | name: 字段名                                               | `std::string_view`   | 获取指定头部字段值            |
| `setHeader(name, value)`       | name: 字段名<br>value: 字段值 (`std::string_view`)         | `void`               | 设置头部字段                  |
| `body()`                       | 无                                                         | `const std::string&` | 获取消息体                    |
| `setBody(body, contentType)`   | body: 消息体<br>contentType: Content-Type                  | `void`               | 设置消息体并指定 Content-Type |
| `setJsonBody(json)`            | json: JSON 值                                              | `void`               | 设置 JSON 消息体              |
| `setCookie(name, value, opts)` | name: Cookie 名<br>value: Cookie 值<br>opts: CookieOptions | `void`               | 追加 Set-Cookie 响应头        |
| `native()`                     | 无                                                         | `NativeResponse&`    | 获取底层原生响应引用          |

#### 工厂方法

| 方法                            | 参数                                                   | 返回值         | 说明                                                                        |
| ------------------------------- | ------------------------------------------------------ | -------------- | --------------------------------------------------------------------------- |
| `ok(body)`                      | body: 消息体（默认空）                                 | `HttpResponse` | 创建 200 OK 响应                                                            |
| `json(json)`                    | json: JSON 值                                          | `HttpResponse` | 创建 JSON 200 OK 响应                                                       |
| `notFound()`                    | 无                                                     | `HttpResponse` | 创建 404 Not Found 响应                                                     |
| `badRequest(message)`           | message: 错误信息（默认 "Bad Request"）                | `HttpResponse` | 创建 400 Bad Request 响应                                                   |
| `serverError()`                 | 无                                                     | `HttpResponse` | 创建 500 Internal Server Error 响应                                         |
| `redirect(location, code)`      | location: 重定向 URL<br>code: 状态码（默认 302 Found） | `HttpResponse` | 创建重定向响应（Location 头经 CRLF 防护）                                   |
| `rangeNotSatisfiable(fileSize)` | fileSize: 资源总大小                                   | `HttpResponse` | 创建 416 Range Not Satisfiable 响应（含 `Content-Range: bytes */fileSize`） |

#### 示例

```cpp
HttpResponse handler(const HttpRequest& req)
{
    // 使用工厂方法
    return HttpResponse::json({{"status", "ok"}});
    return HttpResponse::notFound();
    
    // 手动构建
    HttpResponse res;
    res.setStatus(HttpStatusCode::hCreated);
    res.setHeader("X-Custom", "value");
    res.setBody("Created", "text/plain");
    return res;
    
    // 设置 Cookie
    CookieOptions opts;
    opts.maxAge = 3600;
    HttpResponse res2 = HttpResponse::ok("ok");
    res2.setCookie("token", "abc123", opts);
    return res2;
}
```

---

### HeaderMap

HTTP 头部容器，大小写不敏感查找。内部使用 `vector<pair<string,string>>` 存储，典型 <20 个头部下线性扫描在 L1 缓存内完成，性能优于 hash map。

**头文件：** `<hical/core/HeaderMap.h>`

#### 类型定义

```cpp
using Entry = std::pair<std::string, std::string>;
using Container = std::vector<Entry>;
using iterator = Container::iterator;
using const_iterator = Container::const_iterator;
```

#### 公共方法

| 方法                  | 参数                            | 返回值                          | 说明                                         |
| --------------------- | ------------------------------- | ------------------------------- | -------------------------------------------- |
| `find(name)`          | name: 头部名称                  | `std::string_view`              | 查找首个匹配值（大小写不敏感），未找到返回空 |
| `findAll(name)`       | name: 头部名称                  | `std::vector<std::string_view>` | 查找所有匹配值                               |
| `set(name, value)`    | name: 头部名称<br>value: 头部值 | `void`                          | 覆写首个匹配，不存在则追加                   |
| `insert(name, value)` | name: 头部名称<br>value: 头部值 | `void`                          | 始终追加（用于 Set-Cookie 等多值字段）       |
| `erase(name)`         | name: 头部名称                  | `void`                          | 删除所有匹配的头部                           |
| `contains(name)`      | name: 头部名称                  | `bool`                          | 是否包含指定头部                             |
| `count(name)`         | name: 头部名称                  | `size_t`                        | 统计指定名称的头部数量                       |
| `size()`              | 无                              | `size_t`                        | 头部总数                                     |
| `empty()`             | 无                              | `bool`                          | 是否为空                                     |
| `reserve(n)`          | n: 预留容量                     | `void`                          | 预分配空间                                   |
| `clear()`             | 无                              | `void`                          | 清空所有头部                                 |
| `begin()` / `end()`   | 无                              | `iterator` / `const_iterator`   | 迭代器                                       |

#### 静态方法

| 方法            | 参数             | 返回值 | 说明                   |
| --------------- | ---------------- | ------ | ---------------------- |
| `iequals(a, b)` | 两个 string_view | `bool` | 大小写不敏感字符串比较 |

---

### HttpTypes

HTTP 类型定义，包含方法枚举、状态码枚举和转换函数。

**头文件：** `<hical/core/HttpTypes.h>`

#### HttpMethod 枚举

| 枚举值     | 说明         |
| ---------- | ------------ |
| `hGet`     | GET 方法     |
| `hPost`    | POST 方法    |
| `hPut`     | PUT 方法     |
| `hDelete`  | DELETE 方法  |
| `hPatch`   | PATCH 方法   |
| `hHead`    | HEAD 方法    |
| `hOptions` | OPTIONS 方法 |
| `hUnknown` | 未知方法     |

#### HttpStatusCode 枚举

| 枚举值                          | 数值 | 说明                   |
| ------------------------------- | ---- | ---------------------- |
| `hOk`                           | 200  | 成功                   |
| `hCreated`                      | 201  | 已创建                 |
| `hAccepted`                     | 202  | 已接受                 |
| `hNoContent`                    | 204  | 无内容                 |
| `hPartialContent`               | 206  | 部分内容（Range）      |
| `hMovedPermanently`             | 301  | 永久重定向             |
| `hFound`                        | 302  | 临时重定向             |
| `hNotModified`                  | 304  | 未修改                 |
| `hTemporaryRedirect`            | 307  | 临时重定向（保持方法） |
| `hPermanentRedirect`            | 308  | 永久重定向（保持方法） |
| `hBadRequest`                   | 400  | 错误请求               |
| `hUnauthorized`                 | 401  | 未授权                 |
| `hForbidden`                    | 403  | 禁止访问               |
| `hNotFound`                     | 404  | 未找到                 |
| `hMethodNotAllowed`             | 405  | 方法不允许             |
| `hConflict`                     | 409  | 冲突                   |
| `hPayloadTooLarge`              | 413  | 请求体过大             |
| `hRequestedRangeNotSatisfiable` | 416  | Range 无法满足         |
| `hTooManyRequests`              | 429  | 请求过多               |
| `hInternalServerError`          | 500  | 服务器内部错误         |
| `hNotImplemented`               | 501  | 未实现                 |
| `hBadGateway`                   | 502  | 网关错误               |
| `hServiceUnavailable`           | 503  | 服务不可用             |

#### 转换函数

| 函数                           | 参数              | 返回值        | 说明                      |
| ------------------------------ | ----------------- | ------------- | ------------------------- |
| `httpMethodToString(method)`   | method: HTTP 方法 | `const char*` | 转换为字符串（如 "GET"）  |
| `stringToHttpMethod(str)`      | str: 方法名字符串 | `HttpMethod`  | 从字符串转换              |
| `httpStatusCodeToString(code)` | code: 状态码      | `const char*` | 获取状态码描述（如 "OK"） |

---

### Middleware

中间件系统，采用洋葱模型，按注册顺序执行。

**头文件：** `<hical/core/Middleware.h>`

#### 类型定义

```cpp
// 异步中间件
using MiddlewareNext    = std::function<Awaitable<HttpResponse>(HttpRequest&)>;
using MiddlewareHandler = std::function<Awaitable<HttpResponse>(HttpRequest&, MiddlewareNext)>;

// 同步中间件（零协程帧开销）
using SyncMiddlewareResult = std::optional<HttpResponse>;
using SyncBeforeHandler    = std::function<SyncMiddlewareResult(HttpRequest&)>;
using SyncAfterHandler     = std::function<void(HttpRequest&, HttpResponse&)>;
```

#### MiddlewareEntry 结构体

同步与异步中间件的统一容器，由 `buildOptimizedChain()` 合并连续同步条目。

| 字段           | 类型                | 说明                        |
| -------------- | ------------------- | --------------------------- |
| `type`         | `Type` (Async/Sync) | 中间件类型                  |
| `name`         | `std::string`       | 中间件名称（调试用）        |
| `asyncHandler` | `MiddlewareHandler` | 异步处理器（仅 Async 有值） |
| `before`       | `SyncBeforeHandler` | 同步前置处理器（仅 Sync）   |
| `after`        | `SyncAfterHandler`  | 同步后置处理器（仅 Sync）   |

#### MiddlewarePipeline 类

| 方法                                  | 参数                                               | 返回值                    | 说明                                   |
| ------------------------------------- | -------------------------------------------------- | ------------------------- | -------------------------------------- |
| `use(middleware)`                     | middleware: 异步中间件                             | `void`                    | 添加异步中间件（`build()` 后禁止调用） |
| `use(name, middleware)`               | name: 名称<br>middleware: 异步中间件               | `void`                    | 添加命名异步中间件                     |
| `use(before)`                         | before: 同步前置处理器                             | `void`                    | 添加同步前置中间件                     |
| `use(before, after)`                  | before/after: 同步前后处理器                       | `void`                    | 添加同步前后处理器对                   |
| `use(name, before, after)`            | name/before/after                                  | `void`                    | 添加命名同步前后处理器对               |
| `build(finalHandler)`                 | finalHandler: 最终处理器                           | `void`                    | 预构建调用链（仅调用一次）             |
| `buildFor(finalHandler)`              | finalHandler: 最终处理器                           | `MiddlewareNext`          | 预构建并返回可缓存的调用链             |
| `execute(req)`                        | req: HTTP 请求                                     | `Awaitable<HttpResponse>` | 执行预构建缓存链（需先 `build()`）     |
| `execute(req, finalHandler)`          | req: HTTP 请求<br>finalHandler: 最终处理器         | `Awaitable<HttpResponse>` | 动态构建并执行                         |
| `size()`                              | 无                                                 | `size_t`                  | 获取中间件数量                         |
| `buildChainFrom(handlers, final)`     | handlers: 中间件列表<br>final: 最终处理器          | `MiddlewareNext`          | 静态方法：从异步 handler 列表构建链    |
| `buildOptimizedChain(entries, final)` | entries: MiddlewareEntry 列表<br>final: 最终处理器 | `MiddlewareNext`          | 静态方法：合并连续同步中间件为单协程帧 |

#### 示例

```cpp
#include <hical/core/Middleware.h>

using namespace hical;

// 日志中间件
auto logger = [](HttpRequest& req, MiddlewareNext next)
                  -> Awaitable<HttpResponse> {
    std::cout << req.path() << std::endl;
    auto res = co_await next(req);  // 调用下一层
    co_return res;
};

// 认证中间件（拦截未授权请求）
auto auth = [](HttpRequest& req, MiddlewareNext next)
                -> Awaitable<HttpResponse> {
    if (req.header("Authorization").empty()) {
        co_return HttpResponse::badRequest("需要认证");
    }
    co_return co_await next(req);
};

server.use(logger);
server.use(auth);
```

---

### MetaJson

C++26 反射 / C++20 宏双轨的 JSON 自动序列化/反序列化系统。

**头文件：** `<hical/core/MetaJson.h>`

#### 函数（`namespace hical::meta`）

| 函数                      | 参数                        | 返回值                | 说明                                       |
| ------------------------- | --------------------------- | --------------------- | ------------------------------------------ |
| `toJson(obj)`             | obj: 带 `HICAL_JSON` 的对象 | `boost::json::object` | 序列化结构体为 JSON                        |
| `fromJson<T>(json)`       | json: `boost::json::value`  | `T`                   | 反序列化 JSON 为结构体（类型不匹配抛异常） |
| `readJson<T>(req)`        | req: `HttpRequest&`         | `T`                   | 从请求体反序列化（JSON 无效时抛异常）      |
| `toJsonSnakeCase<T>(obj)` | obj: 结构体对象             | `boost::json::object` | 序列化时 key 自动转 snake_case（仅 C++26） |

> `HttpRequest::readJson<T>()` 是 `meta::readJson<T>(req)` 的便捷成员函数包装。

#### 宏（C++20 回退模式）

| 宏                              | 用途                                     |
| ------------------------------- | ---------------------------------------- |
| `HICAL_JSON(Type, field1, ...)` | 声明参与 JSON 序列化的字段（支持装饰器） |
| `ALIAS(field, "json_key")`      | 自定义 JSON key                          |
| `REQUIRED(field)`               | 反序列化时必须存在，否则抛异常           |
| `REQUIRED_ALIAS(field, "key")`  | 必需 + 自定义 key 的组合                 |
| `HICAL_IGNORE(field)`           | 序列化/反序列化均跳过                    |
| `MIN(field, val)`               | 数值最小值校验（反序列化时）             |
| `MAX(field, val)`               | 数值最大值校验（反序列化时）             |
| `NOT_EMPTY(field)`              | 字符串非空校验                           |
| `PATTERN(field, "re")`          | 正则匹配校验                             |
| `LENGTH(field, min, max)`       | 字符串长度范围校验                       |

#### C++26 反射属性（`HICAL_HAS_REFLECTION == 1` 时）

```cpp
struct User
{
    [[hical::json_required]]        int id;
    [[hical::json_name("userName")]] std::string name;
    [[hical::json_ignore]]          std::string internal;
};
```

#### 示例

```cpp
#include <hical/core/MetaJson.h>

using namespace hical;

struct UserDTO
{
    HICAL_JSON(UserDTO, REQUIRED(id), name, ALIAS(emailAddr, "email"))
    int id;
    std::string name;
    std::string emailAddr;
};

// 序列化
UserDTO user{42, "Alice", "alice@example.com"};
auto json = meta::toJson(user);
// {"id":42,"name":"Alice","email":"alice@example.com"}

// 反序列化
auto parsed = meta::fromJson<UserDTO>(jsonValue);

// 从 HTTP 请求体
server.router().post("/users", [](const HttpRequest& req) -> HttpResponse {
    auto dto = req.readJson<UserDTO>();  // 自动解析
    return HttpResponse::json(meta::toJson(dto));
});
```

---

### MetaRoutes

C++26 反射 / C++20 宏双轨的自动路由注册系统。

**头文件：** `<hical/core/MetaRoutes.h>`

#### 函数（`namespace hical::meta`）

| 函数                               | 参数                                         | 说明                                     |
| ---------------------------------- | -------------------------------------------- | ---------------------------------------- |
| `registerRoutes(router, pHandler)` | router: Router&<br>pHandler: shared_ptr 版本 | 注册所有标注的路由（生命周期安全）       |
| `registerRoutes(router, handler)`  | router: Router&<br>handler: 引用版本         | 注册所有标注的路由（调用者管理生命周期） |

#### 宏（C++20 回退模式）

```cpp
// 在 struct 内标注单个路由处理器
HICAL_HANDLER(Method, "/path", funcName)
// Method: Get / Post / Put / Delete / Patch

// 收集所有路由，生成路由表
HICAL_ROUTES(Type, func1, func2, ...)
```

#### C++26 反射属性

```cpp
struct Handler
{
    [[hical::route("/users/{id}", "GET")]]
    Awaitable<HttpResponse> getUser(const HttpRequest& req) { ... }
};
```

#### RouteInfo 结构体

> 定义位于 `<hical/core/Reflection.h>`，由 `MetaRoutes.h` 重新导出。

| 字段          | 类型               | 说明                 |
| ------------- | ------------------ | -------------------- |
| `method`      | `HttpMethod`       | HTTP 方法            |
| `path`        | `std::string_view` | 路由路径             |
| `handlerName` | `std::string_view` | 成员函数名（调试用） |

#### 类型萃取

| 萃取               | 说明                        |
| ------------------ | --------------------------- |
| `HasRouteTable<T>` | T 是否使用了 `HICAL_ROUTES` |
| `HasJsonFields<T>` | T 是否使用了 `HICAL_JSON`   |

#### 示例

```cpp
#include <hical/core/MetaRoutes.h>
#include <hical/core/MetaJson.h>

using namespace hical;

struct UserHandler
{
    HICAL_HANDLER(Get, "/api/users/{id}", getUser)
    Awaitable<HttpResponse> getUser(const HttpRequest& req)
    {
        co_return HttpResponse::json({{"id", req.param("id")}});
    }

    HICAL_HANDLER(Post, "/api/users", createUser)
    Awaitable<HttpResponse> createUser(const HttpRequest& req)
    {
        auto dto = req.readJson<CreateUserDTO>();
        co_return HttpResponse::json(meta::toJson(dto));
    }

    HICAL_ROUTES(UserHandler, getUser, createUser)
};

int main()
{
    HttpServer server(8080);
    auto handler = std::make_shared<UserHandler>();
    meta::registerRoutes(server.router(), handler);
    server.start();
}
```

---

### Cookie

Cookie 解析（请求侧）与设置（响应侧）支持，符合 RFC 6265。

**头文件：** `<hical/core/Cookie.h>`（CookieOptions），`<hical/core/HttpRequest.h>`，`<hical/core/HttpResponse.h>`

#### CookieOptions 结构体

| 字段       | 类型          | 默认值  | 说明                                   |
| ---------- | ------------- | ------- | -------------------------------------- |
| `path`     | `std::string` | `"/"`   | Cookie 作用路径                        |
| `domain`   | `std::string` | `""`    | Cookie 作用域（空=当前域）             |
| `maxAge`   | `int`         | `-1`    | 有效期（秒），-1 表示会话 Cookie       |
| `httpOnly` | `bool`        | `true`  | 防 XSS：禁止 JS 访问                   |
| `secure`   | `bool`        | `true`  | 仅 HTTPS 传输（开发环境需显式关闭）    |
| `sameSite` | `std::string` | `"Lax"` | SameSite 策略（`Lax`/`Strict`/`None`） |

#### 示例

```cpp
// 读取 Cookie
auto token = req.cookie("token");
if (req.hasCookie("session_id")) { ... }

// 设置 Cookie
CookieOptions opts;
opts.maxAge   = 3600;
opts.httpOnly = true;

HttpResponse res = HttpResponse::ok("ok");
res.setCookie("token", "user_jwt_here", opts);
```

---

### Session

内存 Session 会话管理，通过中间件自动与 Cookie 联动。

**头文件：** `<hical/core/Session.h>`

#### SessionOptions 结构体

| 字段          | 类型          | 默认值            | 说明                        |
| ------------- | ------------- | ----------------- | --------------------------- |
| `cookieName`  | `std::string` | `"HICAL_SESSION"` | Session Cookie 名称         |
| `maxAge`      | `int`         | `3600`            | Session 有效期（秒）        |
| `httpOnly`    | `bool`        | `true`            | Cookie HttpOnly             |
| `secure`      | `bool`        | `true`            | Cookie Secure（仅 HTTPS）   |
| `sameSite`    | `std::string` | `"Lax"`           | Cookie SameSite 策略        |
| `path`        | `std::string` | `"/"`             | Cookie 作用路径             |
| `gcInterval`  | `int`         | `300`             | 懒 GC 触发间隔（秒）        |
| `maxSessions` | `size_t`      | `100000`          | 最大 Session 数量（防 DoS） |

#### SessionManager 类（线程安全）

| 方法                   | 参数                         | 返回值                  | 说明                               |
| ---------------------- | ---------------------------- | ----------------------- | ---------------------------------- |
| `SessionManager(opts)` | opts: SessionOptions（可选） | —                       | 构造（默认选项）                   |
| `find(id)`             | id: Session ID               | `shared_ptr<Session>`   | 查找，不存在返回 nullptr           |
| `create()`             | 无                           | `shared_ptr<Session>`   | 创建新 Session（128-bit 随机 ID）  |
| `destroy(id)`          | id: Session ID               | `void`                  | 销毁指定 Session                   |
| `regenerate(oldId)`    | oldId: 旧 Session ID         | `shared_ptr<Session>`   | 重新生成 ID（防 Session 固定攻击） |
| `gc(force)`            | force: 是否强制（默认 true） | `void`                  | 回收过期 Session                   |
| `count()`              | 无                           | `size_t`                | 当前 Session 总数                  |
| `options()`            | 无                           | `const SessionOptions&` | 获取配置                           |

常量：`static constexpr const char* hSessionKey = "hical.session"` — 请求属性 key。

#### Session 类（线程安全）

| 方法             | 参数                     | 返回值                     | 说明                       |
| ---------------- | ------------------------ | -------------------------- | -------------------------- |
| `id()`           | 无                       | `const std::string&`       | 获取 Session ID            |
| `set(key, v)`    | key: 键<br>v: 任意类型值 | `void`                     | 设置属性                   |
| `get<T>(key)`    | key: 键                  | `std::optional<T>`         | 获取属性（类型安全）       |
| `has(key)`       | key: 键                  | `bool`                     | 检查属性是否存在           |
| `remove(key)`    | key: 键                  | `void`                     | 删除指定属性               |
| `clear()`        | 无                       | `void`                     | 清空所有属性               |
| `migrateFrom(o)` | o: 另一个 Session 引用   | `void`                     | 原子迁移数据（地址序双锁） |
| `markDirty()`    | 无                       | `void`                     | 标记脏位                   |
| `isDirty()`      | 无                       | `bool`                     | 是否有未持久化变更         |
| `touch()`        | 无                       | `void`                     | 更新 lastAccess 时间       |
| `lastAccess()`   | 无                       | `steady_clock::time_point` | 最后访问时间               |

#### makeSessionMiddleware 函数

```cpp
MiddlewareHandler makeSessionMiddleware(std::shared_ptr<SessionManager> manager);
```

#### 示例

```cpp
#include <hical/core/Session.h>

auto sessionMgr = std::make_shared<hical::SessionManager>();
server.use(hical::makeSessionMiddleware(sessionMgr));

server.router().post("/login", [](const HttpRequest& req) -> HttpResponse {
    auto session = req.getAttribute<std::shared_ptr<hical::Session>>(
        hical::SessionManager::hSessionKey);
    (*session)->set("user", std::string("alice"));
    return HttpResponse::ok("ok");
});
```

---

### CORS 中间件

跨域资源共享（CORS）中间件，符合 W3C CORS 规范。

**头文件：** `<hical/core/Cors.h>`

#### CorsOptions 结构体

| 字段               | 类型                       | 默认值                                            | 说明                           |
| ------------------ | -------------------------- | ------------------------------------------------- | ------------------------------ |
| `allowedOrigins`   | `std::vector<std::string>` | `{"*"}`                                           | 允许的源列表（`"*"` 为通配符） |
| `allowedMethods`   | `std::vector<std::string>` | `{"GET","POST","PUT","DELETE","PATCH","OPTIONS"}` | 允许的 HTTP 方法               |
| `allowedHeaders`   | `std::vector<std::string>` | `{"Content-Type","Authorization"}`                | 允许的请求头                   |
| `exposeHeaders`    | `std::vector<std::string>` | `{}`                                              | 暴露给浏览器的响应头           |
| `allowCredentials` | `bool`                     | `false`                                           | 是否允许凭证                   |
| `maxAge`           | `int`                      | `86400`                                           | 预检缓存时间（秒）             |

#### makeCorsMiddleware 函数

```cpp
MiddlewareHandler makeCorsMiddleware(CorsOptions opts = {});
```

#### 示例

```cpp
#include <hical/core/Cors.h>

// 允许所有源
server.use(makeCorsMiddleware());

// 精确匹配
CorsOptions opts;
opts.allowedOrigins   = {"https://example.com"};
opts.allowCredentials = true;
server.use(makeCorsMiddleware(opts));
```

---

### StaticFiles

静态文件服务工厂函数，将 URL 前缀映射到本地目录。

**头文件：** `<hical/core/StaticFiles.h>`

#### serveStatic 函数

```cpp
std::function<Awaitable<HttpResponse>(const HttpRequest&)> serveStatic(
    const std::string& rootDir,
    const std::string& urlPrefix,
    std::uintmax_t maxFileSize = 64ULL * 1024 * 1024);
```

**功能特性：** 异步文件 I/O、PathCache（4096 条目/60s TTL）、MIME 自动推断（27 种扩展名）、ETag/304、路径遍历防护（403）、大文件保护（413）、HTTP 206 Range 请求（单范围 `Range` / `If-Range` ETag 条件请求，200 响应自动添加 `Accept-Ranges: bytes`）。

#### 示例

```cpp
#include <hical/core/StaticFiles.h>

server.router().get("/static/{path}", hical::serveStatic("./public", "/static/"));
```

---

### Multipart

`multipart/form-data` 上传解析器（RFC 7578）。

**头文件：** `<hical/core/Multipart.h>`

#### MultipartPart 结构体

| 字段          | 类型          | 说明                         |
| ------------- | ------------- | ---------------------------- |
| `name`        | `std::string` | 字段名                       |
| `filename`    | `std::string` | 原始文件名（文件字段才有值） |
| `contentType` | `std::string` | Part 的 Content-Type         |
| `data`        | `std::string` | Part 数据内容（二进制安全）  |
| `isFile()`    | `bool`        | 是否为文件字段               |

#### MultipartParser 静态方法

| 方法                         | 参数                                   | 返回值                                      | 说明                                   |
| ---------------------------- | -------------------------------------- | ------------------------------------------- | -------------------------------------- |
| `parse(req)`                 | req: HTTP 请求                         | `std::optional<std::vector<MultipartPart>>` | 解析全部 Part（超 256 个返回 nullopt） |
| `getFile(parts, fieldName)`  | parts: 预解析结果<br>fieldName: 字段名 | `std::optional<MultipartPart>`              | 从预解析结果搜索文件 Part（推荐）      |
| `getField(parts, fieldName)` | parts: 预解析结果<br>fieldName: 字段名 | `std::optional<std::string>`                | 从预解析结果搜索文本字段（推荐）       |

#### 示例

```cpp
#include <hical/core/Multipart.h>

server.router().post("/upload", [](const HttpRequest& req) -> HttpResponse {
    auto parts = MultipartParser::parse(req);
    if (!parts) return HttpResponse::badRequest("无效数据");

    auto file = MultipartParser::getFile(*parts, "avatar");
    if (!file) return HttpResponse::badRequest("缺少 avatar");

    // file->filename / file->contentType / file->data
    return HttpResponse::ok("上传成功: " + file->filename);
});
```

---

### JWT Auth

JWT 认证中间件，基于 OpenSSL EVP 自实现 HMAC-SHA256（HS256）签发和验证，零第三方依赖。

**头文件：** `<hical/core/JwtAuth.h>`

#### JwtAuthOptions 结构体

| 字段          | 类型                       | 默认值 | 说明                                       |
| ------------- | -------------------------- | ------ | ------------------------------------------ |
| `secret`      | `std::string`              | `""`   | HMAC-SHA256 共享密钥                       |
| `issuer`      | `std::string`              | `""`   | Token 签发者（iss claim），为空不写入      |
| `audience`    | `std::string`              | `""`   | Token 受众（aud claim），为空不写入        |
| `tokenExpiry` | `std::chrono::seconds`     | `24h`  | Token 有效时长（仅 payload 无 exp 时生效） |
| `skipPaths`   | `std::vector<std::string>` | `{}`   | 白名单路径，跳过认证直接放行               |

#### 自由函数

| 函数                       | 参数                                                       | 返回值                | 说明                                                                        |
| -------------------------- | ---------------------------------------------------------- | --------------------- | --------------------------------------------------------------------------- |
| `jwtSign(payload, opts)`   | payload: `boost::json::object&`<br>opts: `JwtAuthOptions&` | `std::string`         | 签发 JWT Token（自动补 iat/exp/iss/aud）                                    |
| `jwtSign(payload, secret)` | payload: `boost::json::object&`<br>secret: `string_view`   | `std::string`         | 便捷重载，仅需 secret，exp 默认 24h                                         |
| `jwtVerify(token, secret)` | token: `string_view`<br>secret: `string_view`              | `boost::json::object` | 验证 Token，返回 payload（签名不匹配/过期/格式错误抛 `std::runtime_error`） |

#### 中间件工厂

```cpp
SyncBeforeHandler makeJwtAuthMiddleware(JwtAuthOptions opts);
```

返回 `SyncBeforeHandler`（零协程帧开销），行为：
1. 白名单路径（`skipPaths`）直接放行
2. 提取 `Authorization: Bearer <token>` 头（scheme 大小写不敏感，符合 RFC 7235）
3. 验证通过 → `req.setAttribute("jwt.payload", payload)` + 放行
4. 验证失败 → 返回 401

#### 示例

```cpp
#include <hical/core/JwtAuth.h>
#include <hical/core/HttpServer.h>

using namespace hical;

int main()
{
    HttpServer server(8080);

    // 1. 注册 JWT 中间件
    JwtAuthOptions jwtOpts;
    jwtOpts.secret = "my-secret-key";
    jwtOpts.issuer = "hical-api";
    jwtOpts.tokenExpiry = std::chrono::hours(4);
    jwtOpts.skipPaths = {"/public/health", "/api/login"};
    server.use(makeJwtAuthMiddleware(jwtOpts));

    // 2. 登录接口：签发 Token
    server.router().post("/api/login",
        [&jwtOpts](const HttpRequest& req) -> HttpResponse {
            auto body = req.jsonBody();
            boost::json::object payload;
            payload["sub"] = body.as_object()["username"];
            payload["role"] = "user";

            auto token = jwtSign(payload, jwtOpts);
            return HttpResponse::json({{"token", token}});
        });

    // 3. 受保护接口：从请求属性读取 payload
    server.router().get("/api/me",
        [](const HttpRequest& req) -> HttpResponse {
            auto jwt = req.getAttribute<boost::json::object>("jwt.payload");
            if (!jwt) return HttpResponse::badRequest();

            return HttpResponse::json({
                {"user", (*jwt)["sub"].as_string()},
                {"role", (*jwt)["role"].as_string()},
            });
        });

    server.start();
}
```

---

### ConfigLoader

JSON 配置加载器，支持层级 key 访问（点分隔）、环境变量覆盖（`HICAL_` 前缀 + 大写 + 下划线）和环境变量缓存。

**头文件：** `<hical/core/ConfigLoader.h>`

#### 构造函数

```cpp
ConfigLoader loader;
```

#### 方法

| 方法                      | 参数                         | 返回值                  | 说明                              |
| ------------------------- | ---------------------------- | ----------------------- | --------------------------------- |
| `loadFile(path)`          | path: `const std::string&`   | `bool`                  | 从文件加载 JSON 配置              |
| `loadString(json)`        | json: `std::string_view`     | `bool`                  | 从字符串加载 JSON 配置            |
| `get<T>(key, defaultVal)` | key + defaultVal: `const T&` | `T`                     | 获取配置值（优先 env，次 JSON）   |
| `setEnvPrefix(prefix)`    | prefix: `const std::string&` | —                       | 设置环境变量前缀，默认 `"HICAL_"` |
| `resolveEnv(key)`         | key: `const std::string&`    | `std::optional<string>` | 解析环境变量（带缓存，线程安全）  |

#### 支持的类型

`get<T>()` 模板支持以下类型：
- `int64_t` — 整数
- `std::string` — 字符串
- `bool` — 布尔值
- `double` — 浮点数
- `std::vector<std::string>` — 字符串数组

环境变量覆盖仅对标量类型（`int64_t`/`std::string`/`bool`/`double`）生效。

#### 环境变量转换规则

- 前缀：默认 `HICAL_`（可通过 `setEnvPrefix()` 修改）
- key 中的 `.` 转为 `_`，字母转大写
- 例：`"db.host"` → `HICAL_DB_HOST`

#### 示例

```cpp
#include <hical/core/ConfigLoader.h>

ConfigLoader loader;
loader.loadFile("config.json");

auto host = loader.get<std::string>("db.host", "127.0.0.1");
auto port = loader.get<int64_t>("db.port", 3306);
auto debug = loader.get<bool>("debug", false);

// 数组类型
auto allowlist = loader.get<std::vector<std::string>>("security.allowlist");

// 环境变量覆盖（优先级高于 JSON 文件）
// export HICAL_DB_HOST=10.0.0.1
// → loader.get<std::string>("db.host") 返回 "10.0.0.1"
```

---

### WebSocketSession

WebSocket 会话封装（RFC 6455），支持文本/二进制消息、心跳保活、子协议协商和 per-connection 上下文。

**头文件：** `<hical/core/WebSocket.h>`（枚举定义在 `<hical/core/WsFrame.h>`）

#### WsOpcode 枚举（`enum class : uint8_t`）

| 枚举值          | 值    | 说明     |
| --------------- | ----- | -------- |
| `hContinuation` | `0x0` | 延续帧   |
| `hText`         | `0x1` | 文本帧   |
| `hBinary`       | `0x2` | 二进制帧 |
| `hClose`        | `0x8` | 关闭帧   |
| `hPing`         | `0x9` | Ping 帧  |
| `hPong`         | `0xA` | Pong 帧  |

#### WsCloseCode 枚举（`enum class : uint16_t`）

| 枚举值                | 值     | 说明             |
| --------------------- | ------ | ---------------- |
| `hNormal`             | `1000` | 正常关闭         |
| `hGoingAway`          | `1001` | 端点关闭         |
| `hProtocolError`      | `1002` | 协议错误         |
| `hUnsupportedData`    | `1003` | 不支持的数据类型 |
| `hNoStatusReceived`   | `1005` | 无状态码（保留） |
| `hAbnormalClosure`    | `1006` | 异常关闭（保留） |
| `hInvalidPayload`     | `1007` | 无效载荷数据     |
| `hPolicyViolation`    | `1008` | 策略违规         |
| `hMessageTooBig`      | `1009` | 消息过大         |
| `hMandatoryExtension` | `1010` | 必需扩展未协商   |
| `hInternalError`      | `1011` | 服务端内部错误   |

#### 公共方法

| 方法                       | 参数                                    | 返回值                             | 说明                                |
| -------------------------- | --------------------------------------- | ---------------------------------- | ----------------------------------- |
| `send(msg)`                | msg: 文本消息                           | `Awaitable<void>`                  | 发送文本帧                          |
| `sendBinary(data)`         | data: `string_view`                     | `Awaitable<void>`                  | 发送二进制帧                        |
| `receive()`                | 无                                      | `Awaitable<optional<std::string>>` | 接收文本消息（关闭返回 nullopt）    |
| `receiveMessage()`         | 无                                      | `Awaitable<optional<WsMessage>>`   | 接收 typed 消息（区分 Text/Binary） |
| `sendPing(payload)`        | payload: Ping 载荷（≤125B）             | `Awaitable<void>`                  | 发送 Ping 帧                        |
| `closeAsync()`             | 无                                      | `Awaitable<void>`                  | 优雅关闭（Normal Closure）          |
| `closeAsync(code, reason)` | code: `WsCloseCode`<br>reason: 关闭原因 | `Awaitable<void>`                  | 优雅关闭（自定义关闭码）            |
| `close()`                  | 无                                      | `void`                             | 同步关闭（不发 close 帧）           |
| `isOpen()`                 | 无                                      | `bool`                             | 连接是否打开                        |
| `socket()`                 | 无                                      | `tcp::socket&`                     | 获取底层 socket 引用                |
| `compressionConfig()`      | 无                                      | `const WsCompressionConfig&`       | 获取压缩配置                        |
| `subprotocol()`            | 无                                      | `std::string_view`                 | 协商后的子协议                      |
| `setContext(ptr)`          | ptr: `shared_ptr<void>`                 | `void`                             | 设置 per-connection 上下文          |
| `getContext<T>()`          | 无                                      | `shared_ptr<T>`                    | 获取类型化上下文                    |
| `hasContext()`             | 无                                      | `bool`                             | 是否已设置上下文                    |
| `clearContext()`           | 无                                      | `void`                             | 清除上下文                          |
| `lastPongTime()`           | 无                                      | `steady_clock::time_point`         | 最后收到 Pong 的时间                |

#### WsMessage 结构体

```cpp
struct WsMessage
{
    WsOpcode type = WsOpcode::hText;  // hText 或 hBinary
    std::string data;
};
```

#### 回调类型

```cpp
using WsMessageCallback      = std::function<Awaitable<void>(const std::string&, WebSocketSession&)>;
using WsTypedMessageCallback = std::function<Awaitable<void>(const WsMessage&, WebSocketSession&)>;
using WsConnectCallback      = std::function<Awaitable<void>(WebSocketSession&)>;
using WsDisconnectCallback   = std::function<Awaitable<void>(WebSocketSession&)>;
```

#### 示例

```cpp
#include <hical/core/WebSocket.h>

// 带心跳 + 压缩 + 子协议
WsOptions wsOpts;
wsOpts.pingInterval = std::chrono::seconds(30);
wsOpts.enableCompression = true;
wsOpts.subprotocols = {"chat.v1"};

server.router().ws("/ws/chat", wsOpts,
    [](const std::string& msg, WebSocketSession& ws) -> Awaitable<void> {
        co_await ws.send("Echo: " + msg);
    },
    [](WebSocketSession& ws) -> Awaitable<void> {
        co_await ws.send("Welcome! Protocol: " + ws.subprotocol());
    }
);
```

---

### WsHub

WebSocket 连接管理器 / 广播中心，线程安全。

**头文件：** `<hical/core/WsHub.h>`

#### 公共方法

| 方法                                   | 参数                                    | 返回值           | 说明                  |
| -------------------------------------- | --------------------------------------- | ---------------- | --------------------- |
| `add(session)`                         | session: `shared_ptr<WebSocketSession>` | `WsConnectionId` | 注册连接，返回唯一 ID |
| `remove(id)`                           | id: 连接 ID                             | `void`           | 移除连接              |
| `join(id, room)`                       | id: 连接 ID<br>room: 房间名             | `void`           | 加入房间              |
| `leave(id, room)`                      | id: 连接 ID<br>room: 房间名             | `void`           | 离开房间              |
| `broadcast(room, message, exclude)`    | room/message/exclude                    | `void`           | 文本广播到房间        |
| `broadcastBinary(room, data, exclude)` | room/data/exclude                       | `void`           | 二进制广播到房间      |
| `broadcastAll(message, exclude)`       | message/exclude                         | `void`           | 广播到所有连接        |
| `sendTo(id, message)`                  | id/message                              | `void`           | 单播到指定连接        |
| `roomSize(room)`                       | room: 房间名                            | `size_t`         | 房间内连接数          |
| `connectionCount()`                    | 无                                      | `size_t`         | 总连接数              |

> Hub 存储 `weak_ptr`，不延长连接生命周期。**必须在 `onDisconnect` 回调中调用 `remove(id)`**。

---

### WsOptions

WebSocket 路由选项，配置安全策略、压缩、心跳和子协议。

**头文件：** `<hical/core/Router.h>`

| 字段                      | 类型                    | 默认值 | 说明                       |
| ------------------------- | ----------------------- | ------ | -------------------------- |
| `allowedOrigins`          | `unordered_set<string>` | 空     | Origin 白名单（空=不校验） |
| `enableCompression`       | `bool`                  | false  | 启用 permessage-deflate    |
| `serverMaxWindowBits`     | `int`                   | 15     | 服务端 zlib 窗口位数       |
| `clientMaxWindowBits`     | `int`                   | 15     | 客户端 zlib 窗口位数       |
| `serverNoContextTakeover` | `bool`                  | false  | 每消息独立压缩             |
| `pingInterval`            | `std::chrono::seconds`  | 0      | 心跳间隔（0=禁用）         |
| `maxMissedPongs`          | `uint32_t`              | 3      | 最大连续未收到 Pong 次数   |
| `subprotocols`            | `vector<string>`        | 空     | 支持的子协议列表           |

---

## 基础设施 API

### Coroutine

协程工具，提供协程类型别名和便捷函数。

**头文件：** `<hical/core/Coroutine.h>`

#### 类型别名

```cpp
template <typename T = void>
using Awaitable = boost::asio::awaitable<T>;
```

#### 函数

| 函数                                 | 参数                                 | 返回值            | 说明                   |
| ------------------------------------ | ------------------------------------ | ----------------- | ---------------------- |
| `sleep(seconds)`                     | seconds: 等待秒数                    | `Awaitable<void>` | 协程内等待             |
| `sleep(duration)`                    | duration: chrono 时长                | `Awaitable<void>` | 协程内等待             |
| `coSpawn(ioCtx, coroutine)`          | ioCtx: io_context<br>coroutine: 协程 | `void`            | 启动协程               |
| `coSpawn(ioCtx, coroutine, handler)` | ioCtx/coroutine/handler              | `void`            | 启动协程并设置完成回调 |

#### 示例

```cpp
#include <hical/core/Coroutine.h>

Awaitable<void> asyncTask()
{
    co_await hical::sleep(1.0);
    co_await hical::sleep(std::chrono::milliseconds(500));
}
```

---

### MemoryPool

全局 pmr 内存池管理器，提供三层内存分配策略。

**头文件：** `<hical/core/MemoryPool.h>`

#### PoolConfig 结构体

| 字段                           | 类型     | 默认值 | 说明                   |
| ------------------------------ | -------- | ------ | ---------------------- |
| `globalMaxBlocksPerChunk`      | `size_t` | 128    | 全局同步池每块最大块数 |
| `globalLargestPoolBlock`       | `size_t` | 1MB    | 全局同步池最大块大小   |
| `threadLocalMaxBlocksPerChunk` | `size_t` | 64     | 线程本地池每块最大块数 |
| `threadLocalLargestPoolBlock`  | `size_t` | 512KB  | 线程本地池最大块大小   |
| `requestPoolInitialSize`       | `size_t` | 4096   | 请求级池初始大小       |

#### MemoryPool 类

| 方法                             | 参数                | 返回值                                       | 说明                   |
| -------------------------------- | ------------------- | -------------------------------------------- | ---------------------- |
| `instance()`                     | 无                  | `MemoryPool&`                                | 获取全局单例           |
| `configure(config)`              | config: 池配置      | `void`                                       | 配置（须在首次使用前） |
| `globalAllocator()`              | 无                  | `pmr::polymorphic_allocator<byte>`           | 全局同步池分配器       |
| `threadLocalAllocator()`         | 无                  | `pmr::polymorphic_allocator<byte>`           | 线程本地池分配器       |
| `createRequestPool(initialSize)` | initialSize（可选） | `unique_ptr<pmr::monotonic_buffer_resource>` | 创建请求级单调池       |
| `getStats()`                     | 无                  | `Stats`                                      | 获取统计信息           |

#### 示例

```cpp
#include <hical/core/MemoryPool.h>

// 使用线程本地池
auto alloc = MemoryPool::instance().threadLocalAllocator();
std::pmr::vector<int> vec(alloc);

// 请求级单调池
auto pool = MemoryPool::instance().createRequestPool();
// pool 析构时整体释放
```

#### mimalloc 可选 upstream

通过 CMake 选项 `HICAL_WITH_MIMALLOC=ON` 启用，使用 mimalloc 替代默认 `new_delete_resource` 作为 PMR 最底层分配器（`MimallocResource`），在高并发场景下减少分配器锁竞争。

> **注意：** `createRequestPool()` 的 upstream 为 `threadLocalPool`，扩容路径零锁竞争。

---

### SslContext

SSL/TLS 上下文配置封装。

**头文件：** `<hical/core/SslContext.h>`

#### 公共方法

| 方法                        | 参数                   | 返回值          | 说明                     |
| --------------------------- | ---------------------- | --------------- | ------------------------ |
| `loadCertificate(certFile)` | certFile: 证书文件路径 | `void`          | 加载服务端证书（PEM）    |
| `loadPrivateKey(keyFile)`   | keyFile: 私钥文件路径  | `void`          | 加载服务端私钥（PEM）    |
| `loadCaCertificate(caFile)` | caFile: CA 证书路径    | `void`          | 加载 CA 证书             |
| `setVerifyPeer(verify)`     | verify: 是否验证对端   | `void`          | 设置对端验证             |
| `native()`                  | 无                     | `ssl::context&` | 获取底层 Asio SSL 上下文 |

---

### Error

错误处理模块，将 Boost.Asio 错误码统一映射为框架内部错误码。

**头文件：** `<hical/core/Error.h>`

#### ErrorCode 枚举（21 种）

`hNoError` / `hEof` / `hConnectionReset` / `hConnectionRefused` / `hTimedOut` / `hAddressInUse` / `hNetworkUnreachable` / `hOperationAborted` / `hBrokenPipe` / `hTooManyOpenFiles` / `hSslHandshakeError` / `hSslInvalidCertificate` / `hUnknown` 等。

#### NetworkError 结构体

| 字段      | 类型          | 说明     |
| --------- | ------------- | -------- |
| `code`    | `ErrorCode`   | 错误码   |
| `message` | `std::string` | 错误描述 |

| 方法              | 返回值 | 说明           |
| ----------------- | ------ | -------------- |
| `operator bool()` | `bool` | 是否有错误     |
| `ok()`            | `bool` | 是否无错误     |
| `isEof()`         | `bool` | 是否为 EOF     |
| `isCancelled()`   | `bool` | 是否为操作取消 |

#### 转换函数

| 函数                 | 参数             | 返回值         | 说明             |
| -------------------- | ---------------- | -------------- | ---------------- |
| `fromBoostError(ec)` | ec: Boost 错误码 | `ErrorCode`    | 转换为框架错误码 |
| `toNetworkError(ec)` | ec: Boost 错误码 | `NetworkError` | 转换为错误结构体 |

---

### InetAddress

IP 地址 + 端口封装（支持 IPv4/IPv6）。

**头文件：** `<hical/core/InetAddress.h>`

#### 公共方法

| 方法                     | 参数                                  | 返回值                   | 说明                  |
| ------------------------ | ------------------------------------- | ------------------------ | --------------------- |
| `InetAddress()`          | 无                                    | —                        | 默认构造（0.0.0.0:0） |
| `InetAddress(ip, port)`  | ip: IP 字符串<br>port: 端口号         | —                        | 指定 IP + 端口        |
| `InetAddress(addr)`      | addr: `sockaddr_in` 或 `sockaddr_in6` | —                        | 从原生地址构造        |
| `toIp()`                 | 无                                    | `std::string`            | IP 字符串             |
| `toIpPort()`             | 无                                    | `std::string`            | IP:Port 字符串        |
| `port()`                 | 无                                    | `uint16_t`               | 端口号                |
| `isIpV6()`               | 无                                    | `bool`                   | 是否 IPv6             |
| `getSockAddr()`          | 无                                    | `const struct sockaddr*` | 原生地址指针          |
| `setSockAddrInet4(addr)` | addr: `sockaddr_in`                   | `void`                   | 设置 IPv4 地址        |
| `setSockAddrInet6(addr)` | addr: `sockaddr_in6`                  | `void`                   | 设置 IPv6 地址        |

---

## 底层抽象

### PmrBuffer

基于 pmr 的统一缓冲区，支持 prepend 区域和自动扩容。

**头文件：** `<hical/core/PmrBuffer.h>`

**关键方法：** `peek()` / `readableBytes()` / `writableBytes()` / `retrieve(len)` / `append(data, len)` / `findCRLF()` / `swap(rhs)`。

---

### EventLoop

事件循环抽象接口。

**头文件：** `<hical/core/EventLoop.h>`

**关键方法：** `run()` / `stop()` / `dispatch(cb)` / `post(cb)` / `runAfter(delay, cb)` / `runEvery(interval, cb)` / `cancelTimer(id)` / `isInLoopThread()`。

---

### TcpConnection

TCP 连接抽象接口（基类）。被 `AsioConnection`/`GenericConnection<SocketType>` 实现。

**头文件：** `<hical/core/TcpConnection.h>`

#### 类型别名

```cpp
using Ptr                   = std::shared_ptr<TcpConnection>;
using MessageCallback       = std::function<void(const Ptr&, PmrBuffer*, /*time*/)>;
using ConnectionCallback    = std::function<void(const Ptr&)>;
using CloseCallback         = std::function<void(const Ptr&)>;
using WriteCompleteCallback = std::function<void(const Ptr&)>;
using HighWaterMarkCallback = std::function<void(const Ptr&, size_t)>;
```

#### 关键方法

| 方法                                                                             | 说明                                                                    |
| -------------------------------------------------------------------------------- | ----------------------------------------------------------------------- |
| `send(...)` 多种重载                                                             | 发送 `const char*` / `std::string` / `PmrBuffer` / `shared_ptr<string>` |
| `sendFile(path, offset=0, length=-1)`                                            | 异步发送文件                                                            |
| `shutdown()` / `close()`                                                         | 关闭连接                                                                |
| `setTcpNoDelay(bool)`                                                            | 启用/禁用 Nagle 算法                                                    |
| `startRead()` / `stopRead()`                                                     | 启停读循环                                                              |
| `connectEstablished()` / `connectDestroyed()`                                    | 框架内部生命周期回调                                                    |
| `connected()` / `disconnected()`                                                 | 状态查询                                                                |
| `localAddr()` / `peerAddr()`                                                     | 获取本端/对端 `InetAddress`                                             |
| `getLoop()`                                                                      | 获取所属 `EventLoop*`                                                   |
| `bytesSent()` / `bytesReceived()`                                                | 字节数统计                                                              |
| `lastActiveTime()`                                                               | 最后活跃时间（用于空闲检测）                                            |
| `onMessage` / `onConnection` / `onClose` / `onWriteComplete` / `onHighWaterMark` | 回调注册                                                                |
| `setContext(ptr)` / `getContext<T>()` / `hasContext()` / `clearContext()`        | per-connection 上下文                                                   |

---

### Timer

定时器抽象接口。

**头文件：** `<hical/core/Timer.h>`

| 方法            | 返回值       | 说明           |
| --------------- | ------------ | -------------- |
| `cancel()`      | `void`       | 取消定时器     |
| `isActive()`    | `bool`       | 是否活跃       |
| `getLoop()`     | `EventLoop*` | 所属事件循环   |
| `isRepeating()` | `bool`       | 是否周期触发   |
| `interval()`    | `double`     | 触发间隔（秒） |

---

### WriteNode

多态写缓冲节点，支持内存/文件混合发送队列。

**头文件：** `<hical/core/WriteNode.h>`

| 类                   | 关键方法                                                                        | 说明                       |
| -------------------- | ------------------------------------------------------------------------------- | -------------------------- |
| `WriteNode`（基类）  | `size() const` / `isFile() const` / `asBuffer() const`                          | 写节点抽象                 |
| `MemoryWriteNode`    | `MemoryWriteNode(shared_ptr<string>)` / `(string&&)`<br>`buffer()` / `data()`   | 内存节点                   |
| `PmrBufferWriteNode` | `PmrBufferWriteNode(PmrBuffer&&)` / `buffer()`                                  | PMR 缓冲节点               |
| `FileWriteNode`      | `FileWriteNode(path, offset=0, length=-1)` / `path()` / `offset()` / `length()` | 文件节点（zero-copy 候选） |

---

### WebSocket 协议层

`WsFrame.h` / `WsHandshake.h` / `WsDeflate.h` —— `WebSocketSession` 的底层实现，普通用户无需直接调用。

#### WsFrame.h

| 类型 / 函数                                                                                     | 说明                                                                                  |
| ----------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------- |
| `WsOpcode` 枚举                                                                                 | 见 [WebSocketSession](#websocketsession)                                              |
| `WsCloseCode` 枚举                                                                              | 见 [WebSocketSession](#websocketsession)                                              |
| `struct WsFrameHeader { fin, rsv1/2/3, masked, opcode, payloadLength, maskKey[4], headerSize }` | 帧头部解析结果                                                                        |
| `optional<WsFrameHeader> parseWsFrameHeader(const uint8_t*, size_t)`                            | 解析帧头                                                                              |
| `void unmaskPayload(uint8_t*, size_t, const uint8_t[4])`                                        | 解掩码                                                                                |
| `string buildWsFrame(opcode, payload, fin=true, rsv1=false)`                                    | 构造服务端帧（无掩码）。sendFrame() 已改用 frameBuf_ 内联构造，此函数留给其他调用点用 |
| `string buildMaskedWsFrame(opcode, payload, mask[4], fin, rsv1)`                                | 构造客户端帧（带掩码）                                                                |
| `string buildClosePayload(WsCloseCode, reason={})`                                              | 构造 close 帧载荷                                                                     |

#### WsHandshake.h

| 类型 / 函数                                                                                                                            | 说明                        |
| -------------------------------------------------------------------------------------------------------------------------------------- | --------------------------- |
| `struct WsDeflateNegotiation { accepted, serverMaxWindowBits, clientMaxWindowBits, serverNoContextTakeover, clientNoContextTakeover }` | 协商结果                    |
| `string computeWsAcceptKey(string_view clientKey)`                                                                                     | 计算 `Sec-WebSocket-Accept` |
| `string_view validateWsUpgrade(const NativeRequest&)`                                                                                  | 校验升级请求合法性          |
| `WsDeflateNegotiation negotiateDeflate(extHeader, WsCompressionConfig)`                                                                | 协商 permessage-deflate     |
| `string negotiateSubprotocol(clientOffer, serverSupported)`                                                                            | 协商子协议                  |
| `void buildWsAcceptResponse(FixedBuffer<512>&, acceptKey, deflateNeg=nullptr, subprotocol={})`                                         | 写握手响应到栈缓冲          |
| `string base64Encode(const uint8_t*, size_t)`                                                                                          | Base64 工具                 |

#### WsDeflate.h

`WsDeflateContext` —— permessage-deflate 压缩上下文（pimpl 封装 zlib，含 zip bomb 防护）：

```cpp
struct Config {
    int  serverMaxWindowBits     = 15;
    int  clientMaxWindowBits     = 15;
    bool serverNoContextTakeover = false;
    bool clientNoContextTakeover = false;
    int  compLevel               = 6;
    int  memLevel                = 4;
};
explicit WsDeflateContext(const Config&);
std::string compress(string_view input);
std::string decompress(string_view input, size_t maxOutputSize = 0);  // 0 = 不限制
```

---

### Concepts

C++20 Concept 约束，定义网络后端必须满足的接口。

**头文件：** `<hical/core/Concepts.h>`

| Concept                | 说明             |
| ---------------------- | ---------------- |
| `EventLoopLike<T>`     | 事件循环接口约束 |
| `TcpConnectionLike<T>` | TCP 连接接口约束 |
| `TimerLike<T>`         | 定时器接口约束   |
| `NetworkBackend<T>`    | 网络后端统一约束 |

默认后端：`AsioBackend`（`AsioEventLoop` + `PlainConnection` + `AsioTimer`）。

---

### Asio 适配层

Boost.Asio 具体实现层。

**头文件：** `<hical/asio/>`

| 文件                  | 说明                                         |
| --------------------- | -------------------------------------------- |
| `AsioEventLoop.h`     | EventLoop 的 Asio 实现                       |
| `AsioTimer.h`         | 定时器的 Asio 实现                           |
| `GenericConnection.h` | 模板化连接（TCP/SSL），sendFile 异步文件发送 |
| `SslConnection.h`     | SSL 连接类型别名                             |
| `TcpServer.h`         | TCP 服务器，空闲超时 + IdleFd fd 耗尽防护    |
| `EventLoopPool.h`     | 事件循环线程池                               |

通常用户不需要直接使用适配层，`HttpServer` 已封装了全部网络操作。

---

## 日志系统 API

> 命名空间为 `hical`，头文件在 `<hical/core/>`。

### Log

日志系统，提供 6 级日志、多种 API 风格和零开销设计。

**头文件：** `<hical/core/Log.h>`

#### LogLevel 枚举

| 枚举值   | 说明                        |
| -------- | --------------------------- |
| `hTrace` | 跟踪（NDEBUG 下编译期消除） |
| `hDebug` | 调试                        |
| `hInfo`  | 信息                        |
| `hWarn`  | 警告                        |
| `hError` | 错误                        |
| `hFatal` | 致命（触发 abort）          |

#### Logger 类（单例）

| 方法                   | 参数                        | 返回值     | 说明             |
| ---------------------- | --------------------------- | ---------- | ---------------- |
| `instance()`           | 无                          | `Logger&`  | 获取全局单例     |
| `setLevel(level)`      | level: LogLevel             | `void`     | 设置最低日志级别 |
| `level()`              | 无                          | `LogLevel` | 获取当前级别     |
| `setFlushLevel(level)` | level: LogLevel             | `void`     | 设置自动刷盘级别 |
| `addSink(sink)`        | sink: `shared_ptr<LogSink>` | `void`     | 添加输出后端     |
| `clearSinks()`         | 无                          | `void`     | 清空所有输出后端 |

#### 宏 API

完整宏家族（每个级别均提供 5 种变体）：

| 级别 / 变体                | 格式化（fmt）              | 条件（_IF）                        | 流式（_STREAM）          | 结构化字段（_F）                    |
| -------------------------- | -------------------------- | ---------------------------------- | ------------------------ | ----------------------------------- |
| Trace（仅 Debug 构建保留） | `HICAL_LOG_TRACE(fmt,...)` | `HICAL_LOG_TRACE_IF(cond,fmt,...)` | `HICAL_LOG_TRACE_STREAM` | `HICAL_LOG_TRACE_F(fields,fmt,...)` |
| Debug                      | `HICAL_LOG_DEBUG(fmt,...)` | `HICAL_LOG_DEBUG_IF(cond,fmt,...)` | `HICAL_LOG_DEBUG_STREAM` | `HICAL_LOG_DEBUG_F(fields,fmt,...)` |
| Info                       | `HICAL_LOG_INFO(fmt,...)`  | `HICAL_LOG_INFO_IF(cond,fmt,...)`  | `HICAL_LOG_INFO_STREAM`  | `HICAL_LOG_INFO_F(fields,fmt,...)`  |
| Warn                       | `HICAL_LOG_WARN(fmt,...)`  | `HICAL_LOG_WARN_IF(cond,fmt,...)`  | `HICAL_LOG_WARN_STREAM`  | `HICAL_LOG_WARN_F(fields,fmt,...)`  |
| Error                      | `HICAL_LOG_ERROR(fmt,...)` | `HICAL_LOG_ERROR_IF(cond,fmt,...)` | `HICAL_LOG_ERROR_STREAM` | `HICAL_LOG_ERROR_F(fields,fmt,...)` |
| Fatal（触发 abort）        | `HICAL_LOG_FATAL(fmt,...)` | `HICAL_LOG_FATAL_IF(cond,fmt,...)` | `HICAL_LOG_FATAL_STREAM` | `HICAL_LOG_FATAL_F(fields,fmt,...)` |

通道路由宏：

| 宏                                                 | 说明                                                                 |
| -------------------------------------------------- | -------------------------------------------------------------------- |
| `HICAL_LOG_TO(channel, Level, fmt, ...)`           | 向指定命名通道写日志（Level 取 `Trace/Debug/Info/Warn/Error/Fatal`） |
| `HICAL_LOG_TO_F(channel, Level, fields, fmt, ...)` | 通道路由 + 结构化字段                                                |

#### 示例

```cpp
#include <hical/core/Log.h>

HICAL_LOG_INFO("port={}", 8080);
HICAL_LOG_WARN_IF(latency > 100, "slow: {}ms", latency);
HICAL_LOG_INFO_F("login", {{"userId", 42}, {"ip", "1.2.3.4"}});
```

---

### LogRecord

结构化日志条目。

**头文件：** `<hical/core/LogRecord.h>`

| 字段        | 类型                       | 说明             |
| ----------- | -------------------------- | ---------------- |
| `level`     | `LogLevel`                 | 日志级别         |
| `timestamp` | `system_clock::time_point` | 时间戳           |
| `threadId`  | `uint64_t`                 | 线程 ID          |
| `file`      | `std::string_view`         | 源文件名         |
| `line`      | `int`                      | 行号             |
| `message`   | `std::string`              | 日志消息         |
| `fields`    | `boost::json::object`      | 结构化字段       |
| `traceId`   | `std::string`              | trace-id（可选） |

---

### LogFormatter

日志格式化器接口。

**头文件：** `<hical/core/LogFormatter.h>`

| 实现类          | 说明                          |
| --------------- | ----------------------------- |
| `TextFormatter` | 人类可读文本格式              |
| `JsonFormatter` | JSON Lines 格式（UTC 时间戳） |

接口：`format(const LogRecord&) -> std::string`。

---

### LogSink

可插拔日志输出后端接口。

**头文件：** `<hical/core/LogSink.h>`

| 实现类        | 说明                      |
| ------------- | ------------------------- |
| `StderrSink`  | 输出到 stderr             |
| `FileSink`    | 同步写文件 + LogFile 轮转 |
| `OStreamSink` | 线程安全 ostream 包装     |

接口：`write(const LogRecord&)` / `flush()`。

---

### LogFile

日志文件轮转引擎。

**头文件：** `<hical/core/LogFile.h>`

| 参数          | 默认值 | 说明       |
| ------------- | ------ | ---------- |
| `baseName`    | —      | 基础文件名 |
| `maxFileSize` | 100MB  | 单文件阈值 |
| `maxFiles`    | 10     | 保留归档数 |

归档命名：`app.YYMMDD-HHMMSS.NNNNNN.log`。

---

### AsyncFileSink

异步双缓冲文件 Sink。

**头文件：** `<hical/core/AsyncFileSink.h>`

- 后台 `std::jthread` + `stop_token` 优雅关闭
- 4MB 前后缓冲区交换，背压保护（缓冲满时丢弃并计数）

```cpp
#include <hical/core/AsyncFileSink.h>

AsyncFileSink::Options opts;
opts.file.basePath = "logs/app.log";
opts.file.maxFileSize = 100 * 1024 * 1024;
opts.file.maxFiles = 10;
Logger::instance().addSink(std::make_shared<AsyncFileSink>(opts));
```

---

### FixedBuffer

栈上固定大小缓冲区（默认 4KB），用于日志格式化。

**头文件：** `<hical/core/FixedBuffer.h>`

方法：`append(data, len)` / `appendInt(v)` / `appendFloat(v)` / `str()` / `clear()`。溢出时自动 fallback 到堆。

---

### LogChannel

命名日志通道，支持独立级别、格式化器和 Sink。

**头文件：** `<hical/core/LogChannel.h>`

```cpp
auto ch = LogChannelRegistry::instance().get("access");
ch->setFormatter(std::make_shared<JsonFormatter>());
ch->addSink(std::make_shared<FileSink>("logs/access"));

HICAL_LOG_TO("access", Info, "GET /api/users 200");
```

---

### LogMiddleware

洋葱模型日志中间件，自动生成 trace-id 并记录结构化访问日志。

**头文件：** `<hical/core/LogMiddleware.h>`

```cpp
MiddlewareHandler makeLogMiddleware(const std::string& channelName = "access");
```

```cpp
server.use(makeLogMiddleware());
```

---

### LogAdmin

动态日志级别管理端点。

**头文件：** `<hical/core/LogAdmin.h>`

```cpp
void registerLogAdmin(Router& router, const std::string& prefix = "/admin");
// GET  {prefix}/log-level — 查询
// PUT  {prefix}/log-level — 调整（{"level":"info"} 或 {"channel":"access","level":"debug"}）
```

---

## 数据库中间件 API

> 需要 `HICAL_WITH_DATABASE=ON`。命名空间 `hical::db`，头文件在 `<hical/db/>`。

---

### DbConfig

**头文件：** `<hical/db/DbConfig.h>`

| 字段                  | 类型                   | 默认值      | 说明                         |
| --------------------- | ---------------------- | ----------- | ---------------------------- |
| `host`                | `std::string`          | `127.0.0.1` | 主机地址                     |
| `port`                | `uint16_t`             | `3306`      | 端口                         |
| `user`                | `std::string`          | `""`        | 用户名                       |
| `password`            | `std::string`          | `""`        | 密码                         |
| `database`            | `std::string`          | `""`        | 数据库名                     |
| `charset`             | `std::string`          | `"utf8mb4"` | 字符集                       |
| `minConnections`      | `size_t`               | `2`         | 最小连接数                   |
| `maxConnections`      | `size_t`               | `16`        | 最大连接数                   |
| `idleTimeout`         | `std::chrono::seconds` | `300s`      | 空闲回收超时                 |
| `acquireTimeout`      | `std::chrono::seconds` | `5s`        | 获取连接超时                 |
| `queryTimeout`        | `std::chrono::seconds` | `30s`       | 查询执行超时                 |
| `autoReconnect`       | `bool`                 | `true`      | 断线自动重连                 |
| `idleCheckInterval`   | `std::chrono::seconds` | `60s`       | 空闲连接回收检查间隔         |
| `healthCheckInterval` | `std::chrono::seconds` | `30s`       | 后台健康检查间隔（0=禁用）   |
| `pingGracePeriod`     | `std::chrono::seconds` | `15s`       | acquire() 跳过 ping 的宽限期 |
| `stmtCacheSize`       | `size_t`               | `64`        | PreparedStatement 缓存上限   |

---

### DbResult

**头文件：** `<hical/db/DbResult.h>`

| 字段           | 类型                                    | 说明         |
| -------------- | --------------------------------------- | ------------ |
| `columns`      | `std::vector<std::string>`              | 列名         |
| `rows`         | `std::vector<std::vector<std::string>>` | 结果行       |
| `affectedRows` | `uint64_t`                              | DML 影响行数 |
| `insertId`     | `uint64_t`                              | INSERT 主键  |

方法：`empty()` / `size()` / `operator[]` / `columnIndex(name)`。

---

### DbConnection

数据库连接抽象基类。

**头文件：** `<hical/db/DbConnection.h>`

| 方法                   | 返回值                     | 说明                             |
| ---------------------- | -------------------------- | -------------------------------- |
| `query(sql, params)`   | `Awaitable<DbResult>`      | 参数化查询（防 SQL 注入）        |
| `query(sql)`           | `Awaitable<DbResult>`      | **[[deprecated]]** 非参数化查询  |
| `execute(sql, params)` | `Awaitable<DbResult>`      | 参数化执行                       |
| `execute(sql)`         | `Awaitable<DbResult>`      | **[[deprecated]]** 非参数化执行  |
| `beginTransaction()`   | `Awaitable<void>`          | 开启事务                         |
| `commit()`             | `Awaitable<void>`          | 提交事务                         |
| `rollback()`           | `Awaitable<void>`          | 回滚事务                         |
| `inTransaction()`      | `bool`                     | 是否处于事务中                   |
| `ping()`               | `Awaitable<bool>`          | 检查连通性                       |
| `isAlive()`            | `bool`                     | 连接是否存活                     |
| `backend()`            | `std::string_view`         | 后端名称（如 `"mysql"`）         |
| `lastActiveTime()`     | `steady_clock::time_point` | 最后活跃时间（用于空闲回收）     |
| `lastPingTime()`       | `steady_clock::time_point` | 最后 ping 时间（用于宽限期优化） |
| `touch()`              | `void`                     | 手动更新 lastActiveTime          |

---

### DbConnectionPool

协程化连接池。

**头文件：** `<hical/db/DbConnectionPool.h>`

| 方法            | 返回值                                     | 说明                   |
| --------------- | ------------------------------------------ | ---------------------- |
| `init()`        | `Awaitable<void>`                          | 预热连接池             |
| `acquire()`     | `Awaitable<std::shared_ptr<DbConnection>>` | 获取连接（超时抛异常） |
| `release(conn)` | `void`                                     | 归还连接               |
| `shutdown()`    | `Awaitable<void>`                          | 关闭连接池             |

统计：`activeCount()` / `idleCount()` / `waitingCount()` / `totalCount()`。

---

### DbMiddleware

**头文件：** `<hical/db/DbMiddleware.h>`

```cpp
MiddlewareHandler makeDbMiddleware(
    std::shared_ptr<DbConnectionPool> pool,
    DbMiddlewareOptions opts = {});

std::shared_ptr<DbConnection> getDbConnection(const HttpRequest& req);
std::shared_ptr<DbConnectionPool> getDbPool(const HttpRequest& req);
```

| DbMiddlewareOptions 字段 | 默认值  | 说明             |
| ------------------------ | ------- | ---------------- |
| `autoTransaction`        | `false` | 自动事务         |
| `injectPool`             | `true`  | 注入连接池到请求 |

---

### DbQueryLog

查询日志中间件（需在 `makeDbMiddleware` 之后注册）。通过装饰器模式（`LoggingDbConnection` 包装真实连接）记录查询日志。

**头文件：** `<hical/db/DbQueryLog.h>`

#### QueryLogEntry 结构体

| 字段              | 类型                        | 默认值  | 说明           |
| ----------------- | --------------------------- | ------- | -------------- |
| `sql`             | `std::string`               | —       | SQL 语句       |
| `duration`        | `std::chrono::microseconds` | `0`     | 执行耗时       |
| `rowCount`        | `size_t`                    | `0`     | 结果行数       |
| `affectedRows`    | `uint64_t`                  | `0`     | DML 影响行数   |
| `isParameterized` | `bool`                      | `false` | 是否参数化查询 |

#### QueryLogOptions 结构体

| 字段                 | 类型                                                               | 说明                 |
| -------------------- | ------------------------------------------------------------------ | -------------------- |
| `onRequestComplete`  | `function<void(const HttpRequest&, const vector<QueryLogEntry>&)>` | 请求完成回调         |
| `slowQueryThreshold` | `std::chrono::microseconds`                                        | 慢查询阈值（0=禁用） |
| `onSlowQuery`        | `function<void(const QueryLogEntry&)>`                             | 慢查询回调           |

常量：`static constexpr const char* hQueryLogKey = "hical.db.queryLog"` — 请求属性 key。

```cpp
MiddlewareHandler makeQueryLogMiddleware(QueryLogOptions opts = {});
```

---

### MysqlConnection

MySQL 后端（Boost.MySQL）。

**头文件：** `<hical/db/MysqlConnection.h>`

| 方法                    | 返回值                                        | 说明       |
| ----------------------- | --------------------------------------------- | ---------- |
| `create(ioCtx, config)` | `Awaitable<std::shared_ptr<MysqlConnection>>` | 异步建连   |
| `makeFactory()`         | `DbConnectionFactory`                         | 池工厂函数 |

---

### StmtCache

PreparedStatement LRU 缓存。

**头文件：** `<hical/db/StmtCache.h>`

方法：`find(sql)` / `insert(key, stmt)` / `erase(sql)` / `clear()` / `size()` / `maxSize()`。

---

### 数据库综合示例

```cpp
#include <hical/core/HttpServer.h>
#include <hical/db/DbConfig.h>
#include <hical/db/DbConnectionPool.h>
#include <hical/db/DbMiddleware.h>
#include <hical/db/DbQueryLog.h>
#include <hical/db/MysqlConnection.h>

using namespace hical;
using namespace hical::db;

int main()
{
    HttpServer server(8080, 4);

    // 1. 配置数据库
    DbConfig dbConfig;
    dbConfig.host = "127.0.0.1";
    dbConfig.user = "root";
    dbConfig.password = "secret";
    dbConfig.database = "myapp";
    dbConfig.minConnections = 4;
    dbConfig.maxConnections = 32;

    // 2. 创建连接池（使用 server 内部的 io_context）
    auto pool = std::make_shared<DbConnectionPool>(
        server.ioContext(), dbConfig, MysqlConnection::makeFactory());

    // 3. 注册中间件（顺序：DB 在前，QueryLog 在后）
    server.use(makeDbMiddleware(pool, {.autoTransaction = true}));
    server.use(makeQueryLogMiddleware({
        .slowQueryThreshold = std::chrono::milliseconds(100),
        .onSlowQuery = [](const QueryLogEntry& entry) {
            HICAL_LOG_WARN("[SLOW] {} ({}us)", entry.sql, entry.duration.count());
        }
    }));

    // 4. 在路由中使用
    server.router().get("/users/{id}",
        [](const HttpRequest& req) -> Awaitable<HttpResponse> {
            auto conn = getDbConnection(req);
            auto result = co_await conn->query(
                "SELECT id, name FROM users WHERE id = ?",
                {req.param("id")});
            if (result.empty()) {
                co_return HttpResponse::notFound();
            }
            co_return HttpResponse::json({
                {"id", result[0][0]},
                {"name", result[0][1]}
            });
        });

    server.start();
}
```

---

## OpenAPI 元数据 API

> 需要 `HICAL_WITH_OPENAPI=ON`（默认启用）。命名空间 `hical`，头文件在 `<hical/core/>`。

---

### OpenApiSchema

从 `HICAL_JSON` 类型自动生成 OpenAPI 3.0 Schema。

**头文件：** `<hical/core/OpenApiSchema.h>`

| 函数                         | 返回值                | 说明                           |
| ---------------------------- | --------------------- | ------------------------------ |
| `jsonSchema<T>()`            | `boost::json::object` | 生成 Schema Object             |
| `collectSchemas<T>(schemas)` | `void`                | 递归收集 T 及嵌套类型的 schema |

| 宏                                | 说明                     |
| --------------------------------- | ------------------------ |
| `HICAL_SCHEMA_NAME(Type, "name")` | 注册类型的 `$ref` 引用名 |

**类型映射：** `bool`→boolean, `int`→integer/int32, `int64_t`→integer/int64, `float`→number/float, `double`→number/double, `string`→string, `vector<T>`→array, 带 `HICAL_JSON` 的结构体→object/$ref。

---

### OpenApiRegistry

线程安全的路由 API 元数据注册表。

**头文件：** `<hical/core/OpenApiRegistry.h>`

方法：`add(info)` / `getAll()` / `clear()` / `count()`。

**宏：**

| 宏                                 | 说明                     |
| ---------------------------------- | ------------------------ |
| `HICAL_API(builder_exprs...)`      | 综合标注宏               |
| `HICAL_ROUTES_WITH_API(Type, ...)` | 同时注册路由和收集元数据 |

**builder 命名空间：** `summary(text)` / `description(text)` / `tag(name)` / `requestBody<T>()` / `response<T>(code)` / `response(code, desc)`。

---

### OpenApiDocument

惰性组装 OpenAPI 3.0 完整文档。

**头文件：** `<hical/core/OpenApiDocument.h>`

| 方法                   | 说明                                     |
| ---------------------- | ---------------------------------------- |
| `addSchema(name, obj)` | 添加 schema                              |
| `addSchemas(map)`      | 批量添加                                 |
| `generateString()`     | 惰性生成并缓存完整文档，返回 JSON 字符串 |
| `invalidate()`         | 使缓存失效                               |

---

### OpenApiEndpoint

一行代码暴露文档端点。

**头文件：** `<hical/core/OpenApiEndpoint.h>`

```cpp
void serveOpenApi(
    Router& router,
    std::shared_ptr<OpenApiDocument> doc,
    std::string jsonPath = "/openapi.json",
    std::string docsPath = "/docs");
```

---

## 附录

### 类型别名速查

| 别名                     | 定义                                                              | 头文件               |
| ------------------------ | ----------------------------------------------------------------- | -------------------- |
| `Awaitable<T>`           | `boost::asio::awaitable<T>`                                       | `Coroutine.h`        |
| `RouteHandler`           | `function<Awaitable<HttpResponse>(const HttpRequest&)>`           | `Router.h`           |
| `SyncRouteHandler`       | `function<HttpResponse(const HttpRequest&)>`                      | `Router.h`           |
| `MiddlewareNext`         | `function<Awaitable<HttpResponse>(HttpRequest&)>`                 | `Middleware.h`       |
| `MiddlewareHandler`      | `function<Awaitable<HttpResponse>(HttpRequest&, MiddlewareNext)>` | `Middleware.h`       |
| `SyncMiddlewareResult`   | `std::optional<HttpResponse>`                                     | `Middleware.h`       |
| `SyncBeforeHandler`      | `function<SyncMiddlewareResult(HttpRequest&)>`                    | `Middleware.h`       |
| `SyncAfterHandler`       | `function<void(HttpRequest&, HttpResponse&)>`                     | `Middleware.h`       |
| `WsMessageCallback`      | `function<Awaitable<void>(const string&, WebSocketSession&)>`     | `Router.h`           |
| `WsTypedMessageCallback` | `function<Awaitable<void>(const WsMessage&, WebSocketSession&)>`  | `Router.h`           |
| `WsConnectCallback`      | `function<Awaitable<void>(WebSocketSession&)>`                    | `Router.h`           |
| `WsDisconnectCallback`   | `function<void(WebSocketSession&)>`                               | `Router.h`           |
| `WsConnectionId`         | `uint64_t`                                                        | `WsHub.h`            |
| `ErrorHandler`           | `function<HttpResponse(const exception&, const HttpRequest&)>`    | `HttpServer.h`       |
| `DbConnectionFactory`    | `function<Awaitable<shared_ptr<DbConnection>>(io_context&, ...)>` | `DbConnectionPool.h` |

### 回调类型速查

| 场景             | 类型签名                                                            | 别名 / 头文件                                         |
| ---------------- | ------------------------------------------------------------------- | ----------------------------------------------------- |
| 同步路由         | `HttpResponse(const HttpRequest&)`                                  | `SyncRouteHandler` / `Router.h`                       |
| 协程路由         | `Awaitable<HttpResponse>(const HttpRequest&)`                       | `RouteHandler` / `Router.h`                           |
| 异步中间件       | `Awaitable<HttpResponse>(HttpRequest&, MiddlewareNext)`             | `MiddlewareHandler` / `Middleware.h`                  |
| 同步前置中间件   | `std::optional<HttpResponse>(HttpRequest&)`                         | `SyncBeforeHandler` / `Middleware.h`                  |
| 同步后置中间件   | `void(HttpRequest&, HttpResponse&)`                                 | `SyncAfterHandler` / `Middleware.h`                   |
| 全局错误处理     | `HttpResponse(const std::exception&, const HttpRequest&)`           | `ErrorHandler` / `HttpServer.h`                       |
| WS 消息（文本）  | `Awaitable<void>(const std::string&, WebSocketSession&)`            | `WsMessageCallback` / `Router.h`                      |
| WS 消息（typed） | `Awaitable<void>(const WsMessage&, WebSocketSession&)`              | `WsTypedMessageCallback` / `Router.h`                 |
| WS 连接          | `Awaitable<void>(WebSocketSession&)`                                | `WsConnectCallback` / `Router.h`                      |
| WS 断开          | `Awaitable<void>(WebSocketSession&)`                                | `WsDisconnectCallback` / `Router.h`                   |
| DB 连接工厂      | `Awaitable<shared_ptr<DbConnection>>(io_context&, const DbConfig&)` | `DbConnectionFactory` / `DbConnectionPool.h`          |
| 慢查询回调       | `void(const QueryLogEntry&)`                                        | `QueryLogOptions::SlowQueryCallback` / `DbQueryLog.h` |
| 请求查询日志     | `void(const HttpRequest&, const vector<QueryLogEntry>&)`            | `QueryLogOptions::LogCallback` / `DbQueryLog.h`       |
| 定时器           | `void()`                                                            | `EventLoop::TimerCallback`                            |

---

> 更多信息请参阅：[快速上手](quickstart_cn.md) | [使用示例](examples_guide.md) | [架构设计](architecture.md) | [集成指南](integration_guide.md)
