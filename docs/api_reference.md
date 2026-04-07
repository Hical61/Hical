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
- [WebSocketSession](#websocketsession) — WebSocket 会话

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
    server.use([](const HttpRequest& req, MiddlewareNext next)
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

路由管理器，负责路由注册和请求分发。静态路由使用哈希表 O(1) 查找，参数路由线性匹配。

**头文件：** `src/core/Router.h`

#### 公共方法

| 方法                             | 参数                                                                 | 返回值                    | 说明                           |
| -------------------------------- | -------------------------------------------------------------------- | ------------------------- | ------------------------------ |
| `route(method, path, handler)`   | method: HTTP 方法<br>path: 路由路径<br>handler: 协程处理器           | `void`                    | 注册协程路由                   |
| `route(method, path, handler)`   | method: HTTP 方法<br>path: 路由路径<br>handler: 同步处理器           | `void`                    | 注册同步路由（自动包装为协程） |
| `get(path, handler)`             | path: 路由路径<br>handler: 处理器                                    | `void`                    | 注册 GET 路由                  |
| `post(path, handler)`            | path: 路由路径<br>handler: 处理器                                    | `void`                    | 注册 POST 路由                 |
| `put(path, handler)`             | path: 路由路径<br>handler: 处理器                                    | `void`                    | 注册 PUT 路由                  |
| `del(path, handler)`             | path: 路由路径<br>handler: 处理器                                    | `void`                    | 注册 DELETE 路由               |
| `ws(path, onMessage, onConnect)` | path: 路由路径<br>onMessage: 消息回调<br>onConnect: 连接回调（可选） | `void`                    | 注册 WebSocket 路由            |
| `dispatch(req)`                  | req: HTTP 请求                                                       | `Awaitable<HttpResponse>` | 分发请求到匹配的路由           |
| `routeCount()`                   | 无                                                                   | `size_t`                  | 获取已注册路由数量             |

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

// 使用宏
HICAL_ROUTE(router, Get, "/hello", myHandler);
```

---

### HttpRequest

HTTP 请求封装，对 Boost.Beast `http::request` 的 hical 风格封装。

**头文件：** `src/core/HttpRequest.h`

#### 公共方法

| 方法             | 参数             | 返回值               | 说明                           |
| ---------------- | ---------------- | -------------------- | ------------------------------ |
| `method()`       | 无               | `HttpMethod`         | 获取 HTTP 方法                 |
| `path()`         | 无               | `std::string`        | 获取请求路径（不含查询参数）   |
| `target()`       | 无               | `std::string`        | 获取完整 URI（含查询参数）     |
| `query()`        | 无               | `std::string`        | 获取查询字符串（? 后面的部分） |
| `header(name)`   | name: 头部字段名 | `std::string`        | 获取指定头部字段值             |
| `body()`         | 无               | `const std::string&` | 获取消息体                     |
| `jsonBody()`     | 无               | `boost::json::value` | 将消息体解析为 JSON            |
| `contentType()`  | 无               | `std::string`        | 获取 Content-Type 头           |
| `param(name)`    | name: 参数名     | `std::string`        | 获取路径参数                   |
| `hasParam(name)` | name: 参数名     | `bool`               | 是否有指定路径参数             |
| `native()`       | 无               | `BeastRequest&`      | 获取底层 Beast 请求引用        |

#### 构建请求的方法

| 方法                     | 参数                          | 返回值 | 说明                               |
| ------------------------ | ----------------------------- | ------ | ---------------------------------- |
| `setMethod(method)`      | method: HTTP 方法             | `void` | 设置 HTTP 方法                     |
| `setTarget(target)`      | target: 目标 URI              | `void` | 设置请求路径                       |
| `setHeader(name, value)` | name: 字段名<br>value: 字段值 | `void` | 设置头部字段                       |
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

HTTP 响应封装，对 Boost.Beast `http::response` 的 hical 风格封装。

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
| `native()`                   | 无                                                           | `BeastResponse&`     | 获取底层 Beast 响应引用 |

#### 工厂方法

| 方法                  | 参数                                    | 返回值         | 说明                                |
| --------------------- | --------------------------------------- | -------------- | ----------------------------------- |
| `ok(body)`            | body: 消息体（默认空）                  | `HttpResponse` | 创建 200 OK 响应                    |
| `json(json)`          | json: JSON 值                           | `HttpResponse` | 创建 JSON 200 OK 响应               |
| `notFound()`          | 无                                      | `HttpResponse` | 创建 404 Not Found 响应             |
| `badRequest(message)` | message: 错误信息（默认 "Bad Request"） | `HttpResponse` | 创建 400 Bad Request 响应           |
| `serverError()`       | 无                                      | `HttpResponse` | 创建 500 Internal Server Error 响应 |

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
using MiddlewareNext = std::function<Awaitable<HttpResponse>(const HttpRequest&)>;

// 中间件处理器类型
using MiddlewareHandler = std::function<Awaitable<HttpResponse>(const HttpRequest&, MiddlewareNext)>;
```

#### MiddlewarePipeline 类

| 方法                         | 参数                                       | 返回值                    | 说明           |
| ---------------------------- | ------------------------------------------ | ------------------------- | -------------- |
| `use(middleware)`            | middleware: 中间件处理器                   | `void`                    | 添加中间件     |
| `execute(req, finalHandler)` | req: HTTP 请求<br>finalHandler: 最终处理器 | `Awaitable<HttpResponse>` | 执行中间件管道 |
| `size()`                     | 无                                         | `size_t`                  | 获取中间件数量 |

#### 示例

```cpp
#include "core/Middleware.h"

using namespace hical;

// 日志中间件
auto logger = [](const HttpRequest& req, MiddlewareNext next)
                  -> Awaitable<HttpResponse> {
    std::cout << "[" << httpMethodToString(req.method()) << "] " 
              << req.path() << std::endl;
    
    auto res = co_await next(req);  // 调用下一层
    
    std::cout << "  -> " << static_cast<int>(res.statusCode()) << std::endl;
    co_return res;
};

// 认证中间件（拦截未授权请求）
auto auth = [](const HttpRequest& req, MiddlewareNext next)
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
auto cors = [](const HttpRequest& req, MiddlewareNext next)
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

WebSocket 会话封装，对 Boost.Beast `websocket::stream` 的 hical 风格封装。

**头文件：** `src/core/WebSocket.h`

#### 公共方法

| 方法        | 参数          | 返回值                   | 说明                      |
| ----------- | ------------- | ------------------------ | ------------------------- |
| `send(msg)` | msg: 消息内容 | `Awaitable<void>`        | 发送文本消息              |
| `receive()` | 无            | `Awaitable<std::string>` | 接收消息                  |
| `close()`   | 无            | `void`                   | 关闭连接                  |
| `isOpen()`  | 无            | `bool`                   | 连接是否打开              |
| `native()`  | 无            | `WsStream&`              | 获取底层 WebSocket 流引用 |

#### 回调类型

```cpp
// 消息回调
using WsMessageCallback = std::function<Awaitable<void>(const std::string&, WebSocketSession&)>;

// 连接建立回调
using WsConnectCallback = std::function<Awaitable<void>(WebSocketSession&)>;
```

#### 示例

```cpp
#include "core/WebSocket.h"

using namespace hical;

// 注册 WebSocket 路由
server.router().ws("/ws/chat",
    // 消息回调
    [](const std::string& msg, WebSocketSession& ws) -> Awaitable<void> {
        std::cout << "收到消息: " << msg << std::endl;
        co_await ws.send("Echo: " + msg);
    },
    // 连接建立回调（可选）
    [](WebSocketSession& ws) -> Awaitable<void> {
        std::cout << "新连接建立" << std::endl;
        co_await ws.send("欢迎!");
    }
);

// 手动使用 WebSocketSession
Awaitable<void> chatHandler(WebSocketSession& ws)
{
    co_await ws.send("连接成功");
    
    while (ws.isOpen()) {
        try {
            auto msg = co_await ws.receive();
            co_await ws.send("收到: " + msg);
        } catch (...) {
            break;
        }
    }
    
    ws.close();
}
```

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

| 文件                  | 说明                       |
| --------------------- | -------------------------- |
| `AsioEventLoop.h`     | EventLoop 的 Asio 实现     |
| `AsioTimer.h`         | 定时器的 Asio 实现         |
| `AsioTcpConnection.h` | TcpConnection 类型别名     |
| `GenericConnection.h` | 模板化连接（支持 TCP/SSL） |
| `TcpServer.h`         | TCP 服务器                 |
| `EventLoopPool.h`     | 事件循环线程池             |

通常用户不需要直接使用适配层，`HttpServer` 已封装了全部网络操作。

---

## 附录

### 类型别名速查

| 别名                | 定义                                                                    | 头文件         |
| ------------------- | ----------------------------------------------------------------------- | -------------- |
| `Awaitable<T>`      | `boost::asio::awaitable<T>`                                             | `Coroutine.h`  |
| `RouteHandler`      | `function<Awaitable<HttpResponse>(const HttpRequest&)>`                 | `Router.h`     |
| `SyncRouteHandler`  | `function<HttpResponse(const HttpRequest&)>`                            | `Router.h`     |
| `MiddlewareNext`    | `function<Awaitable<HttpResponse>(const HttpRequest&)>`                 | `Middleware.h` |
| `MiddlewareHandler` | `function<Awaitable<HttpResponse>(const HttpRequest&, MiddlewareNext)>` | `Middleware.h` |
| `WsMessageCallback` | `function<Awaitable<void>(const string&, WebSocketSession&)>`           | `Router.h`     |
| `WsConnectCallback` | `function<Awaitable<void>(WebSocketSession&)>`                          | `Router.h`     |
| `Func`              | `function<void()>`                                                      | `EventLoop.h`  |
| `TimerId`           | `uint64_t`                                                              | `EventLoop.h`  |

### 回调类型速查

| 场景     | 类型签名                                                      | 说明               |
| -------- | ------------------------------------------------------------- | ------------------ |
| 同步路由 | `HttpResponse(const HttpRequest&)`                            | 直接返回响应       |
| 协程路由 | `Awaitable<HttpResponse>(const HttpRequest&)`                 | 协程返回响应       |
| 中间件   | `Awaitable<HttpResponse>(const HttpRequest&, MiddlewareNext)` | 洋葱模型           |
| WS 消息  | `Awaitable<void>(const string&, WebSocketSession&)`           | WebSocket 消息回调 |
| WS 连接  | `Awaitable<void>(WebSocketSession&)`                          | WebSocket 连接回调 |
| 定时器   | `void()`                                                      | 无参回调           |

---

> 更多信息请参阅：[快速上手](quickstart.md) | [使用示例](examples_guide.md) | [架构设计](architecture.md) | [性能报告](performance_report.md)
