# Hical 协程入门：告别回调地狱，用 co_await 写异步 C++

> 传统 C++ 异步编程离不开回调嵌套、状态机、手动生命周期管理——代码写得像意大利面。C++20 协程从根本上改变了这一切：**异步代码写起来和同步一样直观，编译器帮你管理暂停与恢复**。本文从零讲解如何在 Hical 框架中使用协程，不需要你懂 Boost.Asio 底层。

---

## 什么是协程？30 秒版本

传统回调式：

```cpp
// 回调嵌套——"回调地狱"
socket.async_read(buffer, [&](error_code ec, size_t n) {
    if (!ec) {
        socket.async_write(buffer, [&](error_code ec2, size_t) {
            if (!ec2) {
                socket.async_read(buffer, [&](error_code ec3, size_t) {
                    // 继续嵌套...
                });
            }
        });
    }
});
```

协程式：

```cpp
// 同样的逻辑，协程版——像写同步代码一样
auto n = co_await socket.async_read(buffer, use_awaitable);
co_await socket.async_write(buffer, use_awaitable);
auto n2 = co_await socket.async_read(buffer, use_awaitable);
```

**`co_await` 会暂停当前函数，等 I/O 完成后自动恢复执行**。没有回调，没有嵌套，错误用 try/catch 处理。

---

## Hical 对协程做了什么封装？

Hical 在 `Coroutine.h` 中提供了三个核心工具：

### 1. `Awaitable<T>` — 协程返回类型

```cpp
template <typename T = void>
using Awaitable = boost::asio::awaitable<T>;
```

所有 Hical 协程函数的返回类型。`T` 是协程的返回值类型，不需要返回值时用 `Awaitable<void>`（默认）。

### 2. `sleep()` — 协程定时器

```cpp
co_await hical::sleep(1.0);                           // 等待 1 秒
co_await hical::sleep(std::chrono::milliseconds(500)); // 等待 500ms
```

在协程内暂停指定时间，**不阻塞线程**。比 `std::this_thread::sleep_for` 好——线程空闲期间可以处理其他连接。

底层走的是 `TimerService`：每条 io_context 只有一个 `steady_timer` 配一个 deadline 小顶堆，几万个协程同时睡也只占一个等待操作，不是一协程一个 timer。省的是注册/销毁那套簿记；Windows 上 Asio timer 本来就在一个队列里排，「几万个内核定时器变成一个」这说法只在 Linux（一个 timerfd）成立。没绑 `TimerService` 的地方（客户端、单测里自己 new 的 io_context）会自动退回各自独立的 `steady_timer`，用起来没区别。

### 3. `coSpawn()` — 启动协程

```cpp
hical::coSpawn(ioCtx, myCoroutine());
```

在 `io_context` 上启动一个协程。未捕获的异常会输出到 stderr（而非被 Boost 默认的 `detached` 静默吞掉）。

---

## 实战：四种场景

### 场景一：同步路由处理器（不需要协程）

大多数路由逻辑不涉及异步操作（数据库、网络调用），直接用同步写法：

```cpp
#include "core/HttpServer.h"
using namespace hical;

int main()
{
    HttpServer server(8080);

    // 同步处理器：直接返回 HttpResponse
    server.router().get("/api/status", [](const HttpRequest&) -> HttpResponse {
        return HttpResponse::json({{"status", "running"}});
    });

    // 路径参数
    server.router().get("/users/{id}", [](const HttpRequest& req) -> HttpResponse {
        auto id = req.param("id");
        return HttpResponse::json({{"userId", id}, {"name", "User " + id}});
    });

    server.start();
}
```

**关键点**：返回类型是 `HttpResponse`，不是 `Awaitable<HttpResponse>`。Hical 内部会自动把同步处理器包装成协程，你不需要关心。

### 场景二：异步路由处理器（需要协程）

当路由内部要做异步操作（数据库查询、调用外部 API、定时等待）时，用协程版：

```cpp
// 协程处理器：返回 Awaitable<HttpResponse>，可以用 co_await
server.router().get("/api/delayed",
    [](const HttpRequest&) -> Awaitable<HttpResponse>
    {
        co_await hical::sleep(1.0);  // 异步等待 1 秒，不阻塞线程
        co_return HttpResponse::ok("waited 1 second");
    });

// 数据库查询（需要 HICAL_WITH_DATABASE=ON）
server.router().get("/api/users",
    [](const HttpRequest& req) -> Awaitable<HttpResponse>
    {
        auto conn = db::getDbConnection(req);
        auto result = co_await conn->query("SELECT id, name FROM users");
        // 处理结果...
        co_return HttpResponse::json(usersJson);
    });
```

**关键点**：
- 返回类型改为 `Awaitable<HttpResponse>`
- 用 `co_return` 代替 `return`
- 内部可以 `co_await` 任何异步操作

### 场景三：异步中间件

中间件采用洋葱模型——请求进入时从外到内，响应返回时从内到外：

```cpp
// 日志中间件：记录请求路径和响应状态码
server.use(
    [](HttpRequest& req, MiddlewareNext next) -> Awaitable<HttpResponse>
    {
        // ① 前置逻辑（请求进入）
        auto start = std::chrono::steady_clock::now();
        std::cout << req.method() << " " << req.path() << std::endl;

        // ② 调用下一层（中间件或路由处理器）
        auto res = co_await next(req);

        // ③ 后置逻辑（响应返回）
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();
        std::cout << "  -> " << res.statusCode() << " (" << ms << "ms)" << std::endl;

        co_return res;
    });
```

**洋葱模型图解**：

```
请求 →  [中间件A 前置]  →  [中间件B 前置]  →  [路由处理器]
响应 ←  [中间件A 后置]  ←  [中间件B 后置]  ←
```

`co_await next(req)` 是关键——它把控制权交给下一层，等下一层处理完再继续执行后置逻辑。

### 场景四：WebSocket（总是协程）

```cpp
server.router().ws("/ws/echo",
    // 收到消息时
    [](const std::string& msg, WebSocketSession& ws) -> Awaitable<void>
    {
        co_await ws.send("Echo: " + msg);
    },
    // 连接建立时
    [](WebSocketSession& ws) -> Awaitable<void>
    {
        co_await ws.send("Connected!");
    },
    // 连接断开时（可选）
    [](WebSocketSession& ws) -> Awaitable<void>
    {
        std::cout << "Client disconnected" << std::endl;
        co_return;
    });
```

---

## 完整示例：一个带中间件的 REST 服务

```cpp
#include "core/HttpServer.h"
#include "core/WebSocket.h"
#include <iostream>

using namespace hical;

int main()
{
    HttpServer server(8080);

    // 日志中间件
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

    // 同步路由
    server.router().get("/", [](const HttpRequest&) -> HttpResponse {
        return HttpResponse::ok("Welcome to hical!");
    });

    server.router().get("/api/status", [](const HttpRequest&) -> HttpResponse {
        return HttpResponse::json(
            {{"status", "running"}, {"version", "2.5.0"}});
    });

    server.router().post("/api/echo", [](const HttpRequest& req) -> HttpResponse {
        return HttpResponse::ok(req.body());
    });

    // 路径参数
    server.router().get("/users/{id}", [](const HttpRequest& req) -> HttpResponse {
        return HttpResponse::json(
            {{"userId", req.param("id")}, {"name", "User " + req.param("id")}});
    });

    // WebSocket
    server.router().ws("/ws/echo",
        [](const std::string& msg, WebSocketSession& ws) -> Awaitable<void> {
            co_await ws.send("Echo: " + msg);
        },
        [](WebSocketSession& ws) -> Awaitable<void> {
            co_await ws.send("Connected to hical WebSocket!");
        });

    std::cout << "Server listening on port 8080" << std::endl;
    server.start();
}
```

---

## 什么时候用同步，什么时候用协程？

| 场景              | 写法         | 返回类型                  |
| ----------------- | ------------ | ------------------------- |
| 纯计算/JSON 组装  | 同步         | `HttpResponse`            |
| 数据库查询        | 协程         | `Awaitable<HttpResponse>` |
| 调用外部 HTTP API | 协程         | `Awaitable<HttpResponse>` |
| 需要延时/定时     | 协程         | `Awaitable<HttpResponse>` |
| 异步中间件        | 协程         | `Awaitable<HttpResponse>` |
| 同步前置/后置中间件 | 同步       | `SyncBeforeHandler` / `SyncAfterHandler` |
| WebSocket 回调    | **总是协程** | `Awaitable<void>`         |

**经验法则**：如果函数体里没有 `co_await`，用同步。需要 `co_await` 任何东西，就用协程。

---

## 协程 vs 回调 vs 线程：为什么选协程？

| 方案   | 可读性       | 性能             | 每连接开销      |
| ------ | ------------ | ---------------- | --------------- |
| 回调   | 差（嵌套深） | 高               | 低              |
| 线程池 | 好           | 中（上下文切换） | 高（~1MB 栈）   |
| 协程   | **好**       | **高**           | **低（~几KB）** |

Hical 使用 Boost.Asio 协程，底层仍然是 epoll/IOCP 事件驱动，协程只是语法糖——**性能和手写回调一样，可读性和同步代码一样**。

---

## 常见问题

### Q：我必须懂 Boost.Asio 才能用 Hical 吗？

不需要。Hical 已经封装了所有底层细节。你只需要知道：
- 路由处理器返回 `HttpResponse`（同步）或 `Awaitable<HttpResponse>`（协程）
- 中间件和 WebSocket 回调用 `co_await` + `co_return`
- `hical::sleep()` 做异步等待

### Q：协程里能用锁吗？

能，但要小心。`co_await` 暂停期间不持有锁就没问题。不要在持有 `std::mutex` 的情况下 `co_await`——恢复时可能在不同线程上，导致 UB。

### Q：同步处理器和协程处理器能混用吗？

可以。同一个 Router 里可以同时注册同步和协程路由，Hical 内部统一处理。

### Q：性能有差异吗？

同步处理器可走 `dispatchSync()` 快速路径，省掉处理器的协程帧；需要异步等待时使用协程。实际差异取决于工作量，不能把两种写法笼统地说成性能相同。

## HTTP/2 的请求与取消生命周期

当前 HTTP/2 为可选候选实现，仍待最终验收；它复用同样的同步/异步路由和中间件。`useBody()` 在第一个需要 body 的中间件前设置读取门，门前只看到头，门后才看到完整 body 与尾字段。H2 读泵提前收到的 DATA 受有限预算约束，不会改变应用可见顺序。

对端 RST_STREAM、超时或断连会停止流输出，却不会强制销毁正在运行的 handler。跨 `co_await` 的对象仍需有效所有权，请求视图和请求级 PMR 对象仍不能逃逸。handler 的自然收尾期间继续占应用任务配额；数据库中间件按原有成功或异常路径处理事务，RST 本身不等于回滚。服务端用 `gracefulStop()` 排空，保持对象存活到 `start()` 返回。

可选 `HttpClient` 的普通 `request/get/post` 入口在返回 awaitable 前持有参数和内部状态，所以保存 awaitable 后可以释放原 target/body/header；请求绝对期限从提交开始，稍后启动不会重新计时。facade 析构会取消旧请求，已保存的 awaitable 仍能安全启动并返回取消结果；`io_context` 则必须覆盖所有未完成请求、读写泵与投递的清理。

同一个客户端在 H1 下串行，在 H2 下按流配额并发。通过 Asio cancellation slot 取消一个 H2 请求只影响该流；还在排队、没有 stream ID 的请求直接退出队列。`close()` 取消整条连接当前代次，后续请求可以重新建连。连接池的 `shutdown()` 清理也需 owner loop 继续运行，不能调用后立即销毁 io_context。构建、TLS 配置与完整可运行示例见 [HTTP/2 使用指南](http2-guide.md)。

原生取消和超时不会触发池高层的自动重试。允许的幂等方法至多重试一次，继续使用原请求期限与首次实际排队的截止时间；POST/PATCH 默认不重放，RST 或 GOAWAY 也不能替应用决定业务重试。具体证据与白名单见 [连接池与重试边界](http2-guide.md#连接池与重试边界)。
