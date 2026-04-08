# hical

[![CI](https://github.com/Hical61/Hical/actions/workflows/ci.yml/badge.svg)](https://github.com/Hical61/Hical/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)](https://isocpp.org/)
[![Platform](https://img.shields.io/badge/Platform-Linux%20%7C%20Windows%20%7C%20macOS-green.svg)]()
[![Boost](https://img.shields.io/badge/Boost-%E2%89%A51.70-orange.svg)](https://www.boost.org/)

Hical 是一个基于 Boost.Asio/Beast，利用 C++26 反射和 PMR 内存池构建高性能的现代 C++ Web 框架。

> **状态：** 开发中

[English](README.md) | 简体中文

## 特性

- **C++26 反射** — 面向 C++26 反射特性设计，支持自动路由注册、序列化等编译期元编程能力
- **Boost.Asio/Beast 后端** — 工业级网络库，每线程独立 `io_context` 模型
- **PMR 内存池** — 统一 `std::pmr` 分配器策略，缓冲区、HTTP 消息体、JSON 对象共享内存池，减少碎片，提升缓存命中率
- **协程支持** — `asio::awaitable<T>` + `co_spawn`，简洁的异步代码风格
- **C++ Concepts** — 编译期 `NetworkBackend` 类型约束，保障类型安全
- **SSL/TLS** — 模板化 `GenericConnection<SocketType>`，同时支持明文和加密连接
- **WebSocket** — WebSocket 升级与双向通信
- **路由 & 中间件** — 中间件管线（日志、认证、限流），支持路径参数
- **HTTP 服务器** — 基于 Boost.Beast 的完整 HTTP/1.1 支持（分块传输、Keep-Alive）

## 为什么选择 Hical？

| | Hical | Drogon | Crow |
|---|---|---|---|
| **C++ 标准** | C++20（C++26 就绪） | C++17 | C++11 |
| **异步模型** | 协程 (`co_await`) | 回调 + 协程 | 回调 |
| **内存策略** | 三层 PMR 内存池 | 默认分配器 | 默认分配器 |
| **HTTP 解析** | Boost.Beast | 自研 (Trantor) | 自研 |
| **SSL** | 编译期模板分支 | 运行时分支 | 运行时分支 |
| **后端抽象** | C++20 Concepts | 无 | 无 |

## 快速开始

```cpp
#include "core/HttpServer.h"
#include "core/WebSocket.h"

using namespace hical;

int main()
{
    HttpServer server(8080);

    // 中间件 — 日志
    server.use([](const HttpRequest& req, MiddlewareNext next)
                   -> Awaitable<HttpResponse> {
        std::cout << httpMethodToString(req.method()) << " "
                  << req.path() << std::endl;
        co_return co_await next(req);
    });

    // GET / — JSON 响应
    server.router().get("/", [](const HttpRequest&) -> HttpResponse {
        return HttpResponse::json({
            {"status", "running"},
            {"framework", "hical"}
        });
    });

    // GET /users/{id} — 路径参数
    server.router().get("/users/{id}",
        [](const HttpRequest& req) -> HttpResponse {
            return HttpResponse::json({{"userId", req.param("id")}});
        });

    // WebSocket Echo
    server.router().ws("/ws/echo",
        [](const std::string& msg, WebSocketSession& ws)
            -> Awaitable<void> {
            co_await ws.send("Echo: " + msg);
        });

    server.start();
}
```

```bash
curl http://localhost:8080/
# {"status":"running","framework":"hical"}
```

> 完整教程请参考 [docs/quickstart.md](docs/quickstart.md)，更多示例见 [examples/](examples/)。

## 项目结构

```
hical/
├── src/
│   ├── core/          # 抽象接口与共享类型
│   │   ├── EventLoop.h, Timer.h, TcpConnection.h
│   │   ├── MemoryPool.h/cpp, PmrBuffer.h
│   │   ├── Error.h/cpp, Concepts.h, Coroutine.h
│   │   ├── HttpServer.h/cpp, HttpRequest.h/cpp, HttpResponse.h/cpp
│   │   ├── Router.h/cpp, Middleware.h/cpp
│   │   ├── WebSocket.h/cpp, SslContext.h/cpp
│   │   └── HttpTypes.h, InetAddress.h/cpp
│   └── asio/          # Boost.Asio 实现层
│       ├── AsioEventLoop.h/cpp
│       ├── AsioTimer.h/cpp
│       ├── GenericConnection.h/cpp
│       ├── EventLoopPool.h/cpp
│       └── TcpServer.h/cpp
├── tests/             # 单元测试（Google Test）
├── examples/          # HTTP 服务器、WebSocket、基准测试、PMR 演示
├── docs/              # 设计分析文档
└── CMakeLists.txt
```

## 依赖

| 依赖项      | 版本要求                             |
| ----------- | ------------------------------------ |
| C++ 标准    | C++20                                |
| Boost       | >= 1.70（Asio、Beast、System、JSON） |
| CMake       | >= 3.20                              |
| OpenSSL     | 必需                                 |
| Google Test | 必需                                 |
| 编译器      | GCC 14+ / Clang 18+ / MSVC 2022+     |

## 构建

### Linux / macOS

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
```

### Windows（MSYS2 MINGW64）

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

## 性能

Hical 内置三层 PMR 内存池架构：

- **线程本地池** — 无锁分配，线程间零竞争
- **请求级单调池** — 请求结束后整体释放，无逐对象析构开销
- **Scatter-Gather I/O** — 多条消息合并为一次系统调用

运行内置基准测试：

```bash
./build/examples/http_server 8080
./build/examples/http_benchmark localhost 8080 50 1000 /api/status GET
```

> 详见 [docs/performance_report.md](docs/performance_report.md)。

## 文档

- [快速上手](docs/quickstart.md) — 5 分钟教程
- [示例指南](docs/examples_guide.md) — 8 个从简到繁的完整示例
- [API 参考](docs/api_reference.md) — 完整公共 API
- [架构设计](docs/architecture.md) — 设计决策与内部实现
- [性能报告](docs/performance_report.md) — 基准测试方法与分析
- [贡献指南](CONTRIBUTING.md) — 如何参与贡献

## 许可证

[MIT](LICENSE)
