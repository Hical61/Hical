# hical

[![CI](https://github.com/Hical61/Hical/actions/workflows/ci.yml/badge.svg)](https://github.com/Hical61/Hical/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

基于 **Boost.Asio/Beast** 的现代 C++20 高性能 Web 框架，集成 PMR 内存池、协程异步 I/O 和 C++ Concepts。

> **状态：** 开发中

[English](README.md) | 简体中文

## 特性

- **Boost.Asio/Beast 后端** — 工业级网络库，每线程独立 `io_context` 模型
- **PMR 内存池** — 统一 `std::pmr` 分配器策略，缓冲区、HTTP 消息体、JSON 对象共享内存池，减少碎片，提升缓存命中率
- **协程支持** — `asio::awaitable<T>` + `co_spawn`，简洁的异步代码风格
- **C++ Concepts** — 编译期 `NetworkBackend` 类型约束，保障类型安全
- **SSL/TLS** — 模板化 `GenericConnection<SocketType>`，同时支持明文和加密连接
- **WebSocket** — WebSocket 升级与双向通信
- **路由 & 中间件** — 中间件管线（日志、认证、限流），支持路径参数
- **HTTP 服务器** — 基于 Boost.Beast 的完整 HTTP/1.1 支持（分块传输、Keep-Alive）

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
│       ├── AsioTcpConnection.h/cpp
│       ├── GenericConnection.h/cpp
│       ├── EventLoopPool.h/cpp
│       └── TcpServer.h/cpp
├── tests/             # 单元测试（Google Test）
├── examples/          # Echo Server、基准测试、PMR PoC
├── docs/              # 设计分析文档
└── CMakeLists.txt
```

## 依赖

| 依赖项 | 版本要求 |
|--------|---------|
| C++ 标准 | C++20 |
| Boost | >= 1.70（Asio、Beast、System、JSON） |
| CMake | >= 3.20 |
| OpenSSL | 必需 |
| Google Test | 必需 |
| 编译器 | GCC 14+ / Clang 18+ / MSVC 2022+ |

## 构建

### Linux / macOS

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build .
ctest
```

### Windows（MSYS2 MINGW64）

```bash
mkdir build && cd build
cmake .. -G "Ninja" -DCMAKE_BUILD_TYPE=Release
cmake --build .
ctest
```

## 快速开始

协程式 Echo Server：

```cpp
#include <boost/asio.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/use_awaitable.hpp>

using boost::asio::ip::tcp;
using boost::asio::awaitable;
using boost::asio::co_spawn;
using boost::asio::detached;
using boost::asio::use_awaitable;

awaitable<void> handleSession(tcp::socket socket)
{
    try
    {
        char data[1024];
        for (;;)
        {
            auto bytesRead = co_await socket.async_read_some(
                boost::asio::buffer(data), use_awaitable);

            co_await boost::asio::async_write(
                socket,
                boost::asio::buffer(data, bytesRead),
                use_awaitable);
        }
    }
    catch (const std::exception&)
    {
        // 连接关闭
    }
}

awaitable<void> listener(tcp::acceptor acceptor)
{
    for (;;)
    {
        auto socket = co_await acceptor.async_accept(use_awaitable);
        co_spawn(acceptor.get_executor(),
                 handleSession(std::move(socket)), detached);
    }
}

int main()
{
    boost::asio::io_context ioContext;
    tcp::acceptor acceptor(ioContext, tcp::endpoint(tcp::v4(), 8080));

    co_spawn(ioContext, listener(std::move(acceptor)), detached);
    ioContext.run();
}
```

## 许可证

[MIT](LICENSE)
