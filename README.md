# hical

[![CI](https://github.com/Hical61/Hical/actions/workflows/ci.yml/badge.svg)](https://github.com/Hical61/Hical/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![C++ Standard](https://img.shields.io/badge/C%2B%2B-20%20%7C%2026-blue.svg)](https://isocpp.org/)
[![Platform](https://img.shields.io/badge/Platform-Linux%20%7C%20Windows%20%7C%20macOS-green.svg)]()
[![Boost](https://img.shields.io/badge/Boost-%E2%89%A51.82-orange.svg)](https://www.boost.org/)
[![GitHub release](https://img.shields.io/github/v/release/Hical61/Hical?include_prereleases&sort=semver)](https://github.com/Hical61/Hical/releases)
[![GitHub stars](https://img.shields.io/github/stars/Hical61/Hical?style=flat)](https://github.com/Hical61/Hical/stargazers)
[![PRs Welcome](https://img.shields.io/badge/PRs-welcome-brightgreen.svg)](CONTRIBUTING.md)

Hical is a modern C++ web framework built on Boost.Asio with a native HTTP/WebSocket stack, utilizing C++26 reflection and PMR memory pooling to achieve high performance.

> C++20/26 dual-track reflection · PMR memory pool · coroutine async I/O · Cookie / Session / StaticFiles / Multipart built-in · CORS · RouteGroup · logging system · OpenAPI 3.0 auto-docs · Optional DB middleware (Boost.MySQL)

English | [简体中文](README_CN.md)

## Features

- **C++26 Reflection** — Designed around C++26 static reflection for automatic route registration, JSON serialization/deserialization, and compile-time metaprogramming; C++20 macro fallback provides the same API
- **Boost.Asio Backend** — Industrial-grade networking with `io_context` per-thread model
- **PMR Memory Pool** — Three-tier `std::pmr` allocator strategy (global synchronized pool, thread-local pool, request-level monotonic buffer) across buffers, HTTP bodies, and JSON objects
- **Coroutine Support** — `asio::awaitable<T>` + `co_spawn` for clean async code
- **C++ Concepts** — Compile-time `NetworkBackend` constraints for type safety
- **SSL/TLS** — Template-based `GenericConnection<SocketType>` with compile-time `if constexpr` branching for plain and encrypted connections
- **WebSocket** — WebSocket upgrade and bidirectional communication with Origin whitelist
- **Router & Middleware** — Onion-model middleware pipeline with path parameter support (`{id}`), static route O(1) hash lookup, per-method parameter route grouping
- **RouteGroup** — Prefix-based route grouping with group-level middleware, supporting nested groups
- **CORS Middleware** — Built-in Cross-Origin Resource Sharing middleware with automatic preflight handling
- **HTTP Server** — Full HTTP/1.1 support with native picohttpparser-based stack (chunked transfer, keep-alive), fd exhaustion protection
- **Cookie Support** — RFC 6265 compliant parsing (first-wins semantics) + `Set-Cookie` with CRLF injection protection
- **Static File Serving** — MIME auto-detection, ETag/304 caching, path traversal protection, async file I/O, PathCache (4096 entries / 60s TTL), 64 MB size limit
- **Multipart File Upload** — RFC 7578 `multipart/form-data` parser with DoS protection (≤256 parts)
- **Session Management** — In-memory `SessionManager` with lazy GC, 128-bit random IDs, thread-safe `Session` objects, session fixation prevention (`regenerate()`), atomic data migration (`migrateFrom()`)
- **Logging System** — 6-level logging (Trace–Fatal), `std::format` + streaming + conditional macro APIs, pluggable Sinks (Stderr/File/AsyncFile/OStream), named Channels, JSON/Text formatters, async double-buffered file writes, TRACE compile-time elimination under NDEBUG, dynamic level management endpoints (LogAdmin)
- **OpenAPI 3.0 Auto-Docs** — Auto-generate JSON Schema from `HICAL_JSON` macros, `HICAL_API()` route annotations, one-call setup for `/openapi.json` + Swagger UI at `/docs`
- **Optional Database Middleware** — Coroutine-based Boost.MySQL backend with connection pool (LIFO reuse, health check, idle eviction), auto-transaction, query logging with slow query detection, LRU prepared statement cache

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
│   ├── core/            # Abstract interfaces, shared types, HTTP framework & reflection
│   │   ├── EventLoop.h, Timer.h, TcpConnection.h   # Abstract base classes
│   │   ├── Concepts.h, Coroutine.h                  # C++20 Concepts & coroutines
│   │   ├── MemoryPool.h/cpp, PmrBuffer.h            # Three-tier PMR memory pool
│   │   ├── HttpServer.h/cpp, HttpRequest.h/cpp      # HTTP server
│   │   ├── HttpResponse.h/cpp, HttpTypes.h          # HTTP response & types
│   │   ├── Router.h/cpp, RouteGroup.h/cpp           # Router & route grouping
│   │   ├── Middleware.h/cpp, Cors.h                 # Middleware & CORS
│   │   ├── WebSocket.h/cpp, SslContext.h/cpp        # WebSocket & SSL
│   │   ├── Cookie.h, Session.h/cpp                  # Cookie & session
│   │   ├── StaticFiles.h, Multipart.h/cpp           # Static files & file upload
│   │   ├── Reflection.h, MetaJson.h, MetaRoutes.h   # C++26 reflection layer
│   │   ├── Log.h/cpp, LogRecord.h, LogFormatter.h/cpp  # Logging core
│   │   ├── LogSink.h/cpp, LogFile.h/cpp             # Log sinks & file rotation
│   │   ├── AsyncFileSink.h/cpp, FixedBuffer.h       # Async sink & stack buffer
│   │   ├── LogChannel.h/cpp, LogMiddleware.h/cpp    # Log channels & middleware
│   │   ├── LogAdmin.h/cpp                           # Dynamic log level endpoints
│   │   ├── OpenApiSchema.h, OpenApiRegistry.h/cpp   # OpenAPI schema & registry
│   │   ├── OpenApiDocument.h/cpp, OpenApiEndpoint.h # OpenAPI document & endpoints
│   │   └── IdleFd.h, WriteNode.h, Version.h.in      # Utilities
│   ├── asio/            # Boost.Asio concrete implementations
│   │   ├── AsioEventLoop.h/cpp, AsioTimer.h/cpp
│   │   ├── GenericConnection.h/cpp, SslConnection.h
│   │   ├── EventLoopPool.h/cpp, TcpServer.h/cpp
│   │   └── ...
│   └── db/              # Optional database middleware (HICAL_WITH_DATABASE=ON)
│       ├── DbConfig.h, DbResult.h, DbConnection.h  # Config, result, abstract interface
│       ├── DbConnectionPool.h/cpp                   # Coroutine-based connection pool
│       ├── DbMiddleware.h                           # HTTP middleware integration
│       ├── DbQueryLog.h/cpp                         # Query logging & slow query detection
│       ├── MysqlConnection.h/cpp                    # Boost.MySQL backend
│       └── StmtCache.h/cpp                          # LRU prepared statement cache
├── tests/               # Unit tests (Google Test, 30+ test executables)
├── examples/            # HTTP server, WebSocket, OpenAPI, benchmarks, PMR demos
├── docs/                # Design documents and guides
└── CMakeLists.txt
```

## Requirements

| Dependency   | Version                             |
| ------------ | ----------------------------------- |
| C++ Standard | C++20 / C++26                       |
| Boost        | >= 1.82 (Asio, System, JSON); DB middleware >= 1.85 (MySQL, charconv) |
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
# Download & extract (replace VERSION with the desired release, e.g.)
curl -LO https://github.com/Hical61/Hical/releases/download/vVERSION/hical-VERSION-conan-src.tar.gz
tar xzf hical-VERSION-conan-src.tar.gz

# Export to local Conan cache
cd hical
conan export . --version=VERSION
```

Then add to your `conanfile.txt`:

```ini
[requires]
hical/VERSION

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

#### Windows (MSVC + vcpkg)

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
ctest --test-dir build --output-on-failure -C Release
```

#### Enable Optional Modules

```bash
# Database middleware (requires Boost.MySQL >= 1.85)
cmake -B build -DHICAL_WITH_DATABASE=ON ...

# Disable OpenAPI module (enabled by default)
cmake -B build -DHICAL_WITH_OPENAPI=OFF ...

# Enable C++26 Reflection (requires compatible compiler)
cmake -B build -DHICAL_ENABLE_REFLECTION=ON ...
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
- [Build & Test Guide](docs/build_and_test_guide.md) — Build, test, and CI configuration
- [Examples Guide](docs/examples_guide.md) — Progressive examples
- [API Reference](docs/api_reference.md) — Complete public API
- [Architecture](docs/architecture.md) — Design decisions and internals
- [Integration Guide](docs/integration_guide.md) — vcpkg / Conan / CMake integration
- [Project Structure](docs/project_structure.md) — Source directory and module reference
- [Performance Report](docs/performance_report.md) — Benchmarking methodology
- [Contributing](CONTRIBUTING.md) — How to contribute

## License

[MIT](LICENSE)
