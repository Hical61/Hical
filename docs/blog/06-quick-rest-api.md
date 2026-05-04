# C++ 也能优雅写 Web？5 分钟用 Hical 搭建 REST API

> 提到 C++ 写 Web 服务，你脑海中浮现的可能是满屏的模板报错、手动解析 HTTP 报文、以及回调嵌套到看不清缩进的代码。但在 2026 年，C++20 协程 + PMR 内存池 + C++26 反射的组合，已经让 C++ Web 开发体验发生了质变。本文用 Hical 框架带你体验：**10 行代码启动 HTTP 服务器，40 行代码搞定完整 REST API**。

---

## 10 行代码，启动 HTTP 服务器

```cpp
#include "core/HttpServer.h"
using namespace hical;

int main()
{
    HttpServer server(8080);

    server.router().get("/", [](const HttpRequest&) -> HttpResponse {
        return HttpResponse::ok("Hello, hical!");
    });

    server.start();
}
```

```bash
curl http://localhost:8080/
# Hello, hical!
```

没有工厂类，没有 Builder 链，没有 XML 配置。创建服务器、注册路由、启动 —— 三步完事。

---

## 40 行代码，完整 REST API

实际项目当然不止一个路由。下面是一个包含 JSON 响应、路径参数、请求体读取、日志中间件的完整示例：

```cpp
#include "core/HttpServer.h"
using namespace hical;

int main()
{
    HttpServer server(8080);

    // 日志中间件 —— 洋葱模型，请求前后各打一行日志
    server.use(
        [](HttpRequest& req, MiddlewareNext next) -> Awaitable<HttpResponse>
        {
            std::cout << httpMethodToString(req.method()) << " "
                      << req.path() << std::endl;
            auto res = co_await next(req);
            std::cout << "  -> " << static_cast<int>(res.statusCode())
                      << std::endl;
            co_return res;
        });

    // GET /api/status —— JSON 响应
    server.router().get("/api/status",
        [](const HttpRequest&) -> HttpResponse {
            return HttpResponse::json(
                {{"status", "running"}, {"version", "x.x.x"},
                 {"framework", "hical"}});
        });

    // POST /api/echo —— 读取请求体并回写
    server.router().post("/api/echo",
        [](const HttpRequest& req) -> HttpResponse {
            return HttpResponse::ok(req.body());
        });

    // GET /users/{id} —— 路径参数自动提取
    server.router().get("/users/{id}",
        [](const HttpRequest& req) -> HttpResponse {
            return HttpResponse::json(
                {{"userId", req.param("id")},
                 {"name", "User " + req.param("id")}});
        });

    server.start();
}
```

测试一下：

```bash
# JSON 响应
curl http://localhost:8080/api/status
# {"status":"running","version":"x.x.x","framework":"hical"}

# 路径参数
curl http://localhost:8080/users/42
# {"userId":"42","name":"User 42"}

# POST 回写
curl -X POST -d "Hello" http://localhost:8080/api/echo
# Hello
```

这 40 行代码覆盖了 REST API 最常见的场景：JSON 返回、路径参数、请求体处理、请求日志。如果你用过 Express.js 或 Flask，会发现 API 风格非常相似 —— 只是语言换成了 C++。

### 路由分组 + 查询参数

```cpp
// v2.4.0: 路由分组 —— 共享前缀和中间件
auto api = server.router().group("/api/v2");
api.use(authMiddleware);  // 组级中间件，仅对 /api/v2/* 生效

api.get("/users", [](const HttpRequest& req) -> HttpResponse {
    auto page = req.queryParam("page").value_or("1");
    return HttpResponse::json({{"page", page}, {"users", "..."}});
});

api.get("/users/{id}", [](const HttpRequest& req) -> HttpResponse {
    return HttpResponse::json({{"id", req.param("id")}});
});
```

### 日志中间件

```cpp
// 内置日志中间件 —— 一行代码替代手写日志
#include "core/LogMiddleware.h"
server.use(makeLogMiddleware());
// 自动生成 trace-id，自动记录 method/path/status/latency_ms
```

### OpenAPI 一键集成

```cpp
// OpenAPI 文档 —— 访问 /docs 即得 Swagger UI
#include "core/OpenApiEndpoint.h"
auto registry = std::make_shared<OpenApiRegistry>();
// ... 注册路由和标注 ...
auto doc = std::make_shared<OpenApiDocument>(registry, OpenApiConfig{.title = "My API"});
serveOpenApi(server.router(), doc);
```

---

## 为什么选 Hical？

### 协程异步，告别回调地狱

注意上面中间件里的 `co_await next(req)`。这不是回调，不是 Promise，而是 C++20 原生协程。异步代码写起来跟同步一样顺畅：

```cpp
server.router().get("/async", [](const HttpRequest&) -> Awaitable<HttpResponse> {
    co_await hical::sleep(0.1);  // 异步等待，不阻塞线程
    co_return HttpResponse::ok("done");
});
```

### PMR 内存池，性能开箱即用

Hical 内置三层 PMR（Polymorphic Memory Resource）内存池：全局同步池、线程本地无锁池、请求级单调缓冲区。HTTP 请求处理中的缓冲区、JSON 对象、响应体全部走 PMR 分配，请求结束时整体释放 —— 零碎片、零锁争用。你不需要做任何配置，默认就启用了。

### C++26 反射就绪，一行代码搞定序列化

定义一个 DTO 结构体，加一行 `HICAL_JSON` 宏，就能自动序列化/反序列化 JSON：

```cpp
struct UserDTO
{
    std::string name;
    int age;
    std::string email;

    HICAL_JSON(UserDTO, name, age, email)  // 就这一行
};

// 序列化
UserDTO user{"Alice", 30, "alice@example.com"};
auto json = meta::toJson(user);  // -> {"name":"Alice","age":30,"email":"alice@example.com"}

// 反序列化（从 HTTP 请求体）
auto user = req.readJson<UserDTO>();
```

当编译器支持 C++26 反射时，连这行宏都不需要 —— Hical 会自动切换到原生反射路径。

### 数据库协程化，告别手动连接管理

```cpp
// v2.3.0: 协程化 DB 中间件 —— 连接自动获取/释放，事务自动提交/回滚
auto pool = std::make_shared<DbConnectionPool>(MysqlConnection::makeFactory(dbConfig), dbConfig);
server.use(makeDbMiddleware(pool, {.autoTransaction = true}));

server.router().get("/users/{id}", [](const HttpRequest& req) -> Awaitable<HttpResponse> {
    auto conn = getDbConnection(req);
    auto result = co_await conn->query("SELECT * FROM users WHERE id = ?", {req.param("id")});
    co_return HttpResponse::json({{"name", result.rows[0][1]}});
});
```

### OpenAPI 自动生成，告别手写文档

一行 `serveOpenApi()` 即可在 `/docs` 暴露完整的 Swagger UI，在 `/openapi.json` 暴露机器可读的规范文件。路径参数自动提取、同路径多方法自动合并，无需手写 YAML/JSON。

---

## 安装

**vcpkg（推荐）：**

```bash
vcpkg install hical61-hical
```

```cmake
find_package(hical CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE hical::hical_core)
```

**源码构建：**

```bash
git clone https://github.com/Hical61/Hical.git
cd Hical
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

**CMake FetchContent：**

```cmake
include(FetchContent)
FetchContent_Declare(hical
    GIT_REPOSITORY https://github.com/Hical61/Hical.git
    GIT_TAG        VERSION)
FetchContent_MakeAvailable(hical)
target_link_libraries(my_app PRIVATE hical_core)
```

---

## 想深入了解？

本文只展示了 Hical 最基础的用法。框架还支持 WebSocket 双向通信、SSL/TLS 加密、Cookie/Session 管理、静态文件服务、Multipart 文件上传等完整功能。

深度教学系列（从架构设计到性能调优）：
- [第一篇：设计理念与架构总览](01-design-philosophy.md)
- [第二篇：协程异步与 PMR 内存池](02-coroutine-and-memory.md)
- [第三篇：路由、中间件与 SSL](03-router-middleware-ssl.md)
- [第四篇：实战案例与性能调优](04-practice-and-performance.md)
- [第五篇：Cookie、Session 与文件服务](05-cookies-sessions-fileservices.md)
- [第六篇：快速上手 REST API](06-quick-rest-api.md)（本文）
- [第七篇：框架横评对比](07-framework-comparison.md)
- [第八篇：Hical + MySQL CRUD 实战](08-hical-mysql-crud.md)
- [第九篇：日志系统完全指南](09-hical-logging-guide.md)
- [第十篇：OpenAPI 自动文档生成](10-hical-openapi-swagger.md)

GitHub: [https://github.com/Hical61/Hical](https://github.com/Hical61/Hical)

如果觉得 Hical 有意思，欢迎给个 Star，这是对开源项目最好的支持。
