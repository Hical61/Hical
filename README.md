# hical

[![CI](https://github.com/Hical61/Hical/actions/workflows/ci.yml/badge.svg)](https://github.com/Hical61/Hical/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)](https://isocpp.org/)
[![Platform](https://img.shields.io/badge/Platform-Linux%20%7C%20Windows%20%7C%20macOS-green.svg)]()
[![Boost](https://img.shields.io/badge/Boost-%E2%89%A51.70-orange.svg)](https://www.boost.org/)

Hical is a modern C++ web framework built on Boost.Asio/Beast, utilizing C++26 reflection and PMR memory pooling to achieve high performance.

> **Status:** Work in Progress

English | [简体中文](README_CN.md)

## Features

- **C++26 Reflection** — Designed around C++26 static reflection for automatic route registration, serialization, and compile-time metaprogramming
- **Boost.Asio/Beast Backend** — Industrial-grade networking with `io_context` per-thread model
- **PMR Memory Pool** — Unified `std::pmr` allocator strategy across buffers, HTTP bodies, and JSON objects for reduced fragmentation and improved cache locality
- **Coroutine Support** — `asio::awaitable<T>` + `co_spawn` for clean async code
- **C++ Concepts** — Compile-time `NetworkBackend` constraints for type safety
- **SSL/TLS** — Template-based `GenericConnection<SocketType>` supporting both plain and encrypted connections
- **WebSocket** — WebSocket upgrade and bidirectional communication
- **Router & Middleware** — Middleware pipeline (logging, auth, rate limiting) with path parameter support
- **HTTP Server** — Full HTTP/1.1 support via Boost.Beast (chunked transfer, keep-alive)

## Why Hical?

| | Hical | Drogon | Crow |
|---|---|---|---|
| **C++ Standard** | C++20 (C++26 ready) | C++17 | C++11 |
| **Async Model** | Coroutines (`co_await`) | Callbacks + Coroutines | Callbacks |
| **Memory Strategy** | 3-tier PMR pool | Default allocator | Default allocator |
| **HTTP Parser** | Boost.Beast | Custom (Trantor) | Custom |
| **SSL** | Compile-time template branching | Runtime branch | Runtime branch |
| **Backend Abstraction** | C++20 Concepts | N/A | N/A |

## Quick Start

```cpp
#include "core/HttpServer.h"
#include "core/WebSocket.h"

using namespace hical;

int main()
{
    HttpServer server(8080);

    // Middleware — logging
    server.use([](const HttpRequest& req, MiddlewareNext next)
                   -> Awaitable<HttpResponse> {
        std::cout << httpMethodToString(req.method()) << " "
                  << req.path() << std::endl;
        co_return co_await next(req);
    });

    // GET / — JSON response
    server.router().get("/", [](const HttpRequest&) -> HttpResponse {
        return HttpResponse::json({
            {"status", "running"},
            {"framework", "hical"}
        });
    });

    // GET /users/{id} — path parameters
    server.router().get("/users/{id}",
        [](const HttpRequest& req) -> HttpResponse {
            return HttpResponse::json({{"userId", req.param("id")}});
        });

    // WebSocket echo
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

> See [docs/quickstart.md](docs/quickstart.md) for the full tutorial and [examples/](examples/) for more.

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
│       ├── GenericConnection.h/cpp
│       ├── EventLoopPool.h/cpp
│       └── TcpServer.h/cpp
├── tests/             # Unit tests (Google Test)
├── examples/          # HTTP server, WebSocket, benchmarks, PMR demos
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
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
```

### Windows (MSYS2 MINGW64)

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

## Performance

Hical features a three-tier PMR memory pool architecture:

- **Thread-local pools** — lock-free allocation, zero contention across threads
- **Request-level monotonic pools** — bulk deallocation at request end, no per-object overhead
- **Scatter-Gather I/O** — multiple messages coalesced into a single system call

Run the built-in benchmarks:

```bash
./build/examples/http_server 8080
./build/examples/http_benchmark localhost 8080 50 1000 /api/status GET
```

> See [docs/performance_report.md](docs/performance_report.md) for methodology and analysis.

## Documentation

- [Quick Start Guide](docs/quickstart.md) — 5-minute tutorial
- [Examples Guide](docs/examples_guide.md) — 8 progressive examples
- [API Reference](docs/api_reference.md) — Complete public API
- [Architecture](docs/architecture.md) — Design decisions and internals
- [Performance Report](docs/performance_report.md) — Benchmarking methodology
- [Contributing](CONTRIBUTING.md) — How to contribute

## License

[MIT](LICENSE)
