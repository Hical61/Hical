# hical

[![CI](https://github.com/Hical61/Hical/actions/workflows/ci.yml/badge.svg)](https://github.com/Hical61/Hical/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

Hical is a modern C++web framework built on Boost. Asio, utilizing C++26 reflection and pmr memory pooling to achieve high performance

> **Status:** Work in Progress

## Features

- **Boost.Asio/Beast Backend** — Industrial-grade networking with `io_context` per-thread model
- **PMR Memory Pool** — Unified `std::pmr` allocator strategy across buffers, HTTP bodies, and JSON objects for reduced fragmentation and improved cache locality
- **Coroutine Support** — `asio::awaitable<T>` + `co_spawn` for clean async code
- **C++ Concepts** — Compile-time `NetworkBackend` constraints for type safety
- **SSL/TLS** — Template-based `GenericConnection<SocketType>` supporting both plain and encrypted connections
- **WebSocket** — WebSocket upgrade and bidirectional communication
- **Router & Middleware** — Middleware pipeline (logging, auth, rate limiting) with path parameter support
- **HTTP Server** — Full HTTP/1.1 support via Boost.Beast (chunked transfer, keep-alive)

## Project Structure

```
hical/
├── src/
│   ├── core/          # Abstract interfaces & shared types
│   │   ├── EventLoop.h, Timer.h, TcpConnection.h
│   │   ├── MemoryPool.h/cpp, PmrBuffer.h
│   │   ├── Error.h/cpp, Concepts.h, Coroutine.h
│   │   ├── HttpServer.h/cpp, HttpRequest.h/cpp, HttpResponse.h/cpp
│   │   ├── Router.h/cpp, Middleware.h/cpp
│   │   ├── WebSocket.h/cpp, SslContext.h/cpp
│   │   └── HttpTypes.h, InetAddress.h/cpp
│   └── asio/          # Boost.Asio implementations
│       ├── AsioEventLoop.h/cpp
│       ├── AsioTimer.h/cpp
│       ├── AsioTcpConnection.h/cpp
│       ├── GenericConnection.h/cpp
│       ├── EventLoopPool.h/cpp
│       └── TcpServer.h/cpp
├── tests/             # Unit tests (Google Test)
├── examples/          # Echo server, benchmark, PMR PoC
├── docs/              # Design analysis documents
└── CMakeLists.txt
```

## Requirements

| Dependency   | Version                             |
| ------------ | ----------------------------------- |
| C++ Standard | C++20                               |
| Boost        | >= 1.70 (Asio, Beast, System, JSON) |
| CMake        | >= 3.20                             |
| OpenSSL      | Required                            |
| Google Test  | Required                            |
| Compiler     | GCC 14+ / Clang 18+ / MSVC 2022+    |

## Build

### Linux / macOS

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build .
ctest
```

### Windows (MSYS2 MINGW64)

```bash
mkdir build && cd build
cmake .. -G "Ninja" -DCMAKE_BUILD_TYPE=Release
cmake --build .
ctest
```

## Quick Start

A coroutine-based Echo Server:

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
        // Connection closed
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

## License

[MIT](LICENSE)
