# Repository Guidelines

## Project Overview

Hical is a modern C++20 high-performance web framework built on Boost.Asio, featuring a native HTTP/WebSocket stack (picohttpparser + self-developed WebSocket implementation) over both plain TCP and TLS (`ssl::stream`), PMR three-tier memory pools, coroutine-based async I/O (`asio::awaitable<T>`), C++20 Concepts for compile-time type safety, a C++26 reflection layer (dual-track: native P2996 annotation values or C++20 macro fallback, plus a compile-time utility layer), and an optional coroutine-based database middleware (Boost.MySQL and libpq/PostgreSQL backends).

## Project Structure & Module Organization

- `src/core/` — Abstract interfaces, HTTP framework, routing, middleware, logging, server code, reflection layer. Must **not** include `src/asio/` headers.
- `src/asio/` — Boost.Asio concrete implementations (event loop, TCP connection, TCP server, event-loop pool, optional HTTP client)
- `src/core/http2/` — 可选 HTTP/2 协议实现：nghttp2 编解码、流状态、收发预算、TLS 服务端会话及文件生产；内部头不安装
- `src/db/` — Optional database middleware (enable with `-DHICAL_WITH_DATABASE=ON`, guarded by `HICAL_HAS_DATABASE` macro)
- `src/third_party/` — Bundled dependencies (picohttpparser)
- `tests/` — GoogleTest test suite, one `test_<feature>.cpp` per module
- `examples/` — Example servers (echo, benchmark, OpenAPI, reflection, C++26 annotation reflection, static files, PMR, PostgreSQL)
- `docs/` — Longer guides (architecture, coroutines, logging, OpenAPI, deployment)
- `docker/` — CI test matrix, production deployment, benchmark tooling
- `benchmark/` — Multi-framework HTTP benchmark suite (hical vs actix/drogon/cinatra/etc.)
- `scripts/` — Dev tooling (`gen_test_certs.py`, `bump_version.py`, `fix-comment-indent.py`, flamegraph helpers)

Namespaces: public API in `hical::`, JSON/route reflection in `hical::meta::`, C++26 annotation values in `hical::anno::`, JSON Schema specializations in `hical::schema::`, compile-time helpers in `hical::CT::` / `hical::CollectMember::`, database in `hical::db::`, internals in anonymous namespace or `detail::`.

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
cmake -B build -DHICAL_WITH_PGSQL=ON ...       # PostgreSQL backend (requires libpq, implies HICAL_WITH_DATABASE)
cmake -B build -DHICAL_WITH_OPENAPI=OFF ...    # Disable OpenAPI (enabled by default)
cmake -B build -DHICAL_ENABLE_REFLECTION=ON ... # C++26 reflection (GCC 16 + Boost >= 1.90; adds -freflection to C++ sources only)
cmake -B build -DHICAL_WITH_HTTP_CLIENT=ON ...  # Coroutine HTTP client (default OFF)
cmake -B build -DHICAL_WITH_HTTP2=ON ...        # TLS HTTP/2 双端候选实现（默认 OFF，客户端另开 HTTP_CLIENT）
```

HTTP/2 当前仍在最终验收，不能把配置入口或测试源码存在当作跨平台检查已通过。需静态 nghttp2 `>=1.70.0,<2` 和 OpenSSL 1.1.1+；服务端 Boost 下限保持 1.82，生产客户端开启时下限为 1.86。安装消费者使用 `hical::hical_core` 传递的依赖与宏，MinGW 的 `BOOST_ASIO_HAS_PTHREADS` 等线程 ABI 定义不能在消费者另设冲突值。详见 [HTTP/2 使用指南](docs/http2-guide.md)。

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
- `SslContext.h/cpp` — SSL/TLS context wrapper (`loadCertificate` / `loadPrivateKey` / `native()`). A non-empty `sslCtx_` is exactly what makes `HttpServer` wrap accepted sockets in `ssl::stream`
- `HttpServer.h` — Top-level facade: TcpServer + Router + MiddlewarePipeline + WebSocket + IdleScanner. `enableSsl(cert, key)` wraps accepted sockets in `ssl::stream<tcp::socket>` — before 2.7.1 it was a no-op stub that left plaintext running. Bind address is a member, set before `start()`: `listen(endpoint)`, `listen(ip, port)`, `listenAny(port)`, read back via `endpoint()`; the default ctor `HttpServer()` == `HttpServer(0, 1)` (OS-assigned port). SO_REUSEPORT multi-acceptor (Linux/macOS), single-acceptor fallback (Windows); both acceptor paths share one `commitAcceptedSocket` helper. Graceful stop via `releaseWork()` (no `io_context::stop()`).
- `HeaderMap.h` — `vector<pair<string,string>>` backed, case-insensitive lookup, L1-cache-friendly for typical <20 headers
- `HttpRequest.h/cpp` — Zero-copy request wrapper: `string_view` referencing connection-level read buffer, stack-allocated `array<Entry,64>` headers. Public API: `method()`, `path()`, `header()`, `body()`, `cookie()`, `queryParam()`, `formParam()`, `readJson<T>()`, `peerAddr()`. `RequestHeaders`/`NativeRequest` are **move-only** on purpose (one copy is 2KB) — `native()` returns a reference, so `auto n = req.native()` must be `auto& n`
- `HttpResponse.h/cpp` — Response wrapper with `FileBody` deferred async file sending, `serializeHeadTo(FixedBuffer<512>&)` zero-heap scatter-gather I/O, `setHeader()` accepts `std::string_view`
- `HttpSessionImpl.cpp` — Compilation firewall for picohttpparser + WebSocket. `handleSession` is templated on `SocketType` and splits plain from TLS at compile time via `if constexpr (hIsSslStream<SocketType>)`; TLS responses always go through `async_write` (never the optimistic sync path, which would corrupt OpenSSL's state machine) and WS/SSE upgrade is not instantiated at all for TLS. ReadBufferPool borrow/return (8KB thread_local pool), optimistic sync write (≤512B single-buffer), response prefix template (~90B pre-built wire bytes), IdleScanner::Guard RAII idle timeout — registered *before* `async_handshake`, so a stalled handshake is still reclaimed by the normal idle timeout. Resolves the route before reading the body, so 404/405/400 are rejected up front without swallowing the body
- `HttpProtocol.h` / `Http2Options.h` / `http2/` — HTTP/2 候选实现。`enableHttp2()` 必须配合 TLS，ALPN 选 h2 后进入独立会话；H1 路径保持独立。初始头/尾头分开，路由和中间件复用现有分发，`useBody()` 门前不暴露请求体。协议流与应用任务分别计数，RST 后 handler 自然收尾并继续占任务配额；不得把 RST 当数据库回滚。文件按流窗口和连接预算异步生产，输出回调不阻塞读盘；`gracefulStop()` 先 GOAWAY 排空，期限后强制关闭传输但仍等待业务收尾。
- `Router.h` — Static routes (compile-time perfect hash for registered routes, O(1) hash map fallback with transparent hashing for zero-alloc `string_view` lookup) + parameter routes (`{id}`) + wildcard routes (`*path`). Priority: static > param > wildcard. 404-vs-405 is decided across all three kinds (wildcards participate since 2.7.1) with the `Allow` header deduplicated by method bitmask; the decision is gated by per-first-segment method masks, so unregistered paths never scan the param/wildcard groups. `dispatchSync()` sync fast-path skips coroutine frame (~40-130ns savings). `compileTimeRoute()` for routes with compile-time middleware chains. `resolveRoute()` returns a cached `ResolveResult`; `dispatchResolved()`/`dispatchSyncResolved()` reuse it to skip a second lookup
- `Middleware.h` — Onion-model pipeline: `SyncBeforeHandler` / `SyncAfterHandler` for zero-coroutine-frame middleware, `buildOptimizedChain()` merges consecutive sync entries into single frame, `CompileTimeChain` template pre-builds chains at compile time. **Middleware runs after route resolution but before the body is read** — it only sees headers (`req.body()` is empty), an intercepting middleware means the body is never read at all, and field-level body validation belongs in the handler. The pipeline is `build()`-built once in `HttpServer::start()`; per-request tail work (read body + dispatch) is passed via the request's `internalSlot_` as a function-pointer + `void*` pair. `use()` has overloads for a bare `SyncBeforeHandler` and a bare `SyncAfterHandler` (helmet/gzip return the latter). Note: with `HICAL_ENABLE_MIDDLEWARE_PROFILING=ON`, any pipeline containing a Sync entry drops profiling entirely and logs a WARN — `middlewareStats()` returns empty for it
- `Coroutine.h` — `Awaitable<T>` alias, `sleep()`, `coSpawn()` with `recycling_allocator` and `logOnException`
- `TimerService.h/cpp` — Per-io_context centralized timer behind `hical::sleep()`: one `steady_timer` plus a deadline min-heap, instead of one timer object per sleeping coroutine. Single-threaded and lock-free — `schedule()`/`cancel()` and the callbacks must all run on the owning io_context thread. `HttpServer` builds one per io_context (base loop included) and calls `bindToCurrentThread()` there; `currentThreadTimerService()` reads the thread_local, and a thread with nothing bound falls back to its own `steady_timer`. Two limits worth knowing: the per-op `cancellation_slot` is dropped (cancelling a suspended sleep does nothing), and `shutdown()` throws away pending callbacks without running them — which is exactly why `HttpServer::stop()` never calls it.
- `Error.h/cpp` — `ErrorCode` enum + `NetworkError` struct, isolating from raw Asio error codes. Use these, not `boost::system::error_code`.
- `ConfigLoader.h/cpp` — JSON config with dot-separated key access, `HICAL_` env var override, `get<T>(key, defaultVal)`
- `StaticFiles.h` — Async file serving, ETag/304, Range Request (206), MIME detection, path traversal protection, 64MB limit
- `WsFrame.h` / `WsHandshake.h` / `WsDeflate.h/cpp` — WebSocket RFC 6455 stack: frame parsing, handshake, permessage-deflate compression
- `Reflection.h` / `MetaJson.h` / `MetaRoutes.h` — C++26 dual-track reflection: native P2996 **annotation values** (`[[=hical::anno::...]]`) or C++20 `HICAL_JSON`/`HICAL_ROUTES` macros. API: `toJson()`, `fromJson<T>()`, `req.readJson<T>()`, `registerRoutes()`. The annotation track is genuinely implemented (not aspirational), but it needs a compiler with P2996 (e.g. GCC 16) plus `-freflection`, so it's opt-in via `HICAL_ENABLE_REFLECTION=ON` and the macro track remains the default. Mind the syntax: it's an annotation *value* `[[=...]]`, not a standard attribute `[[...]]` — the old attribute form compiles fine but silently does nothing
- `MetaAnno.h` — annotation framework: three-stage handlers (Key / Serialize / Deserialize / View) plus the built-in JSON annotations `json_ignore`, `json_rename`, `json_reset`, `json_required`, and the class-level `json_snake_to_lowerCamel` / `json_snake_to_UpperCamel` / `json_camel_to_snake`. Dispatch is annotation-value + `requires` overload detection rather than attribute string matching, so type-mismatched members are skipped and user types can add their own overloads
- `MetaSchema.h` — JSON Schema type specializations (`hical::schema::kSchema<T>`): primitives, `optional`, `array`, enums, `union`, `variant`
- `Utils/CTConv.h` — compile-time naming-style conversion (`caseSnakeToLowerCamel` / `caseSnakeToUpperCamel` / `caseCamelToSnake`)
- `Utils/CollectMember.h` — inheritance-chain member collection, with two-stage filtering of name ambiguity and diamond non-virtual base-subobject ambiguity
- `Log.h/cpp` — 6-level logging: `HICAL_LOG_INFO("port={}", 8080)`, `HICAL_LOG_INFO_STREAM`, `HICAL_LOG_INFO_IF`. TRACE eliminated under NDEBUG. Use these macros, not printf/iostream.
- `Session.h/cpp` — In-memory sessions, `makeSessionMiddleware`, `regenerate()` for fixation prevention
- `RateLimiter.h/cpp` — Token Bucket middleware, per-key limiting, 429 + Retry-After headers
- `Helmet.h/cpp` — Security headers middleware (7 headers, all toggleable)
- `JwtAuth.h/cpp` — HS256 JWT middleware, zero third-party deps (OpenSSL EVP + self-implemented Base64URL)
- `GzipCompression.h/cpp` — Response compression, auto-checks `Accept-Encoding`, small body inline, large body streaming
- `SseSession.h/cpp` — Server-Sent Events (WHATWG EventSource; chunked streaming, 30s heartbeat)
- `OpenApi*.h/cpp` — OpenAPI 3.0 auto-generation: schema → registry → document → endpoints (`/openapi.json` + `/docs`). Opt-in via `HICAL_WITH_OPENAPI=ON`.
- `PerfectHashRouter.h` — Compile-time perfect hash for static routes: replaces `unordered_map` lookup with djb2 hash + multiply-shift + one string comparison at runtime, computed in `MetaRoutes` and injected into `Router`. Fallback to original hash map on miss.
- `CompileTimeChain.h` — Compile-time middleware chain pre-build: `CompileTimeChain` template unrolls middleware type lists at compile time, merging consecutive `SyncBeforeHandler`/`SyncAfterHandler` entries into single coroutine frames. `compileTimeRoute()`-registered routes dispatch via pre-built chain, skipping dynamic `buildOptimizedChain()`.
- `RequestDispatch.h` — `detail::RequestDispatchContext`: the terminal "skeleton" handler (`detail::runInternalDispatch`) plus the per-request tail handler (read body + dispatch by cached `resolveResult`), passed to the pre-built pipeline via `HttpRequest::internalSlot_` so no chain is rebuilt per request
- `CompileTimeJson.h` — Compile-time JSON serialization: `CompileTimeJson<T>` template flattens DTO struct serialization into a compile-time string concatenation chain, bypassing `boost::json::object` entirely — zero heap allocation, no `serialize()` call. `HttpResponse::jsonFrom<T>()` provides a one-liner convenience API.
- `JsonWriter.h` — Zero-DOM incremental writer behind `HttpResponse::jsonWriter()`: appends wire bytes straight onto the response body (`std::to_chars` for scalars, escapes shared with `CompileTimeJson` through `detail/JsonEscape.h`), container nesting kept in a fixed `array<Frame,32>` — no `boost::json::value`, no heap, no PMR. It's the third JSON entry point: `json()` takes a value you already have, `jsonFrom<T>()` takes a compile-time-fixed DTO, `jsonWriter()` takes shapes only known at runtime. It holds a raw pointer to the response body and is deliberately non-movable, and the `&`-qualified `jsonWriter()` signature rejects temporaries at compile time — one writer per response, one top-level value, don't move the response afterwards.
- `IdleScanner.h/cpp` — Per-io_context centralized idle connection scanner (replaces per-connection timer coroutines), single-threaded, intrusive doubly-linked list
- `IdleFd.h` / `WriteNode.h` / `Version.h.in` / `StringPool.h` / `Multipart.h/cpp` / `ChunkedBody.h/cpp` / `FixedBuffer.h`

**`src/asio/` modules:**
- `AsioEventLoop` — Wraps `io_context`, `releaseWork()` for graceful shutdown
- `GenericConnection<SocketType>` — Template for plain + SSL sockets. MPSC lock-free write queue (`alignas(64)` cache-line isolation), `MpscNodePool` thread_local free list, `kMaxDrainBatch=256`
- `EventLoopPool` — 1:1 thread-to-io_context, least-connections distribution, `pthread_setaffinity_np` CPU pinning (Linux)
- `TcpServer` — Accept loop, `alive_` guard, SO_REUSEPORT multi-acceptor (Windows auto-fallback), `IdleFd` EMFILE protection
- `SslConnection.h` — `GenericConnection<ssl::stream<tcp::socket>>` type alias, lazy-includes OpenSSL
- `HttpClient.h/cpp` + `HttpClientPool.h/cpp` — 可选协程客户端，`HICAL_WITH_HTTP_CLIENT` 默认 OFF，关闭时不编译实现、不安装客户端头。默认 H1；H2 模块开启后可用 TLS ALPN 自动协商或强制 H2。普通入口同步持有参数和内部状态，提交时记录覆盖建连等待、排队、收发的绝对期限；H1 串行、H2 有界多路复用。facade 析构取消旧请求，`close()` 取消当前代次，清理交回 owner loop，io_context 必须覆盖收尾。`verifyPeer` 同时检查证书链与目标 host/IP SAN，`sniHost` 只改路由名称。池身份包括地址、TLS context、有效 SNI、验证模式、协议和生效 H2 配置；高层共享调度与 `acquire/release` 独占租借分开。`requestDisposition` 表达传输证据，`status==0` 不证明未处理。池高层仅对 GET/HEAD/PUT/DELETE/OPTIONS 按证据至多重试一次，POST/PATCH 默认不重放；H2 要求未发送的瞬态错误或 REFUSED_STREAM/GOAWAY 的真实未处理证据，H1 仅首次复用陈旧连接、`status==0` 且命中瞬态白名单时重试。取消/超时/证书/配置/资源错误不重试，原请求与首次排队期限不重置；没有公开重试配置字段。不支持自动重定向、cookie jar 或流式下载；H2 可处理 1xx 后等待最终响应，H1 暂不支持。

**`src/db/` (optional) modules:**
- `DbConnectionPool` — Coroutine-based pool with LIFO reuse, health check + idle eviction background loops, `pingGracePeriod`, auto-rollback on release
- `MysqlConnection` — Boost.MySQL backend with PreparedStatement retry, `validateCharset()` SQL injection whitelist
- `PgsqlConnection` — libpq/PostgreSQL backend with non-blocking connect (`PQconnectStart` polling), `$1` placeholder params, `INSERT ... RETURNING` insertId
- `PgSocketAdapter` — Bridges libpq `PQsocket()` to asio `co_await` (POSIX `stream_descriptor` / Windows `WSAEventSelect`)
- `StmtCache` / `PgStmtCache` — Per-connection LRU PreparedStatement cache, transparent `string_view` lookup
- `DbMiddleware` / `DbQueryLog` — HTTP integration + slow query logging

### Key Patterns

- **Coroutine-based I/O**: all async ops use `co_await` + `use_awaitable`. Route handlers return `Awaitable<HttpResponse>`. Always capture `shared_from_this()` in coroutines, never raw `this`.
- **Zero-copy HTTP parsing**: `string_view` into connection-level read buffer, stack-allocated headers. Idle connections hold zero heap read buffer.
- **PMR everywhere**: `PmrBuffer` and JSON objects use `std::pmr` allocators. Note: the HTTP body itself is a plain `std::string`, not PMR-backed. Request-level `monotonic_buffer` objects must not escape request scope.
- **Compilation firewalls**: `.hci` files with `extern template` + explicit instantiation in `.cpp`. Large templates must not live directly in headers.
- **Optional modules**: compile-time `#ifdef HICAL_HAS_XXX` gating via CMake options. Users not using DB/OpenAPI compile zero bytes of that code.
- **Template-based SSL**: `if constexpr (hIsSslStream<SocketType>)` for compile-time SSL vs plain branching. TLS never uses the raw fd for writes, and WS/SSE are plain-only.
- **Move-only zero-copy views**: `RequestHeaders` / `NativeRequest` are move-only on purpose — a copy is 2KB, and the compiler catching it beats a silent runtime copy. Always take `native()` by reference.
- **Headers first, body later**: route resolution and middleware both run before the body is read, so invalid requests and intercepted requests never pull a body into memory. Middleware sees headers only; handlers own body-level validation.
- **Synchronous fast path**: `dispatchSync()` / `dispatchSyncResolved()` skip coroutine frame allocation for sync handlers. Middleware chains are pre-built once in `start()`; the per-request tail (read body + dispatch) is passed via `HttpRequest::internalSlot_`, so nothing is rebuilt per request. `CompileTimeChain` pre-builds middleware chains at compile time. `CompileTimeJson` serializes DTOs at compile time bypassing `boost::json::object`.

## Coding Style & Naming Conventions

### Naming Table (enforced by clang-tidy)

| Element            | Convention              | Example                        |
| ------------------ | ----------------------- | ------------------------------ |
| Class / Struct     | CamelCase (no prefix)   | `HttpServer`, `PoolConfig`     |
| Enum               | CamelCase (no prefix)   | `HttpMethod`                   |
| Abstract/Interface | CamelCase (no prefix)   | `EventLoop`, `TcpConnection`   |
| Enum constant      | `h` prefix + CamelCase  | `hGet`, `hPost`, `hOk`         |
| Member variable    | camelBack + `_` suffix  | `router_`, `maxBodySize_`      |
| Static constexpr   | `k` prefix + CamelCase  | `kMaxPathSegments`, `kPoolKey` |
| Global variable    | `g_` prefix + camelBack | `g_instance`                   |
| Function/Method    | camelBack               | `runAfter()`, `dispatch()`     |
| Local variable     | camelBack               | `bytesRead`                    |
| Macro              | UPPER_CASE              | `HICAL_ROUTE`                  |
| Template param     | CamelCase               | `SocketType`                   |

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

Add GoogleTest coverage in `tests/test_<feature>.cpp` and register with `hical_add_test(test_<feature>)` in `tests/CMakeLists.txt`. CI covers GCC 14, Clang 20 (ASan/UBSan), MSYS2 MINGW64, and MSVC. Avoid brittle timing assertions in performance tests — poll until the condition holds instead of sleeping a fixed duration. Database tests need `HICAL_WITH_DATABASE=ON`; most use mock connections, `test_mysql_integration.cpp` requires a live MySQL instance.

SSL tests read `test_server.crt` / `test_server.key` relative to the test binary's working directory (`build/tests`), so generate them there with `python scripts/gen_test_certs.py` rather than dropping them in `build/` — a misplaced cert makes those cases SKIP silently. Set `HICAL_REQUIRE_SSL_CERTS` to turn a missing cert into a hard failure instead.

`test_meta_anno` / `test_meta_schema` / `test_collect_member` need a reflection-capable compiler and `GTEST_SKIP` otherwise; `test_ct_conv` runs everywhere.

## Commit & Pull Request Guidelines

Commit prefix convention: `[feat]`, `[fix]`, `[perf]`, `[refactor]`, `[docs]`, `[test]`, `[chore]`. Branch naming: `<type>/<description>` (e.g. `feat/idle-scanner-rework`). PRs are squash-merged to `main`. Keep PRs ≤400 lines; split larger changes. Self-review before requesting review. Update `CHANGELOG.md` for behavior changes.

## Dependencies

| Dependency     | Version                                                                 |
| -------------- | ----------------------------------------------------------------------- |
| C++ Standard   | C++20 (C++26 optional for reflection)                                   |
| Boost          | >= 1.82 (Asio, System, JSON); DB middleware >= 1.85 (MySQL, charconv)   |
| HTTP 客户端    | 开启 `HICAL_WITH_HTTP_CLIENT` 时 Boost >= 1.86                          |
| nghttp2        | 仅 HTTP/2：静态 C 库，>= 1.70.0 且 < 2；OpenSSL >= 1.1.1              |
| libpq          | PostgreSQL backend only (`HICAL_WITH_PGSQL=ON`)                         |
| OpenSSL        | Required                                                                |
| zlib           | Required (WebSocket permessage-deflate)                                 |
| picohttpparser | Bundled (system install optional via `HICAL_USE_SYSTEM_PICOHTTPPARSER`) |
| Google Test    | Required                                                                |
| CMake          | >= 3.28                                                                 |
| Compiler       | GCC 14+ / Clang 20+ / MSVC 2022+ (GCC 16+ for C++26 reflection)         |

## CI

GitHub Actions (`.github/workflows/ci.yml`): a `build` matrix over Ubuntu 24.04 (GCC 14, Clang 20) and Windows (MSYS2 MINGW64, MSVC + vcpkg), plus a `middleware-profiling` job. Linux/MSVC and profiling use pinned CMake 4.4.3; MSYS2 uses its native rolling CMake package with a 4.x version check. An extra GCC matrix entry uses CMake 3.28.4 to verify the minimum supported version. Seven ctest runs in total — GCC, GCC coverage, Clang sanitizers, CMake minimum, MSYS2, MSVC, middleware-profiling — and every one of them generates SSL test certs first. GCC job runs clang-format check; Clang job runs clang-tidy (warning mode, non-blocking).

## Security & Configuration

Do not commit credentials, certificates, or local environment files — test certs are generated on the spot by `scripts/gen_test_certs.py` and never checked in. Treat changes to TLS, JWT, static-file paths, headers, and database configuration as security-sensitive. For vulnerability reporting, see [SECURITY.md](SECURITY.md) — do **not** open a public issue.
