# hical

[![CI](https://github.com/Hical61/Hical/actions/workflows/ci.yml/badge.svg)](https://github.com/Hical61/Hical/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![C++ Standard](https://img.shields.io/badge/C%2B%2B-20%20%7C%2026-blue.svg)](https://isocpp.org/)
[![Platform](https://img.shields.io/badge/Platform-Linux%20%7C%20Windows%20%7C%20macOS-green.svg)]()
[![Boost](https://img.shields.io/badge/Boost-%E2%89%A51.82-orange.svg)](https://www.boost.org/)
[![GitHub release](https://img.shields.io/github/v/release/Hical61/Hical?include_prereleases&sort=semver)](https://github.com/Hical61/Hical/releases)
[![GitHub stars](https://img.shields.io/github/stars/Hical61/Hical?style=flat)](https://github.com/Hical61/Hical/stargazers)
[![PRs Welcome](https://img.shields.io/badge/PRs-welcome-brightgreen.svg)](CONTRIBUTING.md)

Hical is a modern C++ web framework built on Boost.Asio/Beast, utilizing C++26 reflection and PMR memory pooling to achieve high performance.

> **v2.2.0** — C++20/26 dual-track reflection · PMR memory pool · coroutine async I/O · Cookie / Session / StaticFiles / Multipart built-in

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
- **Cookie Support** — RFC 6265 compliant parsing (first-wins semantics) + `Set-Cookie` with CRLF injection protection
- **Static File Serving** — MIME auto-detection, ETag/304 caching, path traversal protection, 64 MB size limit
- **Multipart File Upload** — RFC 7578 `multipart/form-data` parser with DoS protection (≤256 parts)
- **Session Management** — In-memory `SessionManager` with lazy GC, 128-bit random IDs, thread-safe `Session` objects

## Why Hical?

|                         | Hical                           | Drogon                 | Crow              |
| ----------------------- | ------------------------------- | ---------------------- | ----------------- |
| **C++ Standard**        | C++20 (C++26 ready)             | C++17                  | C++11             |
| **Async Model**         | Coroutines (`co_await`)         | Callbacks + Coroutines | Callbacks         |
| **Memory Strategy**     | 3-tier PMR pool                 | Default allocator      | Default allocator |
| **HTTP Parser**         | Boost.Beast                     | Custom (Trantor)       | Custom            |
| **SSL**                 | Compile-time template branching | Runtime branch         | Runtime branch    |
| **Backend Abstraction** | C++20 Concepts                  | N/A                    | N/A               |
| **Cookie / Session**    | Built-in                        | Built-in               | Limited           |
| **Static Files**        | Built-in (ETag, DoS guard)      | Built-in               | Built-in          |
| **File Upload**         | Built-in (part limit guard)     | Built-in               | Built-in          |

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
| C++ Standard | C++20 / C++26                       |
| Boost        | >= 1.82 (Asio, Beast, System, JSON) |
| CMake        | >= 3.20                             |
| OpenSSL      | Required                            |
| Google Test  | Required                            |
| Compiler     | GCC 14+ / Clang 20+ / MSVC 2022+    |

## Installation

### vcpkg (Recommended)

```bash
vcpkg install hical61-hical
```

Then in your `CMakeLists.txt`:

```cmake
find_package(hical CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE hical::hical_core)
```

### Conan

Download the Conan source package from [GitHub Releases](https://github.com/Hical61/Hical/releases) and export to local cache:

```bash
# Download & extract (replace version as needed)
curl -LO https://github.com/Hical61/Hical/releases/download/v2.2.0/hical-2.2.0-conan-src.tar.gz
tar xzf hical-2.2.0-conan-src.tar.gz

# Export to local Conan cache
cd hical
conan export . --version=2.2.0
```

Then add to your `conanfile.txt`:

```ini
[requires]
hical/2.2.0

[generators]
CMakeDeps
CMakeToolchain
```

Install and build:

```bash
conan install . --build=missing
```

Then in your `CMakeLists.txt`:

```cmake
find_package(hical REQUIRED)
target_link_libraries(my_app PRIVATE hical::hical_core)
```


### Build from Source

#### Linux / macOS

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
```

#### Windows (MSYS2 MINGW64)

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
