# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Hical is a modern C++20 high-performance web framework built on Boost.Asio, featuring a native HTTP/WebSocket stack (picohttpparser + self-developed WebSocket implementation), PMR memory pools, coroutine-based async I/O (`asio::awaitable<T>`), C++20 Concepts for compile-time type safety, a C++26 reflection layer (dual-track: native P2996 or C++20 macro fallback), and an optional coroutine-based database middleware (Boost.MySQL backend).

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

### Windows (MSVC + vcpkg)
```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
```

### Enable Database Middleware (requires Boost.MySQL)
```bash
cmake -B build -DHICAL_WITH_DATABASE=ON ...
```

### Disable OpenAPI module (enabled by default)
```bash
cmake -B build -DHICAL_WITH_OPENAPI=OFF ...
```

### Enable C++26 Reflection (requires compatible compiler)
```bash
cmake -B build -DHICAL_ENABLE_REFLECTION=ON ...
```

### Run All Tests
```bash
ctest --test-dir build --output-on-failure --timeout 60 -j4
# MSVC needs: ctest ... -C Release
```

### Run a Single Test
```bash
./build/tests/test_router
# Or via ctest with filter:
ctest --test-dir build -R test_router --output-on-failure
```

### Format Check (CI enforces this on GCC job)
```bash
find src tests examples -name '*.h' -o -name '*.cpp' | xargs clang-format --dry-run --Werror
```

### Static Analysis (Clang job, non-blocking)
```bash
find src -name '*.cpp' | xargs clang-tidy -p build
```

## Architecture

### Two-Layer Design

**`src/core/`** — Abstract interfaces, shared types, HTTP framework, and reflection layer:
- `EventLoop.h` / `Timer.h` / `TcpConnection.h` — Abstract base classes (pure virtual). `TcpConnection` includes `sendFile()` and `lastActiveTime()` virtual methods
- `Concepts.h` — C++20 concepts (`EventLoopLike`, `TcpConnectionLike`, `TimerLike`, `NetworkBackend`) for compile-time backend constraints
- `MemoryPool.h` — Three-tier PMR memory strategy: global synchronized pool, thread-local unsynchronized pool, request-level monotonic buffer
- `HttpServer.h` — Top-level facade integrating TcpServer + Router + MiddlewarePipeline + WebSocket middleware pre-build + fd exhaustion handling. SO_REUSEPORT multi-acceptor model (Linux/macOS) with single-acceptor fallback (Windows). Per-io_context `IdleScanner` instances for centralized idle connection scanning (replaces per-connection timer coroutines). Destructor: `shutdown()` all scanners (break timer→timer_service edge) before io_context destruction; Windows IOCP: `restart()+poll()` drain abort completions before `~io_context()` to avoid two-phase coroutine frame destruction crash
- `HeaderMap.h` — HTTP header container: `vector<pair<string,string>>` backed, case-insensitive find/set/insert, multi-value support (Set-Cookie). L1-cache-friendly linear scan for typical <20 headers
- `HttpRequest.h/cpp` — Zero-copy HTTP request wrapper. `NativeRequest` stores `string_view` target + `RequestHeaders` (stack-allocated `array<Entry,64>`, zero heap allocation) referencing connection-level read buffer. `HttpRequest::fromParsed()` factory for parser output. Public API: `method()`, `path()`, `target()`, `header()`, `body()`, `cookie()`, `queryParam()`, `formParam()`, `setAttribute()`
- `HttpResponse.h/cpp` — HTTP response wrapper. `NativeResponse` stores owned `HeaderMap` + `string body` + `optional<FileBody> fileBody` (path/offset/length for deferred async file sending). `serialize()` produces full HTTP wire bytes. `serializeHeadTo(FixedBuffer<512>&)` for zero-heap-alloc scatter-gather I/O. `serializeHeadTo(buf, prefix, prefixLen)` overload accepts pre-built common headers (Server/Connection/Date wire bytes) to skip per-request insert+serialize. `preparePayload()` sets Content-Length via `std::to_chars` (supports both string body and FileBody). `HttpResponse` class provides `setFileBody()`, `hasFileBody()`, `rangeNotSatisfiable()` factory
- `HttpSessionImpl.cpp` — Compilation firewall isolating picohttpparser + self-developed WebSocket implementation. Connection-level `std::string readBuf` (reused across keep-alive requests, no per-request allocation). **Response prefix template**: connection-level `responsePrefix[128]` pre-builds `Server`/`Connection`/`Date` wire bytes (~90B), keep-alive requests use `memcpy` instead of 3× `HeaderMap::insert` + serialize loop; Date updated once per second (29B memcpy). Single-buffer serialization: head+body into one `FixedBuffer<512>`, one `async_write` call. `writeFileResponse()` coroutine for `FileBody` responses (Range requests): `TcpCorkGuard` RAII (Linux `TCP_CORK` / macOS `TCP_NOPUSH` / Windows no-op) merges header + first 64KB chunk into one TCP segment; sends headers first, then 64KB async chunked file reads. WebSocket bridge: reconstructs native request at upgrade time (rare path). Idle timeout via `IdleScanner::Guard` RAII registration: `Entry` on coroutine stack (zero heap allocation), `touch()` on request read/write; scanner is nullptr when `idleTimeout_ == 0` (Guard becomes no-op). `SocketGuard` conditional shutdown: only calls `shutdown()` on clean keep-alive exit, eliminating useless syscalls on already-disconnected sockets
- `Router.h` — Static routes (hash map O(1) with transparent hashing via `RouteKeyView`/`is_transparent` for zero-alloc `string_view` lookup) + parameter routes (`{id}` pattern, per-method grouping via `unordered_map<HttpMethod, vector>`) + WebSocket routes with `WsOptions` (Origin whitelist). `dispatchSync()` synchronous fast path: sync-registered handlers skip coroutine frame allocation (~40-130ns/req). `resolveRoute()` unifies URL decode + path depth check + static/param lookup for both `dispatch()` and `dispatchSync()`
- `Middleware.h` — Onion-model middleware pipeline with `MiddlewareNext` chaining; supports pre-built chain (`build()`), dynamic chain (`buildChain()`), and `buildFor()` for external pre-build, with separate `execute()` overloads for cached vs dynamic paths. `SyncBeforeHandler` / `SyncAfterHandler` / `SyncMiddlewareResult` types for zero-coroutine-frame middleware. `MiddlewareEntry` tagged union (Async/Sync). `buildOptimizedChain()`: consecutive sync entries merged into single coroutine frame, N sync middleware = 1 heap allocation
- `Error.h/cpp` — Unified error code mapping: 21 `ErrorCode` enum values (connection/address/operation/SSL categories) + `NetworkError` struct, isolating upper layers from direct Asio error code dependency
- `Coroutine.h` — `Awaitable<T>` alias for `boost::asio::awaitable<T>`, plus `sleep()` / `coSpawn()` helpers. `coSpawn()` overload with arbitrary executor + `logOnException` (replaces `detached`, unhandled exceptions logged to stderr). All `coSpawn()` overloads use `boost::asio::bind_allocator(recycling_allocator<void>(), ...)` to reuse completion handler memory via `thread_local` cache, avoiding per-spawn malloc/free under high concurrency
- `WsFrame.h` — WebSocket RFC 6455 frame parser/constructor: data/control frame unification, client masking enforcement, RSV bit validation, control frame size limit (≤125B)
- `WsHandshake.h` — WebSocket handshake protocol: `Sec-WebSocket-Key`/`Accept` computation, extension negotiation (permessage-deflate)
- `WsDeflate.h/cpp` — WebSocket permessage-deflate compression extension (RFC 7692), pimpl-encapsulated zlib, configurable `serverMaxWindowBits`/`clientMaxWindowBits`/`serverNoContextTakeover`, zip bomb protection
- `MetaJsonError.h/cpp` — `[[noreturn]]` non-template error functions (`throwTypeMismatch` / `throwMissingField` / `throwParseError`), extracted from template code to reduce per-type `HICAL_JSON` instantiation code size and improve icache utilization
- `Reflection.h` / `MetaJson.h` / `MetaRoutes.h` — C++26 reflection layer (see below)
- `StaticFiles.h` — Async static file serving (`Awaitable<HttpResponse>`) with `BOOST_ASIO_HAS_FILE` async I/O + ifstream fallback, PathCache (4096/60s TTL), ETag/304, MIME detection, path traversal protection, 64MB file size limit, HTTP 206 Range Request support (single range, `If-Range` ETag conditional, `FileBody` deferred async chunked sending for large ranges >4MB, `Accept-Ranges: bytes` on all 200 responses)
- `Multipart.h/cpp` — RFC 7578 multipart/form-data parser (256 part DoS limit), dual API: `getFile(req, field)` (re-parses) and `getFile(parts, field)` (searches pre-parsed vector, recommended)
- `Session.h/cpp` — In-memory session manager with `shared_mutex` (read-write lock), lazy GC, OpenSSL RAND_bytes 128-bit IDs, `makeSessionMiddleware` factory, `maxSessions` DoS limit, atomic `lastAccess` (lock-free), `regenerate()` for session fixation prevention, `migrateFrom()` for atomic data migration with address-ordered double locking
- `Log.h/cpp` — Production-grade logging system: 6-level `LogLevel` (Trace/Debug/Info/Warn/Error/Fatal), `Logger` singleton with `std::format`-style API (`HICAL_LOG_INFO("port={}", 8080)`), streaming API (`HICAL_LOG_INFO_STREAM << val`), conditional macros (`HICAL_LOG_INFO_IF`), compile-time TRACE elimination under NDEBUG, `thread_local` timestamp cache + thread ID cache, configurable flush level (`setFlushLevel()`), Fatal auto-abort
- `LogRecord.h` — Structured log entry: level, timestamp, threadId, file, line, message, `boost::json::object` fields, traceId
- `LogFormatter.h/cpp` — Log formatter interface + `TextFormatter` (Phase 1/2 compatible text output with `thread_local` timestamp cache) + `JsonFormatter` (JSON Lines via `boost::json::serialize`, UTC timestamps)
- `LogSink.h/cpp` — Pluggable log output backend interface (`LogSink` abstract) + `StderrSink` (fprintf) + `FileSink` (sync fwrite + `LogFile` rotation) + `OStreamSink` (thread-safe ostream wrapper for `setOutput()` compat)
- `LogFile.h/cpp` — Log file rotation engine: size-based rotation (default 100MB), max file count limit, timestamp-sequenced archive naming (`app.YYMMDD-HHMMSS.NNNNNN.log`), strict filename matching for cleanup, `FILE*`-based I/O
- `AsyncFileSink.h/cpp` — Async double-buffered file Sink: `std::jthread` + `stop_token` background thread, 4MB front/back buffer swap, `condition_variable_any` wakeup, backpressure protection (drop + count), 1s timeout flush, graceful shutdown with final `curBuf_` drain
- `FixedBuffer.h` — Stack-allocated fixed buffer template (default 4KB), `std::to_chars` integer/float formatting, heap fallback on overflow, replaces `std::ostringstream` in `LogStream`
- `LogChannel.h/cpp` — Named log channel with independent level/formatter/sinks, `LogChannelRegistry` with `shared_mutex` (read-many-write-rarely), `HICAL_LOG_TO("channel", Info, fmt, ...)` macro
- `LogMiddleware.h/cpp` — Onion-model logging middleware: auto trace-id generation (OpenSSL RAND_bytes 128-bit hex), `req.setAttribute("hical.trace_id", ...)`, structured access log to named channel (method/path/status/latency_ms)
- `LogAdmin.h/cpp` — Dynamic log level admin endpoints: `GET /admin/log-level` (query all levels) + `PUT /admin/log-level` (adjust default or per-channel level at runtime)
- `OpenApiSchema.h` — C++20 JSON Schema generation: `jsonSchema<T>()` auto-generates OpenAPI 3.0 Schema Objects from the `FieldDescriptor` tuple of the `HICAL_JSON` macro, supporting primitive types/vector/nested structs/$ref. `HICAL_SCHEMA_NAME` macro registers type names for `$ref` references. `collectSchemas<T>()` recursively collects nested schemas
- `OpenApiRegistry.h/cpp` — Route metadata registry: `RouteApiInfo` stores route annotations (summary/tags/requestBody/responses). `OpenApiRegistry` is a thread-safe registry (mutex + snapshot return). `HICAL_API()` all-in-one annotation macro + `builder::*` helpers. `HICAL_ROUTES_WITH_API()` enhanced route collection macro. `registerRoutesWithOpenApi()` registers routes and collects metadata simultaneously
- `OpenApiDocument.h/cpp` — OpenAPI 3.0 document assembly: `OpenApiDocument` lazily generates and caches the full JSON document (mutex + bool flag). `OpenApiConfig` holds configuration (title/version/description/servers). Automatic path parameter extraction (`{param}` pattern). Multiple methods on the same path are merged into a single Path Item
- `OpenApiEndpoint.h` — Endpoint exposure: `serveOpenApi()` registers `/openapi.json` (JSON spec) + `/docs` (Swagger UI CDN page) in one call. The jsonPath is safely escaped via `boost::json::serialize()` to prevent JS injection
- `IdleScanner.h/cpp` — Per-io_context centralized idle connection scanner replacing per-connection `steady_timer` coroutines. Intrusive doubly-linked list + `optional<steady_timer>`, single-threaded (no locks). `Entry` struct embedded on coroutine stack (zero heap allocation) with `atomic<int64_t> lastActiveMs` + `socket*`. `Guard` RAII class accepts `IdleScanner*` (nullptr = no-op). `run()` sets `thread_local` pointer via `currentThreadIdleScanner()`. Scan interval: `max(1s, timeout/4)`. `stop()` posts `timer_.cancel()` to executor (cross-thread safe). `shutdown()` cancels and resets optional timer (idempotent), called in `~HttpServer()` body to break circular dependency with io_context (timer → timer_service edge)
- `IdleFd.h` — Cross-platform idle fd reservation (POSIX: `/dev/null` fd; Windows: no-op stub) for EMFILE accept loop protection
- `WriteNode.h` — Polymorphic write buffer nodes: `WriteNode` base, `MemoryWriteNode` (shared_ptr\<string\>), `FileWriteNode` (path/offset/length) for heterogeneous send queue
- `Version.h.in` — CMake-configured version header (single source of truth from `project(VERSION)`)

**`src/asio/`** — Boost.Asio concrete implementations:
- `AsioEventLoop` — Wraps `boost::asio::io_context`, implements `EventLoop`
- `GenericConnection<SocketType>` — Template supporting both `tcp::socket` (plain) and `ssl::stream<tcp::socket>` (SSL). Write queue uses Vyukov intrusive MPSC lock-free queue (`MpscNode` + `MpscQueue` with embedded stub sentinel, `alignas(64) atomic<MpscNode*> tail_` for cache-line isolation, wait-free O(1) push, amortized O(1) pop). `MpscNodePool`: thread_local free list (max 128 nodes) for `MpscNode` allocation/deallocation, eliminating malloc/free on hot path; `allocateNode()` / `deallocateNode()` static methods with placement new/explicit destructor. `WriteEntry` tagged union (hMemory: inline `shared_ptr<string>`, hNode: polymorphic `shared_ptr<WriteNode>`). `sendFile()` + `sendFileNode()` for async file I/O with `BOOST_ASIO_HAS_FILE` guard + ifstream fallback. `lastActiveTimeMs_` atomic for idle detection. `reading_` is `atomic<bool>` for thread-safe `stopRead()`. Template implementation extracted to `GenericConnection.hci` compilation firewall (`extern template` + explicit instantiation in `.cpp`, `#ifndef HICAL_BUILDING_GENERIC_CONNECTION` guard)
- `SslConnection.h` — Lightweight SSL connection type alias (`SslConnection = GenericConnection<ssl::stream<tcp::socket>>`), lazy OpenSSL include
- `EventLoopPool` — Multi-threaded pool (1 thread : 1 io_context), round-robin connection distribution. `start()` binds each worker thread to its corresponding CPU core via `pthread_setaffinity_np` (Linux only) to reduce TLB flush and cross-core IPI from thread migration
- `TcpServer` — Accept loop managing connection lifecycle, `alive_` flag guards coroutine against use-after-this, `setIdleTimeout()` + `idleCheckLoop()` for idle connection cleanup, `unordered_set` connection storage (O(1)), `IdleFd` for EMFILE protection. SO_REUSEPORT multi-acceptor model: each worker loop runs its own acceptor, accept and I/O on same thread (zero cross-thread dispatch); Windows auto-fallback to single acceptor

**`src/db/`** — Optional coroutine-based database middleware (enabled via `HICAL_WITH_DATABASE=ON`, guarded by `HICAL_HAS_DATABASE` macro). Namespace: `hical::db`. Four-layer architecture:
- `DbConfig.h` — `struct DbConfig` with pool sizing (`minConnections`/`maxConnections`), timeouts (`idleTimeout`/`acquireTimeout`/`queryTimeout`), health check intervals (`healthCheckInterval`/`pingGracePeriod`/`idleCheckInterval`), `stmtCacheSize` (per-connection LRU capacity), `autoReconnect`, `charset`
- `DbResult.h` — `struct DbResult` with `columns`/`rows` (string-based), `affectedRows`, `insertId`, `columnIndex()` for name-based lookup
- `DbConnection.h` — Abstract interface (pure virtual). Parameterized `query()`/`execute()` with `std::span<const std::string>` params (deprecated non-parameterized overloads with `[[deprecated]]`). Transaction control: `beginTransaction()`/`commit()`/`rollback()`/`inTransaction()`. Connection health: `ping()`/`isAlive()`/`lastActiveTime()`/`lastPingTime()`/`touch()`. All async methods return `Awaitable<T>`
- `DbConnectionPool.h/cpp` — Coroutine-based connection pool using `steady_timer` as coroutine semaphore (no `condition_variable`). LIFO idle connection reuse, background `healthCheckLoop` (ping + replenish to `minConnections`), `idleCheckLoop` (evict idle beyond `idleTimeout`), `pingGracePeriod` optimization (skip ping if recently checked), automatic rollback on release if `inTransaction()`. Factory pattern via `DbConnectionFactory` function type
- `DbMiddleware.h` — HTTP middleware integration: `makeDbMiddleware()` factory with `DbMiddlewareOptions` (`autoTransaction`/`injectPool`). Helper functions `getDbConnection(req)`/`getDbPool(req)`. Onion model: acquire → inject → [auto begin] → next → [auto commit/rollback] → release. Request attribute keys: `hPoolKey`/`hConnKey`
- `DbQueryLog.h/cpp` — Query logging middleware via decorator pattern (`LoggingDbConnection` wraps real connection). `makeQueryLogMiddleware()` with `QueryLogOptions`: `onRequestComplete` callback, `slowQueryThreshold` + `onSlowQuery` for slow query detection. Must be registered **after** `makeDbMiddleware()`. `QueryLogEntry` struct records `sql`/`duration`/`rowCount`/`affectedRows`/`isParameterized`
- `MysqlConnection.h/cpp` — Boost.MySQL backend using `any_connection` (type-erased TCP/SSL). `create()` async factory + `makeFactory()` for pool integration. Full type conversion (int64/uint64/double/string/blob/date/datetime/time/NULL). PreparedStatement retry on stale statement. `validateCharset()` whitelist against SQL injection in `SET NAMES`
- `StmtCache.h/cpp` — Per-connection LRU PreparedStatement cache (not thread-safe, one instance per connection). `std::list` + `std::unordered_map` with transparent `StringHash`/`StringEqual` for zero-alloc `string_view` lookup. Evicted statements returned to caller for async close

### Key Patterns

- **Coroutine-based I/O**: All async operations use `co_await` with `boost::asio::use_awaitable`. Route handlers return `Awaitable<HttpResponse>`.
- **Template-based SSL**: `GenericConnection<SocketType>` uses `if constexpr (hIsSslStream<SocketType>)` to branch SSL vs plain logic at compile time.
- **PMR everywhere**: Buffers (`PmrBuffer`), HTTP bodies, and JSON objects use `std::pmr` allocators from the three-tier pool.
- **Zero-copy HTTP parsing**: picohttpparser parses into stack-allocated `phr_header[64]` array. `NativeRequest` stores `string_view` referencing connection-level `readBuf` — zero heap allocation for headers/target. Body uses owned `std::string` (read directly from socket). Single-buffer response serialization: head+body into `FixedBuffer<512>`, one `async_write` call.
- **Backend abstraction**: `AsioBackend` struct bundles `AsioEventLoop` + `PlainConnection` + `AsioTimer` to satisfy the `NetworkBackend` concept. Future backends can be swapped in.
- **Namespaces**: Public API in `hical::`, reflection layer in `hical::meta::`, database middleware in `hical::db::`.
- **Optional OpenAPI module**: `src/core/OpenApi*.h/cpp` is opt-in via `HICAL_WITH_OPENAPI=ON` (default ON). `HICAL_HAS_OPENAPI` macro guards all OpenAPI code. Four-layer design: Schema generation → Registry → Document assembly → Endpoint exposure. Zero-invasive: does not modify MetaJson.h / MetaRoutes.h / Router.h.
- **Optional DB module**: Entire `src/db/` is opt-in via `HICAL_WITH_DATABASE=ON`. `HICAL_HAS_DATABASE` macro guards all DB code at compile boundaries. DB core layer (pool/middleware/query log) is backend-agnostic; MySQL backend is a separate layer. Adding PostgreSQL requires only a new `if(HICAL_WITH_PGSQL)` CMake block.
- **Synchronous fast path**: `Router::dispatchSync()` returns result directly for sync-registered handlers, skipping coroutine frame allocation (~40-130ns savings). `SyncBeforeHandler`/`SyncAfterHandler` allow middleware to run without coroutine overhead. `buildOptimizedChain()` merges N consecutive sync middleware into a single coroutine frame. `HttpSessionImpl` no-middleware path calls `dispatchSync()` first, falls back to `co_await dispatch()` on `nullopt`.
- **Compilation firewalls**: `GenericConnection.hci` extracts ~780-line template implementation from header; `extern template` declarations + explicit instantiation in `.cpp` prevent re-compilation on user code changes. `HttpSessionImpl.cpp` isolates picohttpparser + WebSocket heavy-template code from `HttpServer.h`.

### C++26 Reflection Layer (Dual-Track)

Core design principle: when `HICAL_HAS_REFLECTION == 1` (compiler supports P2996 or `HICAL_FORCE_REFLECTION` is defined), use native C++26 reflection. Otherwise, fall back to C++20 macros providing the same user API.

**`Reflection.h`** — Feature detection (`HICAL_HAS_REFLECTION`), `RouteInfo` struct, `HasRouteTable` / `HasJsonFields` type traits.

**`MetaJson.h`** — Automatic JSON serialization/deserialization:
- C++26 path: `^^T` + `std::meta::nonstatic_data_members_of` enumerates fields automatically, supports `[[hical::json_name("alias")]]`, `[[hical::json_required]]`, `[[hical::json_ignore]]` attributes, plus `jsonSchema<T>()` and `toJsonSnakeCase<T>()`
- C++20 fallback: `HICAL_JSON(Type, ...)` macro with `__VA_OPT__` recursive expansion (no field count limit), IS_PAREN + Tag dispatch for decorator syntax: `ALIAS(field, "key")`, `REQUIRED(field)`, `REQUIRED_ALIAS(field, "key")`, `HICAL_IGNORE(field)`. Compile-time field validation via `static_assert + requires`
- API: `hical::meta::toJson(obj)`, `hical::meta::fromJson<T>(json)`, `req.readJson<T>()`

**`MetaRoutes.h`** — Automatic route registration:
- C++26 path: `[[hical::route(...)]]` attribute on member functions
- C++20 fallback: `HICAL_HANDLER(Method, "/path", funcName)` + `HICAL_ROUTES(Type, func1, func2, ...)`
- API: `hical::meta::registerRoutes(router, handler)`

## Naming Conventions (enforced by clang-tidy)

| Element            | Convention              | Example                             |
| ------------------ | ----------------------- | ----------------------------------- |
| Class / Struct     | CamelCase (no prefix)   | `HttpServer`, `PoolConfig`          |
| Enum               | CamelCase (no prefix)   | `HttpMethod`                        |
| Abstract/Interface | CamelCase (no prefix)   | `EventLoop`, `TcpConnection`        |
| Enum constant      | `h` prefix + CamelCase  | `hGet`, `hPost`, `hOk`              |
| Member variable    | camelBack + `_` suffix  | `router_`, `maxBodySize_`, `ioCtx_` |
| Static constexpr   | `k` prefix + CamelCase  | `kMaxPathSegments`, `kPoolKey`      |
| Global variable    | `g_` prefix + camelBack | `g_instance`                        |
| Function/Method    | camelBack               | `runAfter()`, `dispatch()`          |
| Local variable     | camelBack               | `bytesRead`                         |
| Macro              | UPPER_CASE              | `HICAL_ROUTE`                       |
| Template param     | CamelCase               | `SocketType`                        |

**Note**: Class/Struct/Enum/Interface types do not use C/S/E/I prefixes — plain CamelCase only (e.g. `HttpServer`, `RouteInfo`, `HttpMethod`, `EventLoop`). Enum constants use `h` prefix (for hical namespace identity) with scoped `enum class`. Static constexpr constants use `k` prefix (Google style, e.g. `kMaxPathSegments`, `kPoolKey`). All member variables use trailing underscore — no `m_` prefix anywhere in the codebase.

## Code Style

- **clang-format**: Requires version 22+. On Windows use MSYS2 MINGW64's `C:\msys64\mingw64\bin\clang-format.exe`. Allman brace style (braces on new line), 4-space indent, 120-char column limit, `InsertBraces: true`, `UseTab: ForContinuationAndIndentation`
- **clang-tidy**: readability, bugprone, cppcoreguidelines, modernize, misc, performance checks enabled. Function line threshold: 150, nesting threshold: 4, parameter threshold: 5
- Qualifier order: `inline static const type`
- Pointer/reference alignment: left (`int* p`, `std::string& s`)
- `BinPackArguments: false`, `BinPackParameters: false` — each argument on its own line when they don't fit one line

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

> **Note:** The OpenAPI module (`HICAL_WITH_OPENAPI=ON`) introduces no new dependencies; it reuses the existing Boost.JSON.

## Test Structure

39 test executables in `tests/` (+ 5 optional DB tests), each linked against `hical_core` + `GTest::gtest_main`. Tests are registered via `gtest_discover_tests()` for CTest integration. On Windows, tests also link `ws2_32` and `mswsock`. Key test files:
- `test_router.cpp` / `test_router_perf.cpp` — Route dispatch and performance
- `test_memory_pool.cpp` — Three-tier PMR allocation
- `test_http_server.cpp` / `test_integration.cpp` — Full HTTP request/response cycle
- `test_middleware.cpp` — Onion-model middleware pipeline
- `test_ssl_connection.cpp` — SSL/TLS handshake
- `test_websocket.cpp` — WebSocket messaging
- `test_concepts.cpp` — Compile-time concept verification
- `test_reflection.cpp` — MetaJson + MetaRoutes reflection layer (39 tests: alias, required, ignore, mixed decorators, large field count, backward compat)
- `test_cookie.cpp` — Cookie parsing and Set-Cookie header
- `test_static_files.cpp` — Static file serving, ETag, path traversal
- `test_multipart.cpp` — multipart/form-data parsing
- `test_session.cpp` — Session lifecycle and thread safety
- `test_openapi.cpp` — OpenAPI auto-generation (35 tests: schema generation for all types/decorators, registry CRUD, document assembly with path merging/param extraction/caching, endpoint registration, full integration workflow)
- `test_log.cpp` — Log system (36 tests: format API, level filter, thread ID, timestamp, flush strategy, Fatal abort, stream macros, conditional macros, Sink API, multi-Sink dispatch)
- `test_log_ndebug.cpp` — NDEBUG compile-out verification (3 tests: TRACE/TRACE_IF/TRACE_STREAM eliminated)
- `test_fixed_buffer.cpp` — FixedBuffer stack buffer (19 tests: append, overflow fallback, integer/float/bool formatting, clear)
- `test_log_file.cpp` — LogFile rotation engine (7 tests: write, size rotation, max files, naming format, nested dirs)
- `test_async_file_sink.cpp` — AsyncFileSink (7 tests: basic write, multi-thread, graceful shutdown, rotation, sink level)
- `test_log_formatter.cpp` — TextFormatter + JsonFormatter (12 tests: output format, traceId, filename extraction, JSON validity, structured fields)
- `test_log_channel.cpp` — LogChannel + Registry (12 tests: emit, level filter, custom formatter, multi-sink, registry CRUD, HICAL_LOG_TO/HICAL_LOG_TO_F macros)
- `test_log_middleware.cpp` — LogMiddleware (3 tests: trace-id generation length/hex/uniqueness)
- `test_log_admin.cpp` — LogAdmin endpoints (4 tests: registration, custom prefix, level round-trip, dynamic channel level)
- `test_error.cpp` — Unified error code mapping and NetworkError
- `test_http_types.cpp` — HTTP types and status codes
- `test_asio_event_loop.cpp` / `test_asio_timer.cpp` / `test_coroutine.cpp` — Asio backend and coroutine primitives
- `test_asio_tcp_connection.cpp` / `test_tcp_server.cpp` — TCP connection and server lifecycle
- `test_basic.cpp` — Basic framework smoke tests

### Database Tests (requires `HICAL_WITH_DATABASE=ON`)

5 additional test executables, 4 use `MockDbConnection` (no real DB needed), 1 requires a live MySQL instance (auto-skips if unavailable):
- `test_db_pool.cpp` — Connection pool: acquire/release, health check, idle eviction, statistics, ping grace period (12 tests)
- `test_db_middleware.cpp` — DB middleware: connection injection, auto-transaction commit/rollback, onion model integration (8 tests)
- `test_db_query_log.cpp` — Query log middleware: recording, slow query detection, callbacks, connection restore (6 tests)
- `test_stmt_cache.cpp` — PreparedStatement LRU cache: eviction, promotion, disabled mode (9 tests)
- `test_mysql_integration.cpp` — Real MySQL: CRUD, transactions, parameterized queries, pool integration (7 tests, env vars: `MYSQL_HOST`/`MYSQL_PORT`/`MYSQL_USER`/`MYSQL_PASSWORD`/`MYSQL_DATABASE`)

## CI

GitHub Actions (`.github/workflows/ci.yml`): matrix of Ubuntu 24.04 (GCC 14, Clang 20) + Windows (MSYS2 MINGW64, MSVC + vcpkg). GCC job runs clang-format check; Clang job runs clang-tidy (warning mode, non-blocking).
