# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Hical is a modern C++20 high-performance web framework built on Boost.Asio/Beast, featuring PMR memory pools, coroutine-based async I/O (`asio::awaitable<T>`), and C++ Concepts for compile-time type safety. Status: Work in Progress.

## Build Commands

### Linux / macOS
```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

### Windows (MSYS2 MINGW64)
```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

### Run All Tests
```bash
ctest --test-dir build --output-on-failure --timeout 60 -j4
```

### Run a Single Test
```bash
# Each test is a separate executable, e.g.:
./build/tests/test_router
# Or via ctest with filter:
ctest --test-dir build -R test_router --output-on-failure
```

### Format Check
```bash
find src tests examples -name '*.h' -o -name '*.cpp' | xargs clang-format --dry-run --Werror
```

### Static Analysis
```bash
find src -name '*.cpp' | xargs clang-tidy -p build
```

## Architecture

### Two-Layer Design

**`src/core/`** — Abstract interfaces, shared types, and HTTP framework:
- `EventLoop.h` / `Timer.h` / `TcpConnection.h` — Abstract base classes (pure virtual)
- `Concepts.h` — C++20 concepts (`EventLoopLike`, `TcpConnectionLike`, `TimerLike`, `NetworkBackend`) for compile-time backend constraints
- `MemoryPool.h` — Three-tier PMR memory strategy: global synchronized pool, thread-local unsynchronized pool, request-level monotonic buffer
- `HttpServer.h` — Top-level facade integrating TcpServer + Router + MiddlewarePipeline
- `Router.h` — Static routes (hash map O(1)) + parameter routes (`{id}` pattern, linear scan) + WebSocket routes
- `Middleware.h` — Onion-model middleware pipeline with `MiddlewareNext` chaining
- `Coroutine.h` — `Awaitable<T>` alias for `boost::asio::awaitable<T>`, plus `sleep()` / `coSpawn()` helpers

**`src/asio/`** — Boost.Asio concrete implementations:
- `AsioEventLoop` — Wraps `boost::asio::io_context`, implements `EventLoop`
- `GenericConnection<SocketType>` — Template supporting both `tcp::socket` (plain) and `ssl::stream<tcp::socket>` (SSL). Type aliases: `PlainConnection`, `SslConnection`
- `EventLoopPool` — Multi-threaded pool (1 thread : 1 io_context), round-robin connection distribution
- `TcpServer` — Accept loop managing connection lifecycle

### Key Patterns

- **Coroutine-based I/O**: All async operations use `co_await` with `boost::asio::use_awaitable`. Route handlers return `Awaitable<HttpResponse>`.
- **Template-based SSL**: `GenericConnection<SocketType>` uses `if constexpr (hIsSslStream<SocketType>)` to branch SSL vs plain logic at compile time.
- **PMR everywhere**: Buffers (`PmrBuffer`), HTTP bodies, and JSON objects use `std::pmr` allocators from the three-tier pool.
- **Backend abstraction**: `AsioBackend` struct bundles `AsioEventLoop` + `PlainConnection` + `AsioTimer` to satisfy the `NetworkBackend` concept. Future backends can be swapped in.

## Naming Conventions (enforced by clang-tidy)

| Element            | Convention              | Example                    |
| ------------------ | ----------------------- | -------------------------- |
| Class              | `C` prefix + CamelCase  | `CMyClass`                 |
| Struct             | `S` prefix + CamelCase  | `SRouteKey`                |
| Enum               | `E` prefix + CamelCase  | `EHttpMethod`              |
| Abstract/Interface | `I` prefix + CamelCase  | `IEventLoop`               |
| Enum constant      | `E` prefix + CamelCase  | `EGet`, `EPost`            |
| Member variable    | `m_` prefix + camelBack | `m_ioContext`              |
| Global variable    | `g_` prefix + camelBack | `g_instance`               |
| Static variable    | `s` prefix + camelBack  | `sThreadPool`              |
| Function/Method    | camelBack               | `runAfter()`, `dispatch()` |
| Local variable     | camelBack               | `bytesRead`                |
| Pointer param      | `p` prefix + CamelCase  | `pSocket`                  |
| Macro              | UPPER_CASE              | `HICAL_ROUTE`              |
| Template param     | CamelCase               | `SocketType`               |

**Note**: The existing codebase uses a slightly relaxed form — many types omit the C/S/E/I prefix (e.g., `HttpServer` not `CHttpServer`, `PoolConfig` not `SPoolConfig`). Follow the existing style in each file.

## Code Style

- **clang-format**: Allman brace style (braces on new line), 4-space indent, 120-char column limit, `InsertBraces: true`
- **clang-tidy**: readability, bugprone, cppcoreguidelines, modernize, performance checks enabled. Function line threshold: 150, nesting threshold: 4, parameter threshold: 5
- Qualifier order: `inline static const type`
- Pointer/reference alignment: left (`int* p`, `std::string& s`)

## Dependencies

| Dependency   | Version                             |
| ------------ | ----------------------------------- |
| C++ Standard | C++20                               |
| Boost        | >= 1.70 (Asio, Beast, System, JSON) |
| OpenSSL      | Required                            |
| Google Test  | Required                            |
| CMake        | >= 3.20                             |
| Compiler     | GCC 14+ / Clang 18+ / MSVC 2022+    |

## Test Structure

17 test executables in `tests/`, each linked against `hical_core` + `GTest::gtest_main`. Tests are registered via `gtest_discover_tests()` for CTest integration. Key test files:
- `test_router.cpp` / `test_router_perf.cpp` — Route dispatch and performance
- `test_memory_pool.cpp` — Three-tier PMR allocation
- `test_http_server.cpp` / `test_integration.cpp` — Full HTTP request/response cycle
- `test_middleware.cpp` — Onion-model middleware pipeline
- `test_ssl_connection.cpp` — SSL/TLS handshake
- `test_websocket.cpp` — WebSocket messaging
- `test_concepts.cpp` — Compile-time concept verification

## CI

GitHub Actions (`.github/workflows/ci.yml`): matrix of Ubuntu 24.04 (GCC 14, Clang 18) + Windows (MSYS2 MINGW64). GCC job runs clang-format check; Clang job runs clang-tidy (warning mode, non-blocking).
