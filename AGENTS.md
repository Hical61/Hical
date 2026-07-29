# Repository Guidelines

## Project Overview

Hical is a modern C++20 high-performance web framework built on Boost.Asio, featuring a native HTTP/WebSocket stack (picohttpparser + self-developed WebSocket implementation), PMR three-tier memory pools, coroutine-based async I/O (`asio::awaitable<T>`), C++20 Concepts for compile-time type safety, a C++26 reflection layer (dual-track: native P2996 or C++20 macro fallback), and an optional coroutine-based database middleware (Boost.MySQL backend).

## Project Structure & Module Organization

- `src/core/` — Abstract interfaces, HTTP framework, routing, middleware, logging, server code, reflection layer. Must **not** include `src/asio/` headers.
- `src/asio/` — Boost.Asio concrete implementations (event loop, TCP connection, TCP server, event-loop pool)
- `src/db/` — Optional database middleware (enable with `-DHICAL_WITH_DATABASE=ON`, guarded by `HICAL_HAS_DATABASE` macro)
- `src/third_party/` — Bundled dependencies (picohttpparser)
- `tests/` — GoogleTest test suite, one `test_<feature>.cpp` per module
- `examples/` — Example servers (echo, benchmark, OpenAPI, reflection, PMR)
- `docs/` — Longer guides (architecture, coroutines, logging, OpenAPI, deployment)
- `docker/` — CI test matrix, production deployment, benchmark tooling
- `benchmark/` — Multi-framework HTTP benchmark suite (hical vs actix/drogon/cinatra/etc.)

Namespaces: public API in `hical::`, reflection in `hical::meta::`, database in `hical::db::`, internals in anonymous namespace or `detail::`.

## Build, Test, and Development Commands

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

### Windows (MSVC + vcpkg)
```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
```

### Optional Modules
```bash
cmake -B build -DHICAL_WITH_DATABASE=ON ...    # Database middleware (requires Boost.MySQL >= 1.85)
cmake -B build -DHICAL_WITH_OPENAPI=OFF ...    # Disable OpenAPI (enabled by default)
cmake -B build -DHICAL_ENABLE_REFLECTION=ON ... # C++26 reflection (requires compatible compiler)
```

### Run Tests
```bash
# Full suite
ctest --test-dir build --output-on-failure --timeout 60 -j4
# MSVC needs: ctest ... -C Release

# Single test
./build/tests/test_router
ctest --test-dir build -R test_router --output-on-failure

# CI-like Linux suite
cd docker/test && docker compose up --build --abort-on-container-exit
```

### Format & Static Analysis
```bash
# Format check (CI enforces on GCC job)
find src tests examples -name '*.h' -o -name '*.cpp' | xargs clang-format --dry-run --Werror

# Format fix
find src tests examples -name '*.h' -o -name '*.cpp' | xargs clang-format -i

# Static analysis (Clang job, non-blocking)
find src -name '*.cpp' | xargs clang-tidy -p build
```

### Architecture

**`src/core/` modules:**
- `EventLoop.h` / `Timer.h` / `TcpConnection.h` — Abstract base classes
- `Concepts.h` — C++20 concepts (`EventLoopLike`, `TcpConnectionLike`, `TimerLike`, `NetworkBackend`)
- `MemoryPool.h` — Three-tier PMR: global synchronized pool → thread-local unsynchronized pool → request-level monotonic buffer. **Critical constraint:** objects from request-level monotonic buffer must not escape request lifetime (use-after-free).
- `HttpServer.h` — Top-level facade: TcpServer + Router + MiddlewarePipeline + WebSocket + IdleScanner. SO_REUSEPORT multi-acceptor (Linux/macOS), single-acceptor fallback (Windows). Graceful stop via `releaseWork()` (no `io_context::stop()`).
- `HeaderMap.h` — `vector<pair<string,string>>` backed, case-insensitive lookup, L1-cache-friendly for typical <20 headers
- `HttpRequest.h/cpp` — Zero-copy request wrapper: `string_view` referencing connection-level read buffer, stack-allocated `array<Entry,64>` headers. Public API: `method()`, `path()`, `header()`, `body()`, `cookie()`, `queryParam()`, `formParam()`, `readJson<T>()`
- `HttpResponse.h/cpp` — Response wrapper with `FileBody` deferred async file sending, `serializeHeadTo(FixedBuffer<512>&)` zero-heap scatter-gather I/O, `setHeader()` accepts `std::string_view`
- `HttpSessionImpl.cpp` — Compilation firewall for picohttpparser + WebSocket. ReadBufferPool borrow/return (8KB thread_local pool), optimistic sync write (≤512B single-buffer), response prefix template (~90B pre-built wire bytes), IdleScanner::Guard RAII idle timeout
- `Router.h` — Static routes (O(1) hash map with transparent hashing for zero-alloc `string_view` lookup) + parameter routes (`{id}`) + wildcard routes (`*path`). Priority: static > param > wildcard. `dispatchSync()` sync fast-path skips coroutine frame (~40-130ns savings)
- `Middleware.h` — Onion-model pipeline: `SyncBeforeHandler` / `SyncAfterHandler` for zero-coroutine-frame middleware, `buildOptimizedChain()` merges consecutive sync entries into single frame
- `Coroutine.h` — `Awaitable<T>` alias, `sleep()`, `coSpawn()` with `recycling_allocator` and `logOnException`
- `Error.h/cpp` — `ErrorCode` enum + `NetworkError` struct, isolating from raw Asio error codes. Use these, not `boost::system::error_code`.
- `ConfigLoader.h/cpp` — JSON config with dot-separated key access, `HICAL_` env var override, `get<T>(key, defaultVal)`
- `StaticFiles.h` — Async file serving, ETag/304, Range Request (206), MIME detection, path traversal protection, 64MB limit
- `WsFrame.h` / `WsHandshake.h` / `WsDeflate.h/cpp` — WebSocket RFC 6455 stack: frame parsing, handshake, permessage-deflate compression
- `Reflection.h` / `MetaJson.h` / `MetaRoutes.h` — C++26 dual-track reflection: native P2996 `^^T` attributes or C++20 `HICAL_JSON`/`HICAL_ROUTES` macros. API: `toJson()`, `fromJson<T>()`, `req.readJson<T>()`, `registerRoutes()`
- `Log.h/cpp` — 6-level logging: `HICAL_LOG_INFO("port={}", 8080)`, `HICAL_LOG_INFO_STREAM`, `HICAL_LOG_INFO_IF`. TRACE eliminated under NDEBUG. Use these macros, not printf/iostream.
- `Session.h/cpp` — In-memory sessions, `makeSessionMiddleware`, `regenerate()` for fixation prevention
- `RateLimiter.h/cpp` — Token Bucket middleware, per-key limiting, 429 + Retry-After headers
- `Helmet.h/cpp` — Security headers middleware (7 headers, all toggleable)
- `JwtAuth.h/cpp` — HS256 JWT middleware, zero third-party deps (OpenSSL EVP + self-implemented Base64URL)
- `GzipCompression.h/cpp` — Response compression, auto-checks `Accept-Encoding`, small body inline, large body streaming
- `SseSession.h/cpp` — Server-Sent Events (RFC 8895), chunked streaming, 30s heartbeat
- `OpenApi*.h/cpp` — OpenAPI 3.0 auto-generation: schema → registry → document → endpoints (`/openapi.json` + `/docs`). Opt-in via `HICAL_WITH_OPENAPI=ON`.
- `IdleScanner.h/cpp` — Per-io_context centralized idle connection scanner (replaces per-connection timer coroutines), single-threaded, intrusive doubly-linked list
- `IdleFd.h` / `WriteNode.h` / `Version.h.in` / `StringPool.h` / `Multipart.h/cpp` / `ChunkedBody.h/cpp` / `FixedBuffer.h`

**`src/asio/` modules:**
- `AsioEventLoop` — Wraps `io_context`, `releaseWork()` for graceful shutdown
- `GenericConnection<SocketType>` — Template for plain + SSL sockets. MPSC lock-free write queue (`alignas(64)` cache-line isolation), `MpscNodePool` thread_local free list, `kMaxDrainBatch=256`
- `EventLoopPool` — 1:1 thread-to-io_context, least-connections distribution, `pthread_setaffinity_np` CPU pinning (Linux)
- `TcpServer` — Accept loop, `alive_` guard, SO_REUSEPORT multi-acceptor (Windows auto-fallback), `IdleFd` EMFILE protection

**`src/db/` (optional) modules:**
- `DbConnectionPool` — Coroutine-based pool with LIFO reuse, health check + idle eviction background loops, `pingGracePeriod`, auto-rollback on release
- `MysqlConnection` — Boost.MySQL backend with PreparedStatement retry, `validateCharset()` SQL injection whitelist
- `StmtCache` — Per-connection LRU PreparedStatement cache, transparent `string_view` lookup
- `DbMiddleware` / `DbQueryLog` — HTTP integration + slow query logging

### Key Patterns

- **Coroutine-based I/O**: all async ops use `co_await` + `use_awaitable`. Route handlers return `Awaitable<HttpResponse>`. Always capture `shared_from_this()` in coroutines, never raw `this`.
- **Zero-copy HTTP parsing**: `string_view` into connection-level read buffer, stack-allocated headers. Idle connections hold zero heap read buffer.
- **PMR everywhere**: `PmrBuffer`, HTTP bodies, JSON objects use `std::pmr` allocators. Request-level `monotonic_buffer` objects must not escape request scope.
- **Compilation firewalls**: `.hci` files with `extern template` + explicit instantiation in `.cpp`. Large templates must not live directly in headers.
- **Optional modules**: compile-time `#ifdef HICAL_HAS_XXX` gating via CMake options. Users not using DB/OpenAPI compile zero bytes of that code.
- **Template-based SSL**: `if constexpr (hIsSslStream<SocketType>)` for compile-time SSL vs plain branching.
- **Synchronous fast path**: `dispatchSync()` skips coroutine frame allocation for sync handlers. `SyncBeforeHandler`/`SyncAfterHandler` middleware run without coroutine overhead.

## Coding Style & Naming Conventions

### Naming Table (enforced by clang-tidy)

| Element            | Convention              | Example                             |
| ------------------ | ----------------------- | ----------------------------------- |
| Class / Struct     | CamelCase (no prefix)   | `HttpServer`, `PoolConfig`          |
| Enum               | CamelCase (no prefix)   | `HttpMethod`                        |
| Abstract/Interface | CamelCase (no prefix)   | `EventLoop`, `TcpConnection`        |
| Enum constant      | `h` prefix + CamelCase  | `hGet`, `hPost`, `hOk`              |
| Member variable    | camelBack + `_` suffix  | `router_`, `maxBodySize_`           |
| Static constexpr   | `k` prefix + CamelCase  | `kMaxPathSegments`, `kPoolKey`      |
| Global variable    | `g_` prefix + camelBack | `g_instance`                        |
| Function/Method    | camelBack               | `runAfter()`, `dispatch()`          |
| Local variable     | camelBack               | `bytesRead`                         |
| Macro              | UPPER_CASE              | `HICAL_ROUTE`                       |
| Template param     | CamelCase               | `SocketType`                        |

**Forbidden:** `m_` prefix, `C`/`I`/`E` type prefixes. No AI co-author lines in commits.

### Code Style (clang-format 22+)

- Allman braces (braces on new line), 4-space indent, 120-column limit, `InsertBraces: true`
- Qualifier order: `inline static const type`
- Pointer/reference left-aligned: `int* p`, `std::string& s`
- Arguments that don't fit one line: one per line (`BinPackArguments: false`, `BinPackParameters: false`)
- clang-tidy checks: readability, bugprone, cppcoreguidelines, modernize, misc, performance. Function ≤150 lines, nesting ≤4, params ≤5.

### File Headers

Every `.h` / `.cpp` / `.hci` must start with a Doxygen block:
```cpp
/**
 * @file filename.h
 * @brief One-line description
 */
```
Followed by `#pragma once` (for headers). Include order: own module header → project headers → third-party → stdlib.

### Error Handling

- Framework uses `ErrorCode` enum + `NetworkError`, not raw `boost::system::error_code`
- Middleware catches handler exceptions → HTTP 500
- Fatal errors: `HICAL_LOG_FATAL` → auto-abort

### Logging

Use `HICAL_LOG_*` macros exclusively, not printf/iostream. Level guide:
- TRACE — hot-path diagnostics (compiled out under NDEBUG)
- DEBUG — development
- INFO — normal events
- WARN — non-fatal anomalies
- ERROR — operation failed but service continues
- FATAL — unrecoverable, aborts

## Testing Guidelines

Add GoogleTest coverage in `tests/test_<feature>.cpp` and register with `hical_add_test(test_<feature>)` in `tests/CMakeLists.txt`. CI covers GCC 14, Clang 20 (ASan/UBSan), MSYS2 MINGW64, and MSVC. Avoid brittle timing assertions in performance tests. Database tests need `HICAL_WITH_DATABASE=ON`; most use mock connections, `test_mysql_integration.cpp` requires a live MySQL instance.

## Commit & Pull Request Guidelines

Commit prefix convention: `[feat]`, `[fix]`, `[perf]`, `[refactor]`, `[docs]`, `[test]`, `[chore]`. Branch naming: `<type>/<description>` (e.g. `feat/idle-scanner-rework`). PRs are squash-merged to `main`. Keep PRs ≤400 lines; split larger changes. Self-review before requesting review. Update `CHANGELOG.md` for behavior changes.

## Dependencies

| Dependency     | Version                                                                 |
| -------------- | ----------------------------------------------------------------------- |
| C++ Standard   | C++20 (C++26 optional for reflection)                                   |
| Boost          | >= 1.82 (Asio, System, JSON); DB middleware >= 1.85 (MySQL, charconv)   |
| OpenSSL        | Required                                                                |
| zlib           | Required (WebSocket permessage-deflate)                                 |
| picohttpparser | Bundled (system install optional via `HICAL_USE_SYSTEM_PICOHTTPPARSER`) |
| Google Test    | Required                                                                |
| CMake          | >= 3.20                                                                 |
| Compiler       | GCC 14+ / Clang 20+ / MSVC 2022+                                        |

## CI

GitHub Actions (`.github/workflows/ci.yml`): Ubuntu 24.04 (GCC 14, Clang 20) + Windows (MSYS2 MINGW64, MSVC + vcpkg). GCC job runs clang-format check; Clang job runs clang-tidy (warning mode, non-blocking).

## Security & Configuration

Do not commit credentials, certificates, or local environment files. Treat changes to TLS, JWT, static-file paths, headers, and database configuration as security-sensitive. For vulnerability reporting, see [SECURITY.md](SECURITY.md) — do **not** open a public issue.
