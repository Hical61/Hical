# Hical API 参考文档

> 完整的 hical 框架公共 API 说明

---

## 目录

**核心 API（用户直接使用）**
- [HttpServer](#httpserver) — HTTP 服务器
- [Router](#router) — 路由管理
- [HttpRequest](#httprequest) — HTTP 请求
- [HttpResponse](#httpresponse) — HTTP 响应
- [HttpTypes](#httptypes) — HTTP 类型定义
- [Middleware](#middleware) — 中间件
- [Cookie](#cookie) — Cookie 解析与设置
- [StaticFiles](#staticfiles) — 静态文件服务
- [Multipart](#multipart) — 文件上传解析
- [Session](#session) — Session 会话管理
- [CORS 中间件](#cors-中间件) — 跨域资源共享
- [RouteGroup](#routegroup) — 路由分组
- [WebSocketSession](#websocketsession) — WebSocket 会话
- [WsHub](#wshub) — WebSocket 广播管理器
- [WsOptions](#wsoptions) — WebSocket 路由选项

**基础设施 API（进阶用户）**
- [Coroutine](#coroutine) — 协程工具
- [MemoryPool](#memorypool) — pmr 内存池
- [SslContext](#sslcontext) — SSL/TLS 配置
- [Error](#error) — 错误处理

**底层抽象（仅摘要）**
- [PmrBuffer](#pmrbuffer) — pmr 缓冲区
- [EventLoop](#eventloop) — 事件循环接口
- [Concepts](#concepts) — 后端约束
- [Asio 适配层](#asio-适配层) — Boost.Asio 适配

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
- [OpenApiSchema](#openapiSchema) — JSON Schema 生成
- [OpenApiRegistry](#openapiregistry) — 路由元数据注册表
- [OpenApiDocument](#openapidocument) — 文档组装
- [OpenApiEndpoint](#openapiendpoint) — 端点暴露

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

**附录**
- [类型别名速查](#类型别名速查)
- [回调类型速查](#回调类型速查)

---

## 核心 API

### HttpServer

HTTP 服务器，整合路由、中间件和网络层，提供一键启动的高层封装。

**头文件：** `src/core/HttpServer.h`

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
#include "core/HttpServer.h"

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

路由管理器，负责路由注册和请求分发。静态路由使用透明哈希表 O(1) 查找（`RouteKeyView` + `is_transparent` 实现零分配 `string_view` 查找），参数路由按 HTTP 方法分桶存储（`unordered_map<HttpMethod, vector<ParamRouteEntry>>`），仅扫描对应方法的路由子集。

**头文件：** `src/core/Router.h`

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

#### 宏

```cpp
HICAL_ROUTE(router, method, path, handler)
```

预留给未来 C++26 反射自动路由注册。当前等价于 `router.route(HttpMethod::hMethod, path, handler)`。

#### 示例

```cpp
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
wsOpts.allowedOrigins = {"https://example.com", "https://app.example.com"};
router.ws("/ws/chat", wsOpts,
    [](const std::string& msg, WebSocketSession& ws) -> Awaitable<void> {
        co_await ws.send("Echo: " + msg);
    }
);

// 使用宏
HICAL_ROUTE(router, Get, "/hello", myHandler);
```

---

### HttpRequest

HTTP 请求封装，对原生 HTTP 解析结果的 hical 风格封装。

**头文件：** `src/core/HttpRequest.h`

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

#### 构建请求的方法

| 方法                     | 参数                          | 返回值 | 说明                               |
| ------------------------ | ----------------------------- | ------ | ---------------------------------- |
| `setMethod(method)`      | method: HTTP 方法             | `void` | 设置 HTTP 方法                     |
| `setTarget(target)`      | target: 目标 URI              | `void` | 设置请求路径                       |
| `setHeader(name, value)` | name: 字段名<br>value: 字段值 | `void` | 设置头部字段（拒绝含 CR/LF 的值）  |
| `setBody(body)`          | body: 消息体                  | `void` | 设置消息体                         |
| `setParam(name, value)`  | name: 参数名<br>value: 参数值 | `void` | 设置路径参数（由 Router 内部调用） |

#### 示例

```cpp
void handler(const HttpRequest& req)
{
    // 读取请求信息
    auto method = req.method();
    auto path = req.path();
    auto query = req.query();
    auto auth = req.header("Authorization");
    auto body = req.body();
    
    // 解析 JSON 请求体
    try {
        auto json = req.jsonBody();
        auto name = json.at("name").as_string();
    } catch (...) {
        // 处理解析错误
    }
    
    // 读取路径参数
    if (req.hasParam("id")) {
        auto userId = req.param("id");
    }
}
```

---

### HttpResponse

HTTP 响应封装，对原生 HTTP 响应的 hical 风格封装。

**头文件：** `src/core/HttpResponse.h`

#### 公共方法

| 方法                         | 参数                                                         | 返回值               | 说明                    |
| ---------------------------- | ------------------------------------------------------------ | -------------------- | ----------------------- |
| `statusCode()`               | 无                                                           | `HttpStatusCode`     | 获取状态码              |
| `setStatus(code)`            | code: 状态码                                                 | `void`               | 设置状态码              |
| `header(name)`               | name: 字段名                                                 | `std::string`        | 获取指定头部字段值      |
| `setHeader(name, value)`     | name: 字段名<br>value: 字段值                                | `void`               | 设置头部字段            |
| `body()`                     | 无                                                           | `const std::string&` | 获取消息体              |
| `setBody(body, contentType)` | body: 消息体<br>contentType: Content-Type（默认 text/plain） | `void`               | 设置消息体              |
| `setJsonBody(json)`          | json: JSON 值                                                | `void`               | 设置 JSON 消息体        |
| `native()`                   | 无                                                           | `NativeResponse&`    | 获取底层原生响应引用  |

#### 工厂方法

| 方法                       | 参数                                                   | 返回值         | 说明                                      |
| -------------------------- | ------------------------------------------------------ | -------------- | ----------------------------------------- |
| `ok(body)`                 | body: 消息体（默认空）                                 | `HttpResponse` | 创建 200 OK 响应                          |
| `json(json)`               | json: JSON 值                                          | `HttpResponse` | 创建 JSON 200 OK 响应                     |
| `notFound()`               | 无                                                     | `HttpResponse` | 创建 404 Not Found 响应                   |
| `badRequest(message)`      | message: 错误信息（默认 "Bad Request"）                | `HttpResponse` | 创建 400 Bad Request 响应                 |
| `serverError()`            | 无                                                     | `HttpResponse` | 创建 500 Internal Server Error 响应       |
| `redirect(location, code)` | location: 重定向 URL<br>code: 状态码（默认 302 Found） | `HttpResponse` | 创建重定向响应（Location 头经 CRLF 防护） |

#### 示例

```cpp
HttpResponse handler(const HttpRequest& req)
{
    // 使用工厂方法
    return HttpResponse::ok("Hello");
    return HttpResponse::json({{"status", "ok"}});
    return HttpResponse::notFound();
    return HttpResponse::badRequest("Invalid input");
    return HttpResponse::serverError();
    
    // 手动构建响应
    HttpResponse res;
    res.setStatus(HttpStatusCode::hCreated);
    res.setHeader("X-Custom", "value");
    res.setBody("Created", "text/plain");
    return res;
    
    // 设置 JSON 响应
    HttpResponse res2;
    res2.setJsonBody({
        {"userId", 123},
        {"name", "Alice"}
    });
    return res2;
}
```

---

### HttpTypes

HTTP 类型定义，包含方法枚举、状态码枚举和转换函数。

**头文件：** `src/core/HttpTypes.h`

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

| 枚举值                 | 数值 | 说明           |
| ---------------------- | ---- | -------------- |
| `hOk`                  | 200  | 成功           |
| `hCreated`             | 201  | 已创建         |
| `hAccepted`            | 202  | 已接受         |
| `hNoContent`           | 204  | 无内容         |
| `hMovedPermanently`    | 301  | 永久重定向     |
| `hFound`               | 302  | 临时重定向     |
| `hNotModified`         | 304  | 未修改         |
| `hBadRequest`          | 400  | 错误请求       |
| `hUnauthorized`        | 401  | 未授权         |
| `hForbidden`           | 403  | 禁止访问       |
| `hNotFound`            | 404  | 未找到         |
| `hMethodNotAllowed`    | 405  | 方法不允许     |
| `hConflict`            | 409  | 冲突           |
| `hTooManyRequests`     | 429  | 请求过多       |
| `hInternalServerError` | 500  | 服务器内部错误 |
| `hNotImplemented`      | 501  | 未实现         |
| `hBadGateway`          | 502  | 网关错误       |
| `hServiceUnavailable`  | 503  | 服务不可用     |

#### 转换函数

| 函数                           | 参数              | 返回值        | 说明                      |
| ------------------------------ | ----------------- | ------------- | ------------------------- |
| `httpMethodToString(method)`   | method: HTTP 方法 | `const char*` | 转换为字符串（如 "GET"）  |
| `stringToHttpMethod(str)`      | str: 方法名字符串 | `HttpMethod`  | 从字符串转换              |
| `httpStatusCodeToString(code)` | code: 状态码      | `const char*` | 获取状态码描述（如 "OK"） |

#### 示例

```cpp
#include "core/HttpTypes.h"

using namespace hical;

// 方法转换
auto method = HttpMethod::hGet;
auto methodStr = httpMethodToString(method);  // "GET"
auto method2 = stringToHttpMethod("POST");    // HttpMethod::hPost

// 状态码转换
auto code = HttpStatusCode::hOk;
auto codeStr = httpStatusCodeToString(code);  // "OK"
```

---

### Middleware

中间件系统，采用洋葱模型，按注册顺序执行。

**头文件：** `src/core/Middleware.h`

#### 类型定义

```cpp
// 中间件 next 回调类型
using MiddlewareNext = std::function<Awaitable<HttpResponse>(HttpRequest&)>;

// 中间件处理器类型
using MiddlewareHandler = std::function<Awaitable<HttpResponse>(HttpRequest&, MiddlewareNext)>;
```

#### MiddlewarePipeline 类

| 方法                         | 参数                                       | 返回值                    | 说明                                     |
| ---------------------------- | ------------------------------------------ | ------------------------- | ---------------------------------------- |
| `use(middleware)`            | middleware: 中间件处理器                   | `void`                    | 添加中间件（`build()` 后禁止调用）       |
| `build(finalHandler)`        | finalHandler: 最终处理器                   | `void`                    | 预构建调用链（仅调用一次）               |
| `buildFor(finalHandler)`     | finalHandler: 最终处理器                   | `MiddlewareNext`          | 预构建并返回可缓存的调用链               |
| `execute(req)`               | req: HTTP 请求                             | `Awaitable<HttpResponse>` | 执行预构建缓存链（需先 `build()`）       |
| `execute(req, finalHandler)` | req: HTTP 请求<br>finalHandler: 最终处理器 | `Awaitable<HttpResponse>` | 动态构建并执行（始终使用传入的 handler） |
| `size()`                     | 无                                         | `size_t`                  | 获取中间件数量                           |

#### 示例

```cpp
#include "core/Middleware.h"

using namespace hical;

// 日志中间件
auto logger = [](HttpRequest& req, MiddlewareNext next)
                  -> Awaitable<HttpResponse> {
    std::cout << "[" << httpMethodToString(req.method()) << "] " 
              << req.path() << std::endl;
    
    auto res = co_await next(req);  // 调用下一层
    
    std::cout << "  -> " << static_cast<int>(res.statusCode()) << std::endl;
    co_return res;
};

// 认证中间件（拦截未授权请求）
auto auth = [](HttpRequest& req, MiddlewareNext next)
                -> Awaitable<HttpResponse> {
    auto token = req.header("Authorization");
    if (token.empty()) {
        // 拦截请求，不调用 next
        co_return HttpResponse::badRequest("需要认证");
    }
    
    // 验证通过，继续执行
    co_return co_await next(req);
};

// CORS 中间件
auto cors = [](HttpRequest& req, MiddlewareNext next)
                -> Awaitable<HttpResponse> {
    auto res = co_await next(req);
    res.setHeader("Access-Control-Allow-Origin", "*");
    co_return res;
};

// 使用中间件
HttpServer server(8080);
server.use(logger);
server.use(auth);
server.use(cors);
```

---

### WebSocketSession

WebSocket 会话封装，对自研 WebSocket 实现（RFC 6455）的 hical 风格封装。支持文本/二进制消息、心跳保活、子协议协商和 per-connection 上下文存储。

**头文件：** `src/core/WebSocket.h`

#### 公共方法

| 方法                         | 参数                                    | 返回值                                 | 说明                               |
| ---------------------------- | --------------------------------------- | -------------------------------------- | ---------------------------------- |
| `send(msg)`                  | msg: 文本消息                           | `Awaitable<void>`                      | 发送文本帧                         |
| `sendBinary(data)`           | data: `string_view`                     | `Awaitable<void>`                      | 发送二进制帧                       |
| `receive()`                  | 无                                      | `Awaitable<std::string>`               | 接收文本消息（向后兼容）           |
| `receiveMessage()`           | 无                                      | `Awaitable<optional<WsMessage>>`       | 接收 typed 消息（区分 Text/Binary）|
| `sendPing(payload)`          | payload: Ping 载荷（≤125B）             | `Awaitable<void>`                      | 手动发送 Ping 帧                   |
| `closeAsync(code, reason)`   | code: `WsCloseCode`<br>reason: 关闭原因 | `Awaitable<void>`                      | 优雅关闭（发送 Close 帧）         |
| `close()`                    | 无                                      | `void`                                 | 同步关闭连接                       |
| `isOpen()`                   | 无                                      | `bool`                                 | 连接是否打开                       |
| `subprotocol()`              | 无                                      | `const std::string&`                   | 协商后的子协议                     |
| `setSubprotocol(proto)`      | proto: 协议名                           | `void`                                 | 设置子协议（握手阶段使用）         |
| `lastPongTime()`             | 无                                      | `steady_clock::time_point`             | 最后收到 Pong 的时间               |
| `setContext(ptr)`            | ptr: `shared_ptr<void>`                 | `void`                                 | 设置 per-connection 上下文         |
| `getContext<T>()`            | 无                                      | `shared_ptr<T>`                        | 获取类型化上下文                   |
| `hasContext()`               | 无                                      | `bool`                                 | 是否已设置上下文                   |
| `clearContext()`             | 无                                      | `void`                                 | 清除上下文                         |

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
// 文本消息回调（向后兼容）
using WsMessageCallback = std::function<Awaitable<void>(const std::string&, WebSocketSession&)>;

// 类型化消息回调（区分 Text/Binary，优先于 onMessage）
using WsTypedMessageCallback = std::function<Awaitable<void>(const WsMessage&, WebSocketSession&)>;

// 连接建立回调
using WsConnectCallback = std::function<Awaitable<void>(WebSocketSession&)>;

// 连接断开回调
using WsDisconnectCallback = std::function<void(WebSocketSession&)>;
```

#### 示例

```cpp
#include "core/WebSocket.h"

using namespace hical;

// 基础文本回调
server.router().ws("/ws/echo",
    [](const std::string& msg, WebSocketSession& ws) -> Awaitable<void> {
        co_await ws.send("Echo: " + msg);
    }
);

// 带心跳 + 压缩 + 子协议 + Origin 白名单
WsOptions wsOpts;
wsOpts.pingInterval = std::chrono::seconds(30);
wsOpts.maxMissedPongs = 3;
wsOpts.enableCompression = true;
wsOpts.subprotocols = {"chat.v1", "chat.v2"};
wsOpts.allowedOrigins = {"https://example.com"};

server.router().ws("/ws/chat", wsOpts,
    [](const std::string& msg, WebSocketSession& ws) -> Awaitable<void> {
        co_await ws.send("收到: " + msg);
    },
    // onConnect（可选）
    [](WebSocketSession& ws) -> Awaitable<void> {
        co_await ws.send("欢迎! 协议: " + ws.subprotocol());
    }
);

// 手动使用 receiveMessage() 区分 Text/Binary
Awaitable<void> binaryHandler(WebSocketSession& ws)
{
    while (ws.isOpen())
    {
        auto msg = co_await ws.receiveMessage();
        if (!msg) break;  // 连接关闭

        if (msg->type == WsOpcode::hBinary)
            co_await ws.sendBinary(msg->data);  // 二进制回显
        else
            co_await ws.send(msg->data);        // 文本回显
    }
}
```

---

### WsHub

WebSocket 连接管理器 / 广播中心，线程安全。适用于聊天室、实时推送等多连接广播场景。

**头文件：** `src/core/WsHub.h`

#### 类型定义

```cpp
using WsConnectionId = uint64_t;
```

#### 公共方法

| 方法                                       | 参数                                                           | 返回值             | 说明                         |
| ------------------------------------------ | -------------------------------------------------------------- | ------------------ | ---------------------------- |
| `add(session)`                             | session: `shared_ptr<WebSocketSession>`                        | `WsConnectionId`   | 注册连接，返回唯一 ID       |
| `remove(id)`                               | id: 连接 ID                                                    | `void`             | 移除连接（自动离开所有房间） |
| `join(id, room)`                           | id: 连接 ID<br>room: 房间名                                    | `void`             | 加入房间                     |
| `leave(id, room)`                          | id: 连接 ID<br>room: 房间名                                    | `void`             | 离开房间                     |
| `broadcast(room, message, exclude)`        | room: 房间名<br>message: 消息<br>exclude: 排除 ID（默认 0）    | `void`             | 文本广播到房间               |
| `broadcastBinary(room, data, exclude)`     | room: 房间名<br>data: 二进制数据<br>exclude: 排除 ID（默认 0） | `void`             | 二进制广播到房间             |
| `broadcastAll(message, exclude)`           | message: 消息<br>exclude: 排除 ID（默认 0）                    | `void`             | 文本广播到所有连接           |
| `sendTo(id, message)`                      | id: 连接 ID<br>message: 消息                                   | `void`             | 单播到指定连接               |
| `roomSize(room)`                           | room: 房间名                                                   | `size_t`           | 房间内连接数                 |
| `connectionCount()`                        | 无                                                             | `size_t`           | 总注册连接数                 |

#### 注意事项

- Hub 存储 `weak_ptr<WebSocketSession>`，不延长连接生命周期
- **必须在 `onDisconnect` 回调中调用 `remove(id)`**，Hub 不自动清理断开的连接
- 广播通过 `coSpawn` 投递到各连接所属 executor，跨线程安全

#### 示例

```cpp
#include "core/WsHub.h"

using namespace hical;

WsHub hub;

server.router().ws("/ws/room", wsOpts,
    // onMessage
    [&hub](const std::string& msg, WebSocketSession& ws) -> Awaitable<void> {
        auto id = ws.getContext<WsConnectionId>();
        hub.broadcast("lobby", msg, id ? *id : 0);  // 广播给同房间其他人
        co_return;
    },
    // onConnect
    [&hub](WebSocketSession& ws) -> Awaitable<void> {
        auto id = hub.add(ws.shared_from_this());
        ws.setContext(std::make_shared<WsConnectionId>(id));
        hub.join(id, "lobby");
        hub.broadcast("lobby", "新用户加入", id);
        co_return;
    },
    // onDisconnect
    [&hub](WebSocketSession& ws) {
        if (auto ctx = ws.getContext<WsConnectionId>())
        {
            hub.broadcast("lobby", "用户离开", *ctx);
            hub.remove(*ctx);
        }
    }
);
```

---

### WsOptions

WebSocket 路由选项，配置安全策略、压缩、心跳和子协议。

**头文件：** `src/core/Router.h`（`Router::WsOptions` 嵌套结构体）

#### 字段

| 字段                      | 类型                              | 默认值 | 说明                                      |
| ------------------------- | --------------------------------- | ------ | ----------------------------------------- |
| `allowedOrigins`          | `unordered_set<string>`           | 空     | Origin 白名单（空=不校验，防 CSWSH）      |
| `enableCompression`       | `bool`                            | false  | 启用 permessage-deflate 压缩              |
| `serverMaxWindowBits`     | `int`                             | 15     | 服务端 zlib 窗口位数（9-15）              |
| `clientMaxWindowBits`     | `int`                             | 15     | 客户端 zlib 窗口位数（9-15）              |
| `serverNoContextTakeover` | `bool`                            | false  | 每消息独立压缩（省内存降压缩率）          |
| `pingInterval`            | `std::chrono::seconds`            | 0      | 心跳 Ping 间隔（0=禁用）                 |
| `maxMissedPongs`          | `uint32_t`                        | 3      | 最大连续未收到 Pong 次数，超过则关闭      |
| `pingPayload`             | `std::string`                     | 空     | 自定义 Ping 载荷（最大 125 字节）         |
| `subprotocols`            | `vector<string>`                  | 空     | 支持的子协议列表（空=忽略协商）           |

---

## 基础设施 API

### Coroutine

协程工具，提供协程类型别名和便捷函数。

**头文件：** `src/core/Coroutine.h`

#### 类型别名

```cpp
template <typename T = void>
using Awaitable = boost::asio::awaitable<T>;
```

协程返回类型别名，基于 Boost.Asio 的 `awaitable<T>`。

#### 函数

| 函数                                 | 参数                                                               | 返回值            | 说明                                       |
| ------------------------------------ | ------------------------------------------------------------------ | ----------------- | ------------------------------------------ |
| `sleep(seconds)`                     | seconds: 等待秒数                                                  | `Awaitable<void>` | 在当前协程上下文中等待（必须在协程内调用） |
| `sleep(duration)`                    | duration: 等待时长                                                 | `Awaitable<void>` | 在当前协程上下文中等待（chrono 版本）      |
| `sleepFor(ioCtx, seconds)`           | ioCtx: io_context 引用<br>seconds: 等待秒数                        | `Awaitable<void>` | 在指定 io_context 上等待                   |
| `sleepFor(ioCtx, duration)`          | ioCtx: io_context 引用<br>duration: 等待时长                       | `Awaitable<void>` | 在指定 io_context 上等待（chrono 版本）    |
| `coSpawn(ioCtx, coroutine)`          | ioCtx: io_context 引用<br>coroutine: 协程函数                      | `void`            | 启动协程                                   |
| `coSpawn(ioCtx, coroutine, handler)` | ioCtx: io_context 引用<br>coroutine: 协程函数<br>handler: 完成回调 | `void`            | 启动协程并设置完成回调                     |

#### 示例

```cpp
#include "core/Coroutine.h"

using namespace hical;

// 在协程内使用 sleep
Awaitable<void> asyncTask()
{
    std::cout << "开始" << std::endl;
    co_await sleep(1.0);  // 等待 1 秒
    std::cout << "1 秒后" << std::endl;
    
    // 使用 chrono
    co_await sleep(std::chrono::milliseconds(500));
    std::cout << "再过 0.5 秒" << std::endl;
}

// 使用 sleepFor（需要传入 io_context）
Awaitable<void> asyncTask2(boost::asio::io_context& ioCtx)
{
    co_await sleepFor(ioCtx, 2.0);
}

// 启动协程
int main()
{
    boost::asio::io_context ioCtx;
    
    coSpawn(ioCtx, asyncTask());
    
    // 带完成回调
    coSpawn(ioCtx, asyncTask(), [](std::exception_ptr ex) {
        if (ex) {
            std::cout << "协程异常" << std::endl;
        }
    });
    
    ioCtx.run();
    return 0;
}
```

---

### MemoryPool

全局 pmr 内存池管理器，提供三层内存分配策略。

**头文件：** `src/core/MemoryPool.h`

#### PoolConfig 结构体

| 字段                           | 类型     | 默认值 | 说明                   |
| ------------------------------ | -------- | ------ | ---------------------- |
| `globalMaxBlocksPerChunk`      | `size_t` | 128    | 全局同步池每块最大块数 |
| `globalLargestPoolBlock`       | `size_t` | 1MB    | 全局同步池最大块大小   |
| `threadLocalMaxBlocksPerChunk` | `size_t` | 64     | 线程本地池每块最大块数 |
| `threadLocalLargestPoolBlock`  | `size_t` | 512KB  | 线程本地池最大块大小   |
| `requestPoolInitialSize`       | `size_t` | 4096   | 请求级池初始大小       |

#### MemoryPool 类

| 方法                             | 参数                                        | 返回值                                       | 说明                               |
| -------------------------------- | ------------------------------------------- | -------------------------------------------- | ---------------------------------- |
| `instance()`                     | 无                                          | `MemoryPool&`                                | 获取全局单例                       |
| `configure(config)`              | config: 池配置                              | `void`                                       | 配置内存池（必须在首次使用前调用） |
| `globalAllocator()`              | 无                                          | `pmr::polymorphic_allocator<byte>`           | 获取全局同步池分配器               |
| `threadLocalAllocator()`         | 无                                          | `pmr::polymorphic_allocator<byte>`           | 获取线程本地池分配器               |
| `createRequestPool(initialSize)` | initialSize: 初始大小（0 则使用配置默认值） | `unique_ptr<pmr::monotonic_buffer_resource>` | 创建请求级单调池                   |
| `getStats()`                     | 无                                          | `Stats`                                      | 获取全局池统计信息                 |
| `resetStats()`                   | 无                                          | `void`                                       | 重置统计数据                       |

#### Stats 结构体

| 字段                    | 类型     | 说明             |
| ----------------------- | -------- | ---------------- |
| `totalAllocations`      | `size_t` | 总分配次数       |
| `totalDeallocations`    | `size_t` | 总释放次数       |
| `currentBytesAllocated` | `size_t` | 当前已分配字节数 |
| `peakBytesAllocated`    | `size_t` | 峰值字节数       |

#### TrackedResource 类

追踪型内存资源包装器，在 allocate/deallocate 时做原子计数统计。

| 方法                   | 返回值   | 说明             |
| ---------------------- | -------- | ---------------- |
| `totalAllocations()`   | `size_t` | 总分配次数       |
| `totalDeallocations()` | `size_t` | 总释放次数       |
| `currentBytes()`       | `size_t` | 当前已分配字节数 |
| `peakBytes()`          | `size_t` | 峰值字节数       |
| `resetStats()`         | `void`   | 重置统计数据     |

#### 示例

```cpp
#include "core/MemoryPool.h"

using namespace hical;

// 配置内存池
PoolConfig config;
config.requestPoolInitialSize = 8192;
config.threadLocalMaxBlocksPerChunk = 128;
MemoryPool::instance().configure(config);

// 使用全局同步池
auto globalAlloc = MemoryPool::instance().globalAllocator();
std::pmr::vector<int> vec(globalAlloc);

// 使用线程本地池（高性能）
auto threadAlloc = MemoryPool::instance().threadLocalAllocator();
std::pmr::string str("hello", threadAlloc);

// 创建请求级单调池
auto requestPool = MemoryPool::instance().createRequestPool();
std::pmr::polymorphic_allocator<std::byte> requestAlloc(requestPool.get());
std::pmr::vector<char> buffer(requestAlloc);
// 请求结束后，requestPool 析构，整体释放内存

// 查看统计信息
auto stats = MemoryPool::instance().getStats();
std::cout << "当前分配: " << stats.currentBytesAllocated << " bytes\n";
std::cout << "峰值: " << stats.peakBytesAllocated << " bytes\n";
std::cout << "分配次数: " << stats.totalAllocations << "\n";
std::cout << "释放次数: " << stats.totalDeallocations << "\n";

// 重置统计
MemoryPool::instance().resetStats();
```

---

### SslContext

SSL/TLS 上下文配置封装，管理证书、私钥和验证模式。

**头文件：** `src/core/SslContext.h`

#### 构造函数

| 方法                 | 参数             | 说明                       |
| -------------------- | ---------------- | -------------------------- |
| `SslContext()`       | 无               | 默认构造（TLS 自适应方法） |
| `SslContext(method)` | method: SSL 方法 | 以指定 SSL 方法构造        |

#### 公共方法

| 方法                        | 参数                     | 返回值          | 说明                               |
| --------------------------- | ------------------------ | --------------- | ---------------------------------- |
| `loadCertificate(certFile)` | certFile: 证书文件路径   | `void`          | 加载服务端证书（PEM 格式）         |
| `loadPrivateKey(keyFile)`   | keyFile: 私钥文件路径    | `void`          | 加载服务端私钥（PEM 格式）         |
| `loadCaCertificate(caFile)` | caFile: CA 证书文件路径  | `void`          | 加载 CA 证书（用于验证对端）       |
| `setVerifyPeer(verifyPeer)` | verifyPeer: 是否验证对端 | `void`          | 设置是否验证对端证书               |
| `native()`                  | 无                       | `ssl::context&` | 获取底层 Boost.Asio SSL 上下文引用 |

#### 示例

```cpp
#include "core/SslContext.h"

using namespace hical;

// 创建 SSL 上下文
SslContext sslCtx;

// 加载证书和私钥
sslCtx.loadCertificate("server.crt");
sslCtx.loadPrivateKey("server.key");

// 可选：加载 CA 证书并验证客户端
sslCtx.loadCaCertificate("ca.crt");
sslCtx.setVerifyPeer(true);

// 在 HttpServer 中使用
HttpServer server(8443);
server.enableSsl("server.crt", "server.key");
server.start();
```

**生成自签名证书（测试用）：**

```bash
openssl req -x509 -newkey rsa:2048 -keyout server.key -out server.crt \
    -days 365 -nodes -subj "/CN=localhost"
```

---

### Error

错误处理模块，将 Boost.Asio 错误码统一映射为框架内部错误码。

**头文件：** `src/core/Error.h`

#### ErrorCode 枚举

| 枚举值                   | 说明             |
| ------------------------ | ---------------- |
| `hNoError`               | 无错误           |
| `hEof`                   | 对端正常关闭连接 |
| `hConnectionReset`       | 连接被对端重置   |
| `hConnectionRefused`     | 连接被拒绝       |
| `hTimedOut`              | 连接超时         |
| `hConnectionInProgress`  | 连接正在进行中   |
| `hConnectionAborted`     | 连接被中止       |
| `hAddressInUse`          | 地址已在使用中   |
| `hAddressNotAvailable`   | 地址不可用       |
| `hNetworkUnreachable`    | 网络不可达       |
| `hHostUnreachable`       | 主机不可达       |
| `hOperationAborted`      | 操作被取消       |
| `hOperationInProgress`   | 操作正在进行中   |
| `hBrokenPipe`            | 管道破裂         |
| `hPermissionDenied`      | 权限不足         |
| `hTooManyOpenFiles`      | 文件描述符不足   |
| `hWouldBlock`            | 资源暂时不可用   |
| `hSslHandshakeError`     | SSL 握手失败     |
| `hSslInvalidCertificate` | SSL 证书无效     |
| `hSslProtocolError`      | SSL 协议错误     |
| `hUnknown`               | 未知错误         |

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

| 函数                      | 参数             | 返回值         | 说明                 |
| ------------------------- | ---------------- | -------------- | -------------------- |
| `fromBoostError(ec)`      | ec: Boost 错误码 | `ErrorCode`    | 转换为框架错误码     |
| `toNetworkError(ec)`      | ec: Boost 错误码 | `NetworkError` | 转换为网络错误结构体 |
| `errorCodeToString(code)` | code: 错误码     | `const char*`  | 获取错误码描述       |

#### 示例

```cpp
#include "core/Error.h"

using namespace hical;

// 处理 Boost.Asio 错误
void handleAsioError(const boost::system::error_code& ec)
{
    auto err = toNetworkError(ec);
    
    if (err.ok()) {
        std::cout << "无错误" << std::endl;
        return;
    }
    
    if (err.isEof()) {
        std::cout << "对端关闭连接" << std::endl;
        return;
    }
    
    if (err.isCancelled()) {
        std::cout << "操作被取消" << std::endl;
        return;
    }
    
    std::cout << "错误: " << err.message << std::endl;
    std::cout << "错误码: " << errorCodeToString(err.code) << std::endl;
}

// 使用 ErrorCode
ErrorCode code = fromBoostError(ec);
if (code == ErrorCode::hConnectionReset) {
    // 处理连接重置
}
```

---

## 底层抽象

### PmrBuffer

基于 pmr 的统一缓冲区，支持 prepend 区域和自动扩容。底层使用 `std::pmr::vector`。

**头文件：** `src/core/PmrBuffer.h`

**常量：**

| 常量           | 值   | 说明             |
| -------------- | ---- | ---------------- |
| `hDefaultSize` | 2048 | 默认缓冲区大小   |
| `hPrependSize` | 8    | prepend 区域大小 |

**关键方法：**

| 方法                       | 说明                       |
| -------------------------- | -------------------------- |
| `peek()`                   | 获取可读数据起始指针       |
| `readableBytes()`          | 获取可读字节数             |
| `writableBytes()`          | 获取可写字节数             |
| `retrieve(len)`            | 消费指定字节数             |
| `retrieveAll()`            | 消费所有数据               |
| `read(len)`                | 读取指定字节数并返回字符串 |
| `readAll()`                | 读取所有数据并返回字符串   |
| `append(data, len)`        | 追加数据                   |
| `append(str)`              | 追加字符串                 |
| `ensureWritableBytes(len)` | 确保有足够的可写空间       |
| `findCRLF()`               | 查找 CRLF                  |
| `findEOL()`                | 查找换行符                 |
| `swap(rhs)`                | 交换缓冲区                 |
| `get_allocator()`          | 获取底层分配器             |

---

### EventLoop

事件循环抽象接口，定义了生命周期管理、任务调度、定时器和 pmr 分配器支持。

**头文件：** `src/core/EventLoop.h`

**关键方法：**

| 方法                      | 说明                                             |
| ------------------------- | ------------------------------------------------ |
| `run()`                   | 启动事件循环（阻塞）                             |
| `stop()`                  | 停止事件循环                                     |
| `isRunning()`             | 是否正在运行                                     |
| `dispatch(cb)`            | 智能调度任务（当前线程直接执行，否则投递到队列） |
| `post(cb)`                | 投递到队列异步执行                               |
| `runAfter(delay, cb)`     | 延迟执行任务                                     |
| `runEvery(interval, cb)`  | 周期性执行任务                                   |
| `cancelTimer(id)`         | 取消定时器                                       |
| `isInLoopThread()`        | 是否在事件循环线程中                             |
| `index()` / `setIndex(n)` | 获取/设置事件循环索引                            |
| `runOnQuit(cb)`           | 注册退出回调                                     |
| `allocator()`             | 获取关联的 pmr 分配器                            |

**类型定义：**

```cpp
using Func = std::function<void()>;
using TimerId = uint64_t;
enum : TimerId { hInvalidTimerId = 0 };
```

---

### Concepts

C++20 Concept 约束，定义网络后端必须满足的接口要求。

**头文件：** `src/core/Concepts.h`

**Concept 列表：**

| Concept                | 说明                                                                  |
| ---------------------- | --------------------------------------------------------------------- |
| `EventLoopLike<T>`     | 事件循环接口约束（生命周期、任务调度、定时器、pmr）                   |
| `TcpConnectionLike<T>` | TCP 连接接口约束（发送、关闭、状态查询）                              |
| `TimerLike<T>`         | 定时器接口约束（取消、状态、间隔）                                    |
| `NetworkBackend<T>`    | 网络后端统一约束（需提供 EventLoopType / ConnectionType / TimerType） |

**默认后端：**

```cpp
struct AsioBackend
{
    using EventLoopType = AsioEventLoop;
    using ConnectionType = TcpConnection;
    using TimerType = AsioTimer;
};
```

`AsioBackend` 是 hical 的默认后端。未来可通过满足 `NetworkBackend` concept 的新类型实现第二后端。

---

### Asio 适配层

Boost.Asio 适配层，将 Boost.Asio 的原始 API 封装为 hical 风格的接口。

**头文件：** `src/asio/` 目录

| 文件                  | 说明                                                                                                        |
| --------------------- | ----------------------------------------------------------------------------------------------------------- |
| `AsioEventLoop.h`     | EventLoop 的 Asio 实现                                                                                      |
| `AsioTimer.h`         | 定时器的 Asio 实现                                                                                          |
| `GenericConnection.h` | 模板化连接（支持 TCP/SSL），提供 PlainConnection 类型别名；WriteNode 写队列，支持 `sendFile()` 异步文件发送 |
| `SslConnection.h`     | SSL 连接类型别名（`SslConnection`），懒包含 OpenSSL 头文件                                                  |
| `TcpServer.h`         | TCP 服务器，支持 `setIdleTimeout()` 空闲连接超时清理 + `IdleFd` fd 耗尽防护                                 |
| `EventLoopPool.h`     | 事件循环线程池                                                                                              |

通常用户不需要直接使用适配层，`HttpServer` 已封装了全部网络操作。

---

## 日志系统 API

> 命名空间为 `hical`，头文件在 `src/core/`。

---

### Log

日志系统，提供 6 级日志、多种 API 风格和零开销设计。

**头文件：** `src/core/Log.h`

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

| 宏                                        | 说明                              |
| ----------------------------------------- | --------------------------------- |
| `HICAL_LOG_TRACE(fmt, ...)`               | Trace 级别（NDEBUG 下编译期消除） |
| `HICAL_LOG_DEBUG(fmt, ...)`               | Debug 级别                        |
| `HICAL_LOG_INFO(fmt, ...)`                | Info 级别                         |
| `HICAL_LOG_WARN(fmt, ...)`                | Warn 级别                         |
| `HICAL_LOG_ERROR(fmt, ...)`               | Error 级别                        |
| `HICAL_LOG_FATAL(fmt, ...)`               | Fatal 级别（触发 abort）          |
| `HICAL_LOG_INFO_STREAM << val`            | 流式 API                          |
| `HICAL_LOG_INFO_IF(cond, fmt, ...)`       | 条件宏                            |
| `HICAL_LOG_INFO_F(fmt, fields, ...)`      | 结构化字段 API                    |
| `HICAL_LOG_TO("channel", Info, fmt, ...)` | 通道路由                          |

#### 示例

```cpp
#include "core/Log.h"

using namespace hical;

// 基本用法
HICAL_LOG_INFO("服务器启动, port={}", 8080);
HICAL_LOG_ERROR("连接失败: {}", ec.message());

// 流式 API
HICAL_LOG_INFO_STREAM << "请求处理完成, 耗时: " << elapsed << "ms";

// 条件宏
HICAL_LOG_WARN_IF(latency > 100, "慢请求: {}ms", latency);

// 结构化字段
HICAL_LOG_INFO_F("用户登录", {{"userId", 42}, {"ip", "1.2.3.4"}});

// 配置
auto& logger = Logger::instance();
logger.setLevel(LogLevel::hInfo);
logger.setFlushLevel(LogLevel::hWarn);
logger.addSink(std::make_shared<StderrSink>());
```

---

### LogRecord

结构化日志条目，存储单条日志的所有上下文信息。

**头文件：** `src/core/LogRecord.h`

| 字段        | 类型                       | 说明                  |
| ----------- | -------------------------- | --------------------- |
| `level`     | `LogLevel`                 | 日志级别              |
| `timestamp` | `system_clock::time_point` | 时间戳                |
| `threadId`  | `uint64_t`                 | 发出日志的线程 ID     |
| `file`      | `std::string_view`         | 源文件名              |
| `line`      | `int`                      | 源文件行号            |
| `message`   | `std::string`              | 日志消息              |
| `fields`    | `boost::json::object`      | 附加结构化字段        |
| `traceId`   | `std::string`              | 请求 trace-id（可选） |

---

### LogFormatter

日志格式化器接口，决定日志条目的输出文本格式。

**头文件：** `src/core/LogFormatter.h`

| 实现类          | 说明                                                    |
| --------------- | ------------------------------------------------------- |
| `TextFormatter` | 人类可读文本格式，含 `thread_local` 时间戳缓存          |
| `JsonFormatter` | JSON Lines 格式（`boost::json::serialize`，UTC 时间戳） |

所有格式化器实现 `format(const LogRecord&) -> std::string` 接口。

---

### LogSink

可插拔日志输出后端接口。

**头文件：** `src/core/LogSink.h`

| 实现类        | 说明                                             |
| ------------- | ------------------------------------------------ |
| `StderrSink`  | 输出到 stderr（`fprintf`）                       |
| `FileSink`    | 同步写文件（`fwrite` + `LogFile` 轮转）          |
| `OStreamSink` | 线程安全 `ostream` 包装，兼容 `setOutput()` 桥接 |

所有 Sink 实现 `write(const LogRecord&)` 和 `flush()` 接口，可通过 `Logger::addSink()` 注册多个并发输出。

---

### LogFile

日志文件轮转引擎，按大小自动归档历史日志。

**头文件：** `src/core/LogFile.h`

| 构造参数      | 类型          | 默认值 | 说明                       |
| ------------- | ------------- | ------ | -------------------------- |
| `baseName`    | `std::string` | —      | 基础文件名（如 `"app"`）   |
| `maxFileSize` | `size_t`      | 100MB  | 单文件大小阈值，超出后滚动 |
| `maxFiles`    | `size_t`      | `10`   | 保留的归档文件数量         |

归档文件命名格式：`app.YYMMDD-HHMMSS.NNNNNN.log`，旧文件超出 `maxFiles` 后自动删除。

---

### AsyncFileSink

异步双缓冲文件 Sink，避免日志写入阻塞业务线程。

**头文件：** `src/core/AsyncFileSink.h`

- 后台 `std::jthread` 线程 + `stop_token` 优雅关闭
- 前后 4MB 缓冲区交换，`condition_variable_any` 唤醒
- 背压保护：缓冲区满时丢弃并计数，1 秒超时强制刷盘
- 析构时自动排空 `m_curBuf` 中的剩余日志

```cpp
#include "core/AsyncFileSink.h"

AsyncFileSink::Options opts;
opts.file.basePath = "logs/app.log";
opts.file.maxFileSize = 100 * 1024 * 1024;  // 100MB 轮转
opts.file.maxFiles = 10;                     // 保留 10 个归档
opts.bufferSize = 4 * 1024 * 1024;           // 4MB 双缓冲

Logger::instance().addSink(std::make_shared<AsyncFileSink>(opts));
```

---

### FixedBuffer

栈上固定大小缓冲区，用于日志格式化，替代 `std::ostringstream` 以减少堆分配。

**头文件：** `src/core/FixedBuffer.h`

```cpp
template <size_t N = 4096>
class FixedBuffer;
```

| 方法                | 说明                                     |
| ------------------- | ---------------------------------------- |
| `append(data, len)` | 追加原始数据（溢出时自动 fallback 到堆） |
| `appendInt(v)`      | `std::to_chars` 格式化整数               |
| `appendFloat(v)`    | `std::to_chars` 格式化浮点数             |
| `str()`             | 返回 `std::string_view` 视图             |
| `clear()`           | 清空缓冲区（不释放内存）                 |

---

### LogChannel

命名日志通道，支持独立的级别、格式化器和输出后端。

**头文件：** `src/core/LogChannel.h`

`LogChannelRegistry` 以 `shared_mutex` 管理所有通道（读多写少）。通道不存在时 `HICAL_LOG_TO` 静默丢弃。

| 方法（LogChannel）        | 说明             |
| ------------------------- | ---------------- |
| `setLevel(level)`         | 设置通道级别     |
| `setFormatter(formatter)` | 设置格式化器     |
| `addSink(sink)`           | 添加输出 Sink    |
| `emit(record)`            | 发射一条日志条目 |

| 方法（LogChannelRegistry） | 说明               |
| -------------------------- | ------------------ |
| `get(name)`                | 获取或创建命名通道 |
| `remove(name)`             | 删除命名通道       |
| `has(name)`                | 是否存在指定通道   |

```cpp
#include "core/LogChannel.h"

// 配置 access 通道
auto ch = LogChannelRegistry::instance().get("access");
ch->setFormatter(std::make_shared<JsonFormatter>());
ch->addSink(std::make_shared<FileSink>("logs/access"));

// 向通道写日志
HICAL_LOG_TO("access", Info, "GET /api/users 200");
```

---

### LogMiddleware

洋葱模型日志中间件，自动生成 trace-id 并记录结构化访问日志。

**头文件：** `src/core/LogMiddleware.h`

#### makeLogMiddleware 函数

```cpp
MiddlewareHandler makeLogMiddleware(const std::string& channelName = "access");
```

- 自动为每个请求生成 128 位十六进制 trace-id（OpenSSL RAND_bytes）
- 将 trace-id 注入 `req.setAttribute("hical.trace_id", ...)`
- 请求完成后记录结构化访问日志到指定通道（method/path/status/latency_ms）

#### 示例

```cpp
#include "core/LogMiddleware.h"

server.use(makeLogMiddleware());       // 默认通道 "access"
server.use(makeLogMiddleware("api"));  // 自定义通道
```

---

### LogAdmin

动态日志级别管理端点，支持运行时调整日志级别。

**头文件：** `src/core/LogAdmin.h`

#### registerLogAdmin 函数

```cpp
void registerLogAdmin(Router& router, const std::string& prefix = "/admin");
```

注册两个端点：
- `GET {prefix}/log-level` — 查询当前所有日志级别（默认级别 + 各通道级别）
- `PUT {prefix}/log-level` — 调整日志级别，请求体：`{"level":"info"}` 或 `{"channel":"access","level":"debug"}`

#### 示例

```cpp
#include "core/LogAdmin.h"

registerLogAdmin(server.router());               // → GET/PUT /admin/log-level
registerLogAdmin(server.router(), "/internal");  // → GET/PUT /internal/log-level
```

---

## 附录

### 类型别名速查

| 别名                | 定义                                                                    | 头文件         |
| ------------------- | ----------------------------------------------------------------------- | -------------- |
| `Awaitable<T>`      | `boost::asio::awaitable<T>`                                             | `Coroutine.h`  |
| `RouteHandler`      | `function<Awaitable<HttpResponse>(const HttpRequest&)>`                 | `Router.h`     |
| `SyncRouteHandler`  | `function<HttpResponse(const HttpRequest&)>`                            | `Router.h`     |
| `MiddlewareNext`    | `function<Awaitable<HttpResponse>(HttpRequest&)>`                 | `Middleware.h` |
| `MiddlewareHandler` | `function<Awaitable<HttpResponse>(HttpRequest&, MiddlewareNext)>` | `Middleware.h` |
| `WsMessageCallback`      | `function<Awaitable<void>(const string&, WebSocketSession&)>`           | `Router.h`     |
| `WsTypedMessageCallback` | `function<Awaitable<void>(const WsMessage&, WebSocketSession&)>`        | `Router.h`     |
| `WsConnectCallback`      | `function<Awaitable<void>(WebSocketSession&)>`                          | `Router.h`     |
| `WsDisconnectCallback`   | `function<void(WebSocketSession&)>`                                     | `Router.h`     |
| `WsConnectionId`         | `uint64_t`                                                              | `WsHub.h`      |
| `Func`              | `function<void()>`                                                      | `EventLoop.h`  |
| `TimerId`           | `uint64_t`                                                              | `EventLoop.h`  |
| `ErrorHandler`      | `function<HttpResponse(const exception&, const HttpRequest&)>`          | `HttpServer.h` |

### 回调类型速查

| 场景     | 类型签名                                                      | 说明               |
| -------- | ------------------------------------------------------------- | ------------------ |
| 同步路由 | `HttpResponse(const HttpRequest&)`                            | 直接返回响应       |
| 协程路由 | `Awaitable<HttpResponse>(const HttpRequest&)`                 | 协程返回响应       |
| 中间件   | `Awaitable<HttpResponse>(HttpRequest&, MiddlewareNext)` | 洋葱模型           |
| WS 消息  | `Awaitable<void>(const string&, WebSocketSession&)`           | WebSocket 文本消息回调 |
| WS typed | `Awaitable<void>(const WsMessage&, WebSocketSession&)`        | WebSocket typed 回调（区分 Text/Binary） |
| WS 连接  | `Awaitable<void>(WebSocketSession&)`                          | WebSocket 连接回调 |
| WS 断开  | `void(WebSocketSession&)`                                     | WebSocket 断开回调 |
| 定时器   | `void()`                                                      | 无参回调           |

---

### Cookie

Cookie 解析（请求侧）与设置（响应侧）支持，符合 RFC 6265。

**头文件：** `src/core/Cookie.h`（`CookieOptions`），`src/core/HttpRequest.h`，`src/core/HttpResponse.h`

#### CookieOptions 结构体

| 字段       | 类型          | 默认值  | 说明                                          |
| ---------- | ------------- | ------- | --------------------------------------------- |
| `path`     | `std::string` | `"/"`   | Cookie 作用路径                               |
| `domain`   | `std::string` | `""`    | Cookie 作用域（空=当前域）                    |
| `maxAge`   | `int`         | `-1`    | 有效期（秒），-1 表示会话 Cookie              |
| `httpOnly` | `bool`        | `true`  | 防 XSS：禁止 JS 访问（默认开启）              |
| `secure`   | `bool`        | `true`  | 仅 HTTPS 传输（默认开启，开发环境需显式关闭） |
| `sameSite` | `std::string` | `"Lax"` | SameSite 策略（`Lax`/`Strict`/`None`）        |

#### HttpRequest Cookie API

| 方法              | 参数            | 返回值                                               | 说明                              |
| ----------------- | --------------- | ---------------------------------------------------- | --------------------------------- |
| `cookie(name)`    | name: Cookie 名 | `const std::string&`                                 | 获取指定 Cookie 值（懒解析+缓存） |
| `cookies()`       | 无              | `const std::unordered_map<std::string,std::string>&` | 获取所有 Cookie（同名取第一个值） |
| `hasCookie(name)` | name: Cookie 名 | `bool`                                               | 是否存在指定 Cookie               |

#### HttpResponse Cookie API

| 方法                           | 参数                                                       | 返回值 | 说明                                     |
| ------------------------------ | ---------------------------------------------------------- | ------ | ---------------------------------------- |
| `setCookie(name, value, opts)` | name: Cookie 名<br>value: Cookie 值<br>opts: CookieOptions | `void` | 追加 `Set-Cookie` 响应头（防 CRLF 注入） |

#### 示例

```cpp
// 读取 Cookie
server.router().get("/profile", [](const HttpRequest& req) -> HttpResponse {
    auto token = req.cookie("token");
    if (token.empty()) {
        return HttpResponse::badRequest("未登录");
    }
    return HttpResponse::ok("Hello " + token);
});

// 设置 Cookie
server.router().post("/login", [](const HttpRequest& req) -> HttpResponse {
    CookieOptions opts;
    opts.maxAge   = 3600;
    opts.httpOnly = true;
    opts.sameSite = "Lax";

    HttpResponse res = HttpResponse::ok("登录成功");
    res.setCookie("token", "user_jwt_here", opts);
    return res;
});
```

---

### StaticFiles

静态文件服务工厂函数，将 URL 前缀映射到本地目录。

**头文件：** `src/core/StaticFiles.h`

#### serveStatic 函数

```cpp
std::function<Awaitable<HttpResponse>(const HttpRequest&)> serveStatic(
    const std::string& rootDir,
    const std::string& urlPrefix,
    std::uintmax_t maxFileSize = 64ULL * 1024 * 1024);
```

| 参数          | 说明                                       |
| ------------- | ------------------------------------------ |
| `rootDir`     | 本地根目录（如 `"./public"`）              |
| `urlPrefix`   | URL 前缀（如 `"/static/"`）                |
| `maxFileSize` | 单文件大小上限（默认 64 MB，超出返回 413） |

**返回值：** `RouteHandler`（协程版），可直接传入 `router.get()`。调用方需在协程中 `co_await`。

**功能特性：**

| 特性          | 说明                                                          |
| ------------- | ------------------------------------------------------------- |
| 异步文件读取  | 支持 `BOOST_ASIO_HAS_FILE` 异步 I/O，否则 ifstream 回退       |
| 路径缓存      | `PathCache`（4096 条目 / 60s TTL）减少 `canonical()` 系统调用 |
| MIME 自动推断 | 支持 27 种扩展名（html/css/js/png/svg/mp4 等）                |
| 目录默认文件  | 访问目录时自动返回 `index.html`                               |
| ETag 缓存验证 | `If-None-Match` 匹配时返回 304                                |
| 路径遍历防护  | `../` 等跳出根目录的路径返回 403                              |
| 大文件保护    | 超过 `maxFileSize` 返回 413                                   |

#### 示例

```cpp
#include "core/StaticFiles.h"

// 将 /static/... 映射到 ./public 目录
server.router().get("/static/{path}", hical::serveStatic("./public", "/static/"));

// 限制单文件大小为 1 MB
server.router().get("/assets/{path}",
    hical::serveStatic("./assets", "/assets/", 1ULL * 1024 * 1024));
```

---

### Multipart

`multipart/form-data` 上传解析器（RFC 7578）。

**头文件：** `src/core/Multipart.h`

#### MultipartPart 结构体

| 字段          | 类型                                          | 说明                         |
| ------------- | --------------------------------------------- | ---------------------------- |
| `name`        | `std::string`                                 | 字段名（`name` 属性）        |
| `filename`    | `std::string`                                 | 原始文件名（文件字段才有值） |
| `contentType` | `std::string`                                 | Part 的 Content-Type         |
| `data`        | `std::string`                                 | Part 数据内容（二进制安全）  |
| `headers`     | `std::unordered_map<std::string,std::string>` | 所有 Part 头（键已转小写）   |
| `isFile()`    | `bool`                                        | 是否为文件字段               |

#### MultipartParser 静态方法

| 方法                         | 参数                                   | 返回值                                      | 说明                                        |
| ---------------------------- | -------------------------------------- | ------------------------------------------- | ------------------------------------------- |
| `parse(req)`                 | req: HTTP 请求                         | `std::optional<std::vector<MultipartPart>>` | 解析全部 Part（超 256 个返回 nullopt）      |
| `getFile(req, fieldName)`    | req: 请求<br>fieldName: 字段名         | `std::optional<MultipartPart>`              | 获取指定文件 Part（每次重新解析）           |
| `getField(req, fieldName)`   | req: 请求<br>fieldName: 字段名         | `std::optional<std::string>`                | 获取文本字段值（每次重新解析）              |
| `getFile(parts, fieldName)`  | parts: 预解析结果<br>fieldName: 字段名 | `std::optional<MultipartPart>`              | 从预解析结果中搜索文件 Part（零拷贝，推荐） |
| `getField(parts, fieldName)` | parts: 预解析结果<br>fieldName: 字段名 | `std::optional<std::string>`                | 从预解析结果中搜索文本字段（零拷贝，推荐）  |

#### 示例

```cpp
#include "core/Multipart.h"

server.router().post("/upload", [](const HttpRequest& req) -> HttpResponse {
    // 推荐：先解析一次，再用预解析结果查询（避免重复解析）
    auto parts = MultipartParser::parse(req);
    if (!parts) {
        return HttpResponse::badRequest("无效的 multipart 数据");
    }

    auto file = MultipartParser::getFile(*parts, "avatar");
    if (!file) {
        return HttpResponse::badRequest("未找到 avatar 字段");
    }

    // file->filename  — 原始文件名
    // file->contentType — MIME 类型
    // file->data        — 文件内容（二进制）

    // 获取文本字段
    auto desc = MultipartParser::getField(*parts, "description");

    return HttpResponse::ok("上传成功: " + file->filename);
});
```

---

### Session

内存 Session 会话管理，通过中间件自动与 Cookie 联动。

**头文件：** `src/core/Session.h`

#### SessionOptions 结构体

| 字段          | 类型          | 默认值            | 说明                                           |
| ------------- | ------------- | ----------------- | ---------------------------------------------- |
| `cookieName`  | `std::string` | `"HICAL_SESSION"` | Session Cookie 名称                            |
| `maxAge`      | `int`         | `3600`            | Session 有效期（秒）                           |
| `httpOnly`    | `bool`        | `true`            | Cookie HttpOnly                                |
| `secure`      | `bool`        | `true`            | Cookie Secure（仅 HTTPS，开发环境需显式关闭）  |
| `sameSite`    | `std::string` | `"Lax"`           | Cookie SameSite 策略                           |
| `path`        | `std::string` | `"/"`             | Cookie 作用路径                                |
| `gcInterval`  | `int`         | `300`             | 懒 GC 触发间隔（秒），≤0 则禁用 GC             |
| `maxSessions` | `size_t`      | `100000`          | 最大 Session 数量（0=不限制），防 DoS 内存耗尽 |

#### Session 类（线程安全，内部使用 `std::shared_mutex` 读写锁）

| 方法             | 参数                     | 返回值                     | 说明                                     |
| ---------------- | ------------------------ | -------------------------- | ---------------------------------------- |
| `id()`           | 无                       | `const std::string&`       | 获取 Session ID（只读）                  |
| `set(key, v)`    | key: 键<br>v: 任意类型值 | `void`                     | 设置属性（自动 dirty）                   |
| `get<T>(key)`    | key: 键                  | `std::optional<T>`         | 获取属性（类型安全）                     |
| `has(key)`       | key: 键                  | `bool`                     | 检查属性是否存在                         |
| `remove(key)`    | key: 键                  | `void`                     | 删除指定属性                             |
| `clear()`        | 无                       | `void`                     | 清空所有属性                             |
| `isDirty()`      | 无                       | `bool`                     | 是否已修改（需刷新 Cookie）              |
| `touch()`        | 无                       | `void`                     | 更新最后访问时间                         |
| `lastAccess()`   | 无                       | `steady_clock::time_point` | 获取最后访问时间                         |
| `migrateFrom(o)` | o: 另一个 Session 引用   | `void`                     | 原子迁移数据（地址序双锁防死锁，清空源） |

#### SessionManager 类

| 方法             | 参数              | 返回值                        | 说明                                                               |
| ---------------- | ----------------- | ----------------------------- | ------------------------------------------------------------------ |
| `find(id)`       | id: Session ID    | `shared_ptr<Session>`（可空） | 查找 Session（过期条目返回 nullptr，不立即删除，由 gc() 清理）     |
| `create()`       | 无                | `shared_ptr<Session>`（可空） | 创建新 Session（含懒 GC），达到 `maxSessions` 上限时返回 `nullptr` |
| `destroy(id)`    | id: Session ID    | `void`                        | 销毁 Session（登出）                                               |
| `regenerate(id)` | id: 旧 Session ID | `shared_ptr<Session>`（可空） | 重新生成 ID 并迁移数据（防 Session 固定攻击），旧 ID 失效          |
| `gc()`           | 无                | `void`                        | 清理过期 Session                                                   |
| `count()`        | 无                | `size_t`                      | 当前活跃 Session 数                                                |
| `options()`      | 无                | `const SessionOptions&`       | 获取配置                                                           |

#### makeSessionMiddleware 函数

```cpp
MiddlewareHandler makeSessionMiddleware(std::shared_ptr<SessionManager> manager);
```

中间件工作流程：
1. 从 Cookie 读取 Session ID，查找或创建 Session
2. 将 `shared_ptr<Session>` 注入 `HttpRequest` attribute（键：`SessionManager::hSessionKey`）
3. 请求处理完毕后，若 Session 被 dirty，自动刷新 `Set-Cookie`

#### 示例

```cpp
#include "core/Session.h"

// 启动时注册 Session 中间件
auto sessionMgr = std::make_shared<hical::SessionManager>();
server.use(hical::makeSessionMiddleware(sessionMgr));

// 在路由中使用 Session
server.router().post("/login", [](const HttpRequest& req) -> HttpResponse {
    auto session = req.getAttribute<std::shared_ptr<hical::Session>>(
        hical::SessionManager::hSessionKey);
    if (!session) {
        return HttpResponse::serverError();
    }
    (*session)->set("user", std::string("alice"));
    return HttpResponse::ok("登录成功");
});

server.router().get("/profile", [](const HttpRequest& req) -> HttpResponse {
    auto session = req.getAttribute<std::shared_ptr<hical::Session>>(
        hical::SessionManager::hSessionKey);
    if (!session || !(*session)->has("user")) {
        return HttpResponse::badRequest("未登录");
    }
    auto user = (*session)->get<std::string>("user");
    return HttpResponse::ok("Hello " + user.value_or("unknown"));
});

server.router().post("/logout", [](const HttpRequest& req) -> HttpResponse {
    auto session = req.getAttribute<std::shared_ptr<hical::Session>>(
        hical::SessionManager::hSessionKey);
    if (session) {
        sessionMgr->destroy((*session)->id());
    }
    return HttpResponse::ok("已退出");
});
```

---

### CORS 中间件

跨域资源共享（CORS）中间件，符合 W3C CORS 规范。

**头文件：** `src/core/Cors.h`

#### CorsOptions 结构体

| 字段               | 类型                       | 默认值                                            | 说明                           |
| ------------------ | -------------------------- | ------------------------------------------------- | ------------------------------ |
| `allowedOrigins`   | `std::vector<std::string>` | `{"*"}`                                           | 允许的源列表（`"*"` 为通配符） |
| `allowedMethods`   | `std::vector<std::string>` | `{"GET","POST","PUT","DELETE","PATCH","OPTIONS"}` | 允许的 HTTP 方法               |
| `allowedHeaders`   | `std::vector<std::string>` | `{"Content-Type","Authorization"}`                | 允许的请求头                   |
| `exposeHeaders`    | `std::vector<std::string>` | `{}`                                              | 暴露给浏览器的响应头           |
| `allowCredentials` | `bool`                     | `false`                                           | 是否允许凭证（Cookie 等）      |
| `maxAge`           | `int`                      | `86400`                                           | 预检缓存时间（秒）             |

#### makeCorsMiddleware 函数

```cpp
MiddlewareHandler makeCorsMiddleware(CorsOptions opts = {});
```

创建 CORS 中间件。自动处理 OPTIONS 预检请求，添加 `Vary: Origin` 缓存提示。凭证模式下禁止 `"*"` 通配符（安全校验）。

#### 示例

```cpp
#include "core/Cors.h"

// 允许所有源（默认配置）
server.use(makeCorsMiddleware());

// 精确匹配源 + 凭证模式
CorsOptions opts;
opts.allowedOrigins    = {"https://example.com", "https://app.example.com"};
opts.allowCredentials  = true;
opts.allowedHeaders    = {"Content-Type", "Authorization", "X-Custom-Header"};
server.use(makeCorsMiddleware(opts));
```

---

### RouteGroup

路由组，为一组路由设置公共前缀和组级中间件。支持多层嵌套。

**头文件：** `src/core/RouteGroup.h`

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
#include "core/RouteGroup.h"

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

## 数据库中间件 API

> 需要在 CMake 构建时开启 `HICAL_WITH_DATABASE=ON`，并链接 Boost.MySQL。命名空间为 `hical::db`，头文件在 `src/db/`。

---

### DbConfig

数据库连接池全局配置结构体。

**头文件：** `src/db/DbConfig.h`

#### 字段说明

| 字段                  | 类型                   | 默认值      | 说明                                            |
| --------------------- | ---------------------- | ----------- | ----------------------------------------------- |
| `host`                | `std::string`          | `127.0.0.1` | 数据库主机地址                                  |
| `port`                | `uint16_t`             | `3306`      | 数据库端口                                      |
| `user`                | `std::string`          | `""`        | 登录用户名                                      |
| `password`            | `std::string`          | `""`        | 登录密码                                        |
| `database`            | `std::string`          | `""`        | 默认数据库名                                    |
| `charset`             | `std::string`          | `"utf8mb4"` | 连接字符集                                      |
| `minConnections`      | `size_t`               | `2`         | 连接池最小连接数（启动时预热）                  |
| `maxConnections`      | `size_t`               | `16`        | 连接池最大连接数                                |
| `idleTimeout`         | `std::chrono::seconds` | `300s`      | 空闲连接超时后回收                              |
| `acquireTimeout`      | `std::chrono::seconds` | `5s`        | 从池中获取连接的等待超时                        |
| `queryTimeout`        | `std::chrono::seconds` | `30s`       | 单条查询执行超时                                |
| `autoReconnect`       | `bool`                 | `true`      | 连接断开时是否自动重连                          |
| `idleCheckInterval`   | `std::chrono::seconds` | `60s`       | 定期检查空闲连接的间隔                          |
| `healthCheckInterval` | `std::chrono::seconds` | `30s`       | 定期 ping 健康检查的间隔                        |
| `pingGracePeriod`     | `std::chrono::seconds` | `15s`       | ping 超时宽限期                                 |
| `stmtCacheSize`       | `size_t`               | `64`        | 每条连接的 PreparedStatement 缓存上限（0=禁用） |

---

### DbResult

查询结果结构体，包含返回行、列名以及 DML 影响行数。

**头文件：** `src/db/DbResult.h`

#### 字段说明

| 字段           | 类型                                    | 说明                          |
| -------------- | --------------------------------------- | ----------------------------- |
| `columns`      | `std::vector<std::string>`              | 列名列表（SELECT 查询时填充） |
| `rows`         | `std::vector<std::vector<std::string>>` | 结果行，每行为字符串列值列表  |
| `affectedRows` | `uint64_t`                              | DML 操作影响的行数            |
| `insertId`     | `uint64_t`                              | INSERT 操作自动生成的主键值   |

#### 方法

| 方法                | 返回值                      | 说明                                           |
| ------------------- | --------------------------- | ---------------------------------------------- |
| `empty()`           | `bool`                      | 结果集是否为空（无行数据）                     |
| `size()`            | `size_t`                    | 结果行数                                       |
| `operator[](index)` | `std::vector<std::string>&` | 按行下标访问                                   |
| `columnIndex(name)` | `size_t`                    | 按列名查找下标，未找到返回 `std::string::npos` |

---

### DbConnection

数据库连接抽象基类，所有 I/O 方法均为协程接口。

**头文件：** `src/db/DbConnection.h`

#### 查询方法

| 方法                          | 参数                                                  | 返回值                | 说明                               |
| ----------------------------- | ----------------------------------------------------- | --------------------- | ---------------------------------- |
| `query(sql, params)`          | sql: SQL 语句<br>params: 参数列表（`vector<string>`） | `Awaitable<DbResult>` | 参数化查询（推荐，防 SQL 注入）    |
| `execute(sql, params)`        | sql: SQL 语句<br>params: 参数列表（`vector<string>`） | `Awaitable<DbResult>` | 参数化执行（INSERT/UPDATE/DELETE） |
| `query(sql)` *(deprecated)*   | sql: 原始 SQL                                         | `Awaitable<DbResult>` | 直接查询，已标记 `[[deprecated]]`  |
| `execute(sql)` *(deprecated)* | sql: 原始 SQL                                         | `Awaitable<DbResult>` | 直接执行，已标记 `[[deprecated]]`  |

#### 事务方法

| 方法                 | 返回值            | 说明               |
| -------------------- | ----------------- | ------------------ |
| `beginTransaction()` | `Awaitable<void>` | 开启事务           |
| `commit()`           | `Awaitable<void>` | 提交事务           |
| `rollback()`         | `Awaitable<void>` | 回滚事务           |
| `inTransaction()`    | `bool`            | 当前是否处于事务中 |

#### 状态方法

| 方法               | 返回值                                  | 说明                     |
| ------------------ | --------------------------------------- | ------------------------ |
| `isAlive()`        | `bool`                                  | 连接是否存活             |
| `ping()`           | `Awaitable<bool>`                       | 发送 ping 包检查连通性   |
| `backend()`        | `std::string_view`                      | 后端名称（如 `"mysql"`） |
| `lastActiveTime()` | `std::chrono::steady_clock::time_point` | 最后一次活跃时间         |
| `lastPingTime()`   | `std::chrono::steady_clock::time_point` | 最后一次 ping 时间       |
| `touch()`          | `void`                                  | 更新最后活跃时间戳       |

---

### DbConnectionPool

协程化数据库连接池，负责连接生命周期管理、空闲回收和健康检查。

**头文件：** `src/db/DbConnectionPool.h`

#### 工厂类型

```cpp
using DbConnectionFactory =
    std::function<Awaitable<std::shared_ptr<DbConnection>>(
        boost::asio::io_context&, DbConfig&)>;
```

#### 构造函数

| 方法                                       | 参数                                                                       | 说明       |
| ------------------------------------------ | -------------------------------------------------------------------------- | ---------- |
| `DbConnectionPool(ioCtx, config, factory)` | ioCtx: io_context 引用<br>config: DbConfig<br>factory: DbConnectionFactory | 创建连接池 |

#### 公共方法

| 方法            | 参数               | 返回值                                     | 说明                                       |
| --------------- | ------------------ | ------------------------------------------ | ------------------------------------------ |
| `init()`        | 无                 | `Awaitable<void>`                          | 预热连接池（建立 `minConnections` 条连接） |
| `acquire()`     | 无                 | `Awaitable<std::shared_ptr<DbConnection>>` | 从池中获取一条连接（超时抛出异常）         |
| `release(conn)` | conn: 连接智能指针 | `void`                                     | 归还连接到池中                             |
| `shutdown()`    | 无                 | `void`                                     | 关闭连接池，回收所有连接                   |

#### 统计方法

| 方法             | 返回值   | 说明                 |
| ---------------- | -------- | -------------------- |
| `activeCount()`  | `size_t` | 当前借出的连接数     |
| `idleCount()`    | `size_t` | 当前空闲的连接数     |
| `waitingCount()` | `size_t` | 当前等待获取连接的数 |
| `totalCount()`   | `size_t` | 池中总连接数         |

#### 常量键

| 常量       | 值                | 说明                                |
| ---------- | ----------------- | ----------------------------------- |
| `hPoolKey` | `"hical.db.pool"` | 连接池注入到 Request attribute 的键 |
| `hConnKey` | `"hical.db.conn"` | 连接注入到 Request attribute 的键   |

---

### DbMiddleware

将连接池与单次请求的连接获取/释放封装为中间件，支持自动事务。

**头文件：** `src/db/DbMiddleware.h`

#### DbMiddlewareOptions 结构体

| 字段              | 类型   | 默认值  | 说明                                               |
| ----------------- | ------ | ------- | -------------------------------------------------- |
| `autoTransaction` | `bool` | `false` | 是否对每个请求自动包裹事务（成功提交，异常回滚）   |
| `injectPool`      | `bool` | `true`  | 是否同时将连接池本身注入 Request（键：`hPoolKey`） |

#### 函数

| 函数                           | 参数                                                      | 返回值                              | 说明                     |
| ------------------------------ | --------------------------------------------------------- | ----------------------------------- | ------------------------ |
| `makeDbMiddleware(pool, opts)` | pool: 连接池智能指针<br>opts: DbMiddlewareOptions（可选） | `MiddlewareHandler`                 | 创建 DB 中间件           |
| `getDbConnection(req)`         | req: HTTP 请求                                            | `std::shared_ptr<DbConnection>`     | 从请求中取出已注入的连接 |
| `getDbPool(req)`               | req: HTTP 请求                                            | `std::shared_ptr<DbConnectionPool>` | 从请求中取出连接池       |

---

### DbQueryLog

查询日志中间件，记录每次请求中执行的全部 SQL，支持慢查询告警。

**头文件：** `src/db/DbQueryLog.h`

> 注意：必须在 `makeDbMiddleware()` **之后**注册，否则无法拦截查询。

#### QueryLogEntry 结构体

| 字段              | 类型                        | 说明             |
| ----------------- | --------------------------- | ---------------- |
| `sql`             | `std::string`               | 执行的 SQL 语句  |
| `duration`        | `std::chrono::microseconds` | 查询执行耗时     |
| `rowCount`        | `uint64_t`                  | SELECT 返回行数  |
| `affectedRows`    | `uint64_t`                  | DML 影响行数     |
| `isParameterized` | `bool`                      | 是否为参数化查询 |

#### QueryLogOptions 结构体

| 字段                 | 类型                                                     | 默认值    | 说明                                       |
| -------------------- | -------------------------------------------------------- | --------- | ------------------------------------------ |
| `onRequestComplete`  | `std::function<void(const std::vector<QueryLogEntry>&)>` | `nullptr` | 请求完成时回调，接收本次请求的全部日志条目 |
| `slowQueryThreshold` | `std::chrono::microseconds`                              | `0`       | 慢查询阈值（0=禁用慢查询告警）             |
| `onSlowQuery`        | `std::function<void(const QueryLogEntry&)>`              | `nullptr` | 单条慢查询触发时的回调                     |

#### 函数

| 函数                           | 参数                  | 返回值              | 说明               |
| ------------------------------ | --------------------- | ------------------- | ------------------ |
| `makeQueryLogMiddleware(opts)` | opts: QueryLogOptions | `MiddlewareHandler` | 创建查询日志中间件 |

#### 常量键

| 常量           | 值                    | 说明                                      |
| -------------- | --------------------- | ----------------------------------------- |
| `hQueryLogKey` | `"hical.db.queryLog"` | 查询日志列表注入到 Request attribute 的键 |

---

### MysqlConnection

基于 Boost.MySQL 的 MySQL 后端实现，受 `HICAL_HAS_DATABASE` 宏保护。

**头文件：** `src/db/MysqlConnection.h`

#### 静态方法

| 方法                    | 参数                                            | 返回值                                        | 说明                                     |
| ----------------------- | ----------------------------------------------- | --------------------------------------------- | ---------------------------------------- |
| `create(ioCtx, config)` | ioCtx: io_context 引用<br>config: DbConfig 引用 | `Awaitable<std::shared_ptr<MysqlConnection>>` | 异步建立连接并返回实例                   |
| `makeFactory()`         | 无                                              | `DbConnectionFactory`                         | 生成可传入 `DbConnectionPool` 的工厂函数 |

---

### StmtCache

PreparedStatement LRU 缓存，按 SQL 文本为键缓存已编译的语句句柄，减少重复 PREPARE 开销。

**头文件：** `src/db/StmtCache.h`

#### 构造函数

| 方法                 | 参数                                       | 说明              |
| -------------------- | ------------------------------------------ | ----------------- |
| `StmtCache(maxSize)` | maxSize: 最大缓存条目数（默认 64，0=禁用） | 创建 LRU 语句缓存 |

#### 公共方法

| 方法                | 参数                            | 返回值                       | 说明                                       |
| ------------------- | ------------------------------- | ---------------------------- | ------------------------------------------ |
| `find(sql)`         | sql: SQL 语句文本               | `statement*`（可为 nullptr） | 查找缓存中的语句句柄，未命中返回 `nullptr` |
| `insert(key, stmt)` | key: SQL 文本<br>stmt: 语句句柄 | `std::optional<statement>`   | 插入新条目，若触发淘汰则返回被淘汰的语句   |
| `erase(sql)`        | sql: SQL 文本                   | `void`                       | 主动删除指定条目                           |
| `clear()`           | 无                              | `std::vector<statement>`     | 清空全部条目并返回所有语句（供调用方关闭） |
| `size()`            | 无                              | `size_t`                     | 当前缓存条目数                             |
| `maxSize()`         | 无                              | `size_t`                     | 缓存容量上限                               |

---

### 综合示例

以下示例展示连接池、DB 中间件、查询日志的完整集成用法。

```cpp
#include "core/HttpServer.h"
#include "db/DbConfig.h"
#include "db/DbConnectionPool.h"
#include "db/DbMiddleware.h"
#include "db/DbQueryLog.h"
#include "db/MysqlConnection.h"

using namespace hical;
using namespace hical::db;

int main()
{
    HttpServer server(8080);
    boost::asio::io_context& ioCtx = /* 从 server 获取 */;

    // 1. 配置数据库
    DbConfig dbConfig;
    dbConfig.host = "127.0.0.1";
    dbConfig.user = "root";
    dbConfig.password = "secret";
    dbConfig.database = "myapp";
    dbConfig.minConnections = 4;
    dbConfig.maxConnections = 32;

    // 2. 创建连接池
    auto pool = std::make_shared<DbConnectionPool>(
        ioCtx, dbConfig, MysqlConnection::makeFactory());

    // 3. 注册中间件（顺序重要：DB 在前，QueryLog 在后）
    server.use(makeDbMiddleware(pool, {.autoTransaction = true}));
    server.use(makeQueryLogMiddleware({
        .slowQueryThreshold = std::chrono::milliseconds(100),
        .onSlowQuery = [](const QueryLogEntry& entry) {
            std::cerr << "[SLOW] " << entry.sql
                      << " (" << entry.duration.count() << "us)" << std::endl;
        }
    }));

    // 4. 在路由中使用
    server.router().get("/users/{id}",
        [](const HttpRequest& req) -> Awaitable<HttpResponse> {
            auto conn = getDbConnection(req);
            auto result = co_await conn->query(
                "SELECT * FROM users WHERE id = ?",
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

> 需要在 CMake 构建时开启 `HICAL_WITH_OPENAPI=ON`（默认已开启）。命名空间为 `hical`，头文件在 `src/core/`。所有代码由 `HICAL_HAS_OPENAPI` 宏保护。

---

### OpenApiSchema

C++20 模板驱动的 JSON Schema 生成器，从 `HICAL_JSON` 宏描述的类型元数据自动生成符合 OpenAPI 3.0 规范的 Schema Object。

**头文件：** `src/core/OpenApiSchema.h`

#### 函数

| 函数                             | 参数                                    | 返回值                | 说明                                                                   |
| -------------------------------- | --------------------------------------- | --------------------- | ---------------------------------------------------------------------- |
| `jsonSchema<T>()`                | 无（模板参数 T 需有 `HICAL_JSON` 描述） | `boost::json::object` | 生成 OpenAPI 3.0 Schema Object，支持基本类型/vector/嵌套结构体/$ref    |
| `collectSchemas<T>(schemas)`     | schemas: 输出的 schema map 引用         | `void`                | 递归收集 T 及其所有嵌套类型的 schema，写入 `map<string, json::object>` |
| `registerSchemas<Types...>(doc)` | doc: OpenApiDocument 引用               | `void`                | 批量注册多个类型的 schema 到文档中                                     |

#### 宏

| 宏                                | 参数                                    | 说明                                                                |
| --------------------------------- | --------------------------------------- | ------------------------------------------------------------------- |
| `HICAL_SCHEMA_NAME(Type, "name")` | Type: C++ 类型<br>"name": schema 引用名 | 注册类型的 $ref 名称，用于跨类型引用（`#/components/schemas/name`） |

#### 类型映射规则

| C++ 类型                 | OpenAPI Schema 类型                |
| ------------------------ | ---------------------------------- |
| `bool`                   | `boolean`                          |
| `int`, `int32_t`         | `integer`, format: int32           |
| `int64_t`, `long`        | `integer`, format: int64           |
| `float`                  | `number`, format: float            |
| `double`                 | `number`, format: double           |
| `std::string`            | `string`                           |
| `std::vector<T>`         | `array`, items: schema of T        |
| 带 `HICAL_JSON` 的结构体 | `object` 或 `$ref`（若注册了名称） |

#### 示例

```cpp
#include "core/OpenApiSchema.h"
#include "core/MetaJson.h"

HICAL_SCHEMA_NAME(UserDTO, "User")

struct UserDTO
{
    HICAL_JSON(UserDTO,
        REQUIRED(id),
        name,
        ALIAS(emailAddr, "email")
    )
    int id;
    std::string name;
    std::string emailAddr;
};

// 生成单个 schema
auto schema = hical::jsonSchema<UserDTO>();
// 结果：{"type":"object","required":["id"],"properties":{...}}

// 收集嵌套 schema（含 UserDTO 自身及其所有嵌套类型）
std::map<std::string, boost::json::object> schemas;
hical::collectSchemas<UserDTO>(schemas);
```

---

### OpenApiRegistry

线程安全的路由 API 元数据注册表，存储每条路由的 summary、tags、requestBody、responses 等标注信息。

**头文件：** `src/core/OpenApiRegistry.h`

#### RouteApiInfo 结构体

| 字段          | 类型                                 | 默认值 | 说明                             |
| ------------- | ------------------------------------ | ------ | -------------------------------- |
| `method`      | `std::string`                        | `""`   | HTTP 方法（"GET"/"POST" 等）     |
| `path`        | `std::string`                        | `""`   | 路由路径（如 "/users/{id}"）     |
| `summary`     | `std::string`                        | `""`   | 接口简短描述                     |
| `description` | `std::string`                        | `""`   | 接口详细描述（可选）             |
| `tags`        | `std::vector<std::string>`           | `{}`   | 接口分组标签                     |
| `requestBody` | `std::optional<boost::json::object>` | 无     | 请求体 Schema（POST/PUT 时使用） |
| `responses`   | `std::map<int, boost::json::object>` | `{}`   | 响应 Schema，key 为 HTTP 状态码  |

#### OpenApiRegistry 类

| 方法        | 参数               | 返回值                      | 说明                                       |
| ----------- | ------------------ | --------------------------- | ------------------------------------------ |
| `add(info)` | info: RouteApiInfo | `void`                      | 注册一条路由的 API 元数据                  |
| `getAll()`  | 无                 | `std::vector<RouteApiInfo>` | 获取全部已注册信息（快照，mutex 短暂锁定） |
| `clear()`   | 无                 | `void`                      | 清空注册表                                 |
| `count()`   | 无                 | `size_t`                    | 当前注册条目数                             |

#### 宏

| 宏                                        | 说明                                                                |
| ----------------------------------------- | ------------------------------------------------------------------- |
| `HICAL_API(builder_exprs...)`             | 综合标注宏，接受 `builder::*` 函数调用作为参数，返回 `RouteApiInfo` |
| `HICAL_API_DEFAULT(method, path)`         | 最小标注宏，仅指定方法和路径，其余字段取默认值                      |
| `HICAL_ROUTES_WITH_API(HandlerType, ...)` | 增强版路由收集宏，同时注册路由和收集元数据（替代 `HICAL_ROUTES`）   |

#### builder 命名空间

`hical::builder::` 提供流式风格的辅助函数，用于构建 `RouteApiInfo`：

| 函数                            | 参数                               | 说明                                          |
| ------------------------------- | ---------------------------------- | --------------------------------------------- |
| `builder::summary(text)`        | text: 摘要字符串                   | 设置接口摘要                                  |
| `builder::description(text)`    | text: 详细描述字符串               | 设置接口详细描述                              |
| `builder::tag(name)`            | name: 标签名                       | 添加一个分组标签                              |
| `builder::tags(names...)`       | names: 多个标签名                  | 批量添加分组标签                              |
| `builder::requestBody<T>()`     | T: 请求体类型（需有 HICAL_JSON）   | 设置请求体 schema（自动调用 jsonSchema<T>()） |
| `builder::response<T>(code)`    | T: 响应体类型<br>code: HTTP 状态码 | 添加一个响应描述                              |
| `builder::response(code, desc)` | code: 状态码<br>desc: 描述字符串   | 添加无 schema 的响应描述                      |

#### registerRoutesWithOpenApi 函数

```cpp
template <typename HandlerType>
void registerRoutesWithOpenApi(
    Router& router,
    HandlerType& handler,
    OpenApiRegistry& registry);
```

同时完成两件事：调用 `registerRoutes(router, handler)` 注册路由，并将 `HandlerType::routeApiTable()` 中的元数据批量写入 `registry`。

---

### OpenApiDocument

OpenAPI 3.0 完整文档的惰性组装器，将 Registry 中的路由元数据与 Schema Map 合并为标准文档 JSON。

**头文件：** `src/core/OpenApiDocument.h`

#### OpenApiConfig 结构体

| 字段          | 类型                       | 默认值        | 说明                                             |
| ------------- | -------------------------- | ------------- | ------------------------------------------------ |
| `title`       | `std::string`              | `"Hical API"` | API 文档标题                                     |
| `version`     | `std::string`              | `"1.0.0"`     | API 版本号                                       |
| `description` | `std::string`              | `""`          | API 文档整体描述                                 |
| `servers`     | `std::vector<std::string>` | `{}`          | 服务器地址列表（如 `["http://localhost:8080"]`） |

#### OpenApiDocument 类

| 方法                                | 参数                                                | 返回值                       | 说明                                                 |
| ----------------------------------- | --------------------------------------------------- | ---------------------------- | ---------------------------------------------------- |
| `OpenApiDocument(config, registry)` | config: OpenApiConfig<br>registry: OpenApiRegistry& | —                            | 构造文档组装器                                       |
| `addSchema(name, schema)`           | name: schema 名称<br>schema: JSON Schema Object     | `void`                       | 手动添加一个 schema 到 `components/schemas`          |
| `addSchemas(schemaMap)`             | schemaMap: `map<string, json::object>`              | `void`                       | 批量添加 schema                                      |
| `generate()`                        | 无                                                  | `const boost::json::object&` | 惰性生成并缓存完整文档（首次调用后后续直接返回缓存） |
| `invalidate()`                      | 无                                                  | `void`                       | 使缓存失效，下次 `generate()` 重新生成               |

**文档生成规则：**
- 自动从路由路径 `{param}` 提取 path parameters
- 同一路径的不同 HTTP method 合并为同一 Path Item（符合 OpenAPI 规范）
- `components/schemas` 自动注入所有通过 `addSchema` 添加的类型

---

### OpenApiEndpoint

一行代码暴露 `/openapi.json` 和 `/docs` 端点。

**头文件：** `src/core/OpenApiEndpoint.h`

#### serveOpenApi 函数

```cpp
void serveOpenApi(
    Router& router,
    std::shared_ptr<OpenApiDocument> doc,
    std::string jsonPath  = "/openapi.json",
    std::string docsPath  = "/docs");
```

| 参数       | 说明                                       |
| ---------- | ------------------------------------------ |
| `router`   | 路由器引用，端点将注册到此路由器           |
| `doc`      | OpenApiDocument 智能指针                   |
| `jsonPath` | JSON spec 端点路径（默认 `/openapi.json`） |
| `docsPath` | Swagger UI HTML 页面路径（默认 `/docs`）   |

**安全说明：** Swagger UI HTML 中的 `jsonPath` 通过 `boost::json::serialize()` 转义，防止路径中包含特殊字符导致 JS 注入。

---

### 综合用法示例

```cpp
#include "core/HttpServer.h"
#include "core/MetaJson.h"
#include "core/MetaRoutes.h"
#include "core/OpenApiSchema.h"
#include "core/OpenApiRegistry.h"
#include "core/OpenApiDocument.h"
#include "core/OpenApiEndpoint.h"

using namespace hical;

// 1. 定义 DTO（附带 JSON 序列化描述）
HICAL_SCHEMA_NAME(UserDTO, "User")

struct UserDTO
{
    HICAL_JSON(UserDTO, REQUIRED(id), name, ALIAS(emailAddr, "email"))
    int id;
    std::string name;
    std::string emailAddr;
};

HICAL_SCHEMA_NAME(CreateUserRequest, "CreateUserRequest")

struct CreateUserRequest
{
    HICAL_JSON(CreateUserRequest, REQUIRED(name), REQUIRED(email))
    std::string name;
    std::string email;
};

// 2. 定义 Handler，使用 HICAL_API 标注
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
        auto body = req.readJson<CreateUserRequest>();
        co_return HttpResponse::json({{"name", body.name}});
    }

    // API 标注表（与 HICAL_ROUTES_WITH_API 配合）
    static std::vector<RouteApiInfo> routeApiTable()
    {
        return {
            HICAL_API(
                builder::summary("获取用户"),
                builder::tag("用户管理"),
                builder::response<UserDTO>(200),
                builder::response(404, "用户不存在")
            ),
            HICAL_API(
                builder::summary("创建用户"),
                builder::tag("用户管理"),
                builder::requestBody<CreateUserRequest>(),
                builder::response<UserDTO>(201)
            ),
        };
    }

    HICAL_ROUTES_WITH_API(UserHandler, getUser, createUser)
};

int main()
{
    HttpServer server(8080);
    auto& router = server.router();

    // 3. 创建 Registry 和 Document
    auto registry = std::make_shared<OpenApiRegistry>();
    OpenApiConfig config;
    config.title   = "用户服务 API";
    config.version = "1.0.0";
    config.servers = {"http://localhost:8080"};
    auto doc = std::make_shared<OpenApiDocument>(config, *registry);

    // 4. 同时注册路由和收集元数据
    UserHandler handler;
    registerRoutesWithOpenApi(router, handler, *registry);

    // 5. 收集 schema 并注入文档
    std::map<std::string, boost::json::object> schemas;
    collectSchemas<UserDTO>(schemas);
    collectSchemas<CreateUserRequest>(schemas);
    doc->addSchemas(schemas);

    // 6. 一行暴露 /openapi.json 和 /docs
    serveOpenApi(router, doc);

    server.start();
    return 0;
}
```

**访问端点：**

```bash
# 获取 OpenAPI JSON 规范
curl http://localhost:8080/openapi.json

# 浏览器访问 Swagger UI
# http://localhost:8080/docs
```

---

> 更多信息请参阅：[快速上手](quickstart.md) | [使用示例](examples_guide.md) | [架构设计](architecture.md) | [性能报告](performance_report.md)
