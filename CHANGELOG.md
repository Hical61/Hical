# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [2.6.5] - 2026-06-12

### Added
- **Expect: 100-continue 支持**（[#8](https://github.com/Hical61/Hical/issues/8)）：Router 新增 `exists()` 路由预检方法，HttpSessionImpl 解析 `Expect` 头；路由不存在返回 404，body 大小超限返回 413，均通过时发送 `100 Continue` 让客户端传输 body。配套集成测试

### Changed
- **`setBody` API 变更**（[#9](https://github.com/Hical61/Hical/issues/9)）：`setBody(body)` 单参版本不再自动覆写 `Content-Type`，得用双参 `setBody(body, contentType)` 手动传。框架内部所有内置错误响应（`notFound`/`badRequest`/`serverError`/`rangeNotSatisfiable`）和调用点都改成显式传参了
- **`OpenApiDocument::generateString()` 线程安全**：改成按值返回，之前 `invalidate()` 跨线程调的时候跟 `generateString()` 并发读会有 data race
- **`/docs` 页面缓存**：加了 `Cache-Control: public, max-age=3600`
- **`recommendedMaxConnections` 上限放开**：从 65535 提到 100 万

### Performance
- **空闲长连接内存砍半**：搞了个 `ReadBufferPool`（thread_local 无锁池，`BufferHandle` RAII 接管借还），空闲时不占 readBuf，来了请求借一块、写完了还回去。空闲连接从 17.44 KB 降到 ~8 KB。顺带把 `GenericConnection::inputBuffer_` 改成 `optional<PmrBuffer>` 懒分配，readLoop 第一次进来才创建，每个空闲连接再省 ~2 KB。按百万长连接算总共省了大概 9.5 GB
- **`HeaderMap::toLower()` 编译期 256 查表**：`constexpr array` 替代挨个 `tolower()`，头部大小写归一化热路径加速
- **`Router::urlDecode()` 编译期 hex 查表**：256 条 `constexpr` 查表替代运行时分支判断，URL 解码加速
- **`HttpSessionImpl` Connection 头预扫描**：解析 header 时顺手把 `Connection` 值抓出来，省了后面单独 O(n) 扫一遍

### Fixed
- **burst 建连时连接数限制失效**：原来逻辑是先 load 判断再 fetch_add，但 `handleSession` 走 `coSpawn` 异步投递，高速建连时计数跟不上，限流失效。改成 accept 处先 fetch_add 占位，超限就 fetch_sub 退回来 close socket，`AcceptGuard` RAII 保底不回漏；`maxConnections_` 改成 `atomic` 让多 acceptor 并发没问题
- **WS 升级路径堆损坏**：一串三个生命周期 bug——PoolSlots 线程退出时裸指针没释放（LeakSanitizer 能抓到）、IOCP 两阶段析构后 readBuf 还到已销毁的 tlsPool、socket move 后 idleEntry 还挂在 IdleScanner 链表上。修法：move socket 前把 readBuf 和 Guard 都先 release 掉，handleWebSocket 签名改成接 string 参数，从根上断了对 readBuf 的引用
- **MinGW thread_local 析构时序炸堆**：CI 上 msys2-gcc 测试跑完了退进程时爆 0xC0000374 或 SEGFAULT。原因是 PoolSlots 的析构在 DLL TLS 回调里跑，那时候 CRT 堆已经没了。修法：`#ifndef __MINGW32__` 跳过 MinGW 下的 TLS 清理，进程退出 OS 自己收
- **AsyncFileSink 析构 data race**：bgThread_ 成员声明在 flushRequests_ 前面，C++ 反向析构时 flushRequests_ 先被销毁，后台线程还在写它，TSan（Clang-20）抓到。把 bgThread_ 移到 flushRequests_ 后面，保证先 join 线程再销毁队列

## [2.6.4] - 2026-05-30

### Added
- **`IdleScanner` 集中式空闲连接扫描**（`IdleScanner.h/cpp`）：per-io_context 单定时器 + 侵入式双链表替代 per-connection `steady_timer` 协程。`Entry` 嵌入协程栈（零堆分配），`atomic<int64_t> lastActiveMs` 无锁时间戳，`Guard` RAII 管理注册/注销生命周期。扫描间隔 `max(1s, timeout/4)`，`shutdown()` 幂等取消 timer 切断与 `timer_service` 的依赖
- **`StringPool` 线程本地字符串对象池**（`StringPool.h`）：5 级大小类（256/512/1K/2K/4K，各 32 槽），`acquire(size)` 返回池化 `shared_ptr<string>`，自定义 deleter 归还池中。`send()` 热路径池化避免 per-send malloc/free

### Performance
- **IdleScanner 替代 per-connection timer**：每连接独立 `steady_timer` 协程改为每 io_context 一个 `IdleScanner` 集中扫描，消除每连接的 timer 协程帧分配和 `epoll_ctl` 调用
- **EventLoopPool 最少连接分发**：`nextLoop()` 从 round-robin 改为挑选 `connectionCount_` 最小的 loop，负载均衡更优
- **GenericConnection writeLoop drain 上限**：`kMaxDrainBatch = 256` 限制单轮 drain 节点数，防止突发队列饥饿 io_context 其他任务
- **DbConnectionPool acquire 快速淘汰**：`acquire()` 在持锁期间 `isAlive()` 预检，死连接收集后在锁外析构，避免无效 ping
- **idleCheckLoop 精确唤醒**：`min_element` 找到最早过期的空闲连接，timer 精确到期唤醒，`idleCheckInterval` 作为最大等待兜底，减少无效扫描
- **响应前缀模板化**：连接级 `responsePrefix[128]` 预构建 `Server`/`Connection`/`Date` 通用头部 wire bytes（~90B），keep-alive 请求 `memcpy` 替代 3 次 `HeaderMap::insert` + 序列化循环

### Fixed
- **IdleScanner UAF**：`idleScanners_` 声明顺序调整到 `baseLoop_`/`ioPool_` 之前（scanner 比 io_context 后析构），`timer_` 改 `optional<steady_timer>` + `shutdown()` 在 `~HttpServer()` 中提前 cancel+reset timer，双向切断 scanner ↔ io_context 环形依赖
- **Windows IOCP 关服 SegFault**：根因为 `io_context::stop()` 后 IOCP queue 中 abort completions 未 dispatch，`~io_context()` 强杀协程帧导致崩溃。最终方案：移除 `io_context::stop()` 调用，改为 cancel 所有 pending op + `releaseWork()` 释放 work_guard，协程自然退出后 `run()` 返回，queue 为空无两阶段销毁问题。同时修复 `stop()` 跨线程直接操作 timer/socket 的 UB（改为 `post` 到对应线程）、WS 升级后连接未注册到 scanner、`gcLoop` timer 提升为成员变量 `gcTimer_` 确保可 cancel、`IdleScanner` 新增 `closeAll()` 关闭残留 socket

## [2.6.3] - 2026-05-25

### Added
- **HTTP 206 Range 请求**：`StaticFiles` 支持单范围 `Range` / `If-Range` ETag 条件请求，`parseByteRange()` 解析器，`HttpResponse` 新增 `FileBody`（path/offset/length）延迟异步分块发送，200 响应自动添加 `Accept-Ranges: bytes`
- **`MimallocResource`**：可选 mimalloc 作为 PMR 最底层 upstream 分配器（`HICAL_WITH_MIMALLOC` CMake 选项，Conan/vcpkg 同步）

### Changed
- **统一命名风格**：`m_` 前缀全部迁移为尾下划线，`static constexpr` 常量改用 `k` 前缀，枚举值补 `h` 前缀

### Performance
- **GenericConnection MPSC 无锁写队列**：写队列从 `mutex` + `deque` 升级为 Vyukov Intrusive MPSC Queue（wait-free O(1) push，摊销 O(1) pop），移除 `isInLoopThread` 分支和 `lock_guard`，`writeLoop` 批量 drain + `seq_cst` 反饥饿 re-check
- **TcpServer 连接表 per-loop 分片**：全局 `mutex` + `unordered_set` 改为 per-loop `LoopShard`，idle 扫描/增删全程无锁
- **requestPool 无锁扩容**：`createRequestPool()` upstream 从 `globalPool` 改为 `threadLocalPool`，扩容零锁竞争
- **Worker 线程 CPU 亲和性绑核**（Linux）：`EventLoopPool::start()` 使用 `pthread_setaffinity_np` 将 worker 线程绑定到对应 CPU 核心，减少线程迁移导致的 TLB flush 和跨核 IPI
- **coSpawn recycling_allocator**：所有 `coSpawn()` 重载改用 `boost::asio::bind_allocator(recycling_allocator<void>(), ...)` 包装 completion handler，`thread_local` 缓存避免高并发下重复 malloc/free
- **Idle timeout 条件初始化**：`HttpSessionImpl` 会话协程中 `socketAlive` / `lastActiveMs` 仅在 `idleTimeout_ > 0` 时分配，benchmark 场景设 `setIdleTimeout(0)` 后每连接省 3 次 `make_shared` + 1 个 timer 协程
- **SocketGuard 条件 shutdown**：析构时仅在 `cleanExit`（正常 keep-alive 结束）才执行 `shutdown()`，避免对已断开连接的无效系统调用
- **HTTP 响应前缀模板化**：连接级预构建 `Server` / `Connection` / `Date` 三个通用头部的 wire bytes（~90B），keep-alive 请求直接 `memcpy` 替代 3 次 `HeaderMap::insert` + 逐字段序列化循环；`Date` 值每秒最多一次 29B 覆写。新增 `NativeResponse::serializeHeadTo(buf, prefix, prefixLen)` 带前缀重载
- **TcpCorkGuard 文件响应合并小包**：`writeFileResponse` 路径新增 RAII `TcpCorkGuard`（Linux `TCP_CORK` / macOS `TCP_NOPUSH` / Windows no-op），将头部（~200B）与首个 64KB 文件 chunk 合并为一个 TCP 段发送，消除 TCP_NODELAY 下的小包问题
- **Docker 压测环境升级**：启用 mimalloc 分配器（`HICAL_WITH_MIMALLOC=ON`）、RPS/RFS 网络亲和配置（`bench-entrypoint.sh`）、容器内存提升至 1GB + fd 上限 65536

## [2.6.2] - 2026-05-18

### Added
- **WsHub 广播管理器**（`WsHub.h/cpp`）：连接注册/移除、房间分组、广播（`shared_mutex` + `weak_ptr`，跨线程 `coSpawn` 安全写入）
- **WebSocket Binary 消息**：`sendBinary()` / `receiveMessage()` 支持 Text/Binary 类型区分
- **WebSocket 写串行化**：`acquireWrite()` / `releaseWrite()` 防止 Ping 与消息并发 `async_write`
- **WebSocket 自定义关闭码**：`closeAsync(code, reason)` + 连接上下文 `setContext()` / `getContext<T>()`
- **WebSocket 心跳**：`WsOptions` 增加心跳间隔配置，集成 Ping 协程
- **WebSocket 子协议协商**：`negotiateSubprotocol()` 服务端优先匹配 + `Sec-WebSocket-Protocol` 响应头
- **WebSocket 参数路由**：`findWsRoute()` 返回 `WsRouteMatch` 支持 `{param}` 路径参数

### Changed
- **WebSocket 代码消重 + 零分配优化**：`receive()` / `receiveMessage()` 合并为 `receiveInternal()`，消除 ~150 行重复；`WsHub` broadcast 合并为 `broadcastImpl`，接口统一改 `string_view`；透明哈希应用于 `allowedOrigins` / `rooms`；握手 key 拼接改栈上 `char[64]`；`sendTo()` 单目标用 `string move` 替代 `shared_ptr` 控制块分配
- **vcpkg ports 完善**：picohttpparser 声明为正式依赖（对应 vcpkg#51743），新增 `ports/picohttpparser/` overlay port，移除 bundled 构建

### Performance
- **GenericConnection WriteEntry 去虚函数化**：内联 `shared_ptr<string>` 消除三层间接（shared_ptr→control block→WriteNode→虚函数），字段按访问频率重排 + `alignas(64)` 隔离 `lastActiveTimeMs_`
- **MemoryPool TrackedResource**：四个 atomic 计数器各自 `alignas(64)` 消除 false sharing，新增 `HICAL_ENABLE_MEMORY_TRACKING` 条件编译（关闭时零开销）
- **Middleware MiddlewareTimingStats**：热计数器 `alignas(64)` + 冷数据移至末尾

### Fixed
- **AsioEventLoop::stop() 数据竞争**：`quit_.exchange(true)` 一次性门卫，确保 `workGuard_.reset()` / `stop()` 只执行一次
- **TSan 检出的 3 处数据竞争**：`threadId_` 改 `std::atomic`；`AsioTimer::cancel()` post 到 executor 线程执行；`TcpServer::stop()` acceptor 关闭 post 到 `baseLoop_` 线程
- **`HICAL_ENABLE_MEMORY_TRACKING` CMake 选项缺失**：补充 `option(默认 ON)` + PUBLIC compile definition，修复 4 个依赖 stats 的测试断言失败
- **`WsDeflate.cpp` 缺少 `#include <cstdint>`**（PR #7）

## [2.6.1] - 2026-05-13

### Added
- **HTTP Date 头缓存**：`thread_local` 每秒更新一次，响应自动插入 RFC 7231 `Date` 头

### Changed
- **`hical_core` 强制静态库**：移除 `WINDOWS_EXPORT_ALL_SYMBOLS`，`add_library` 显式 STATIC
- **picohttpparser 条件集成**：新增 `HICAL_USE_SYSTEM_PICOHTTPPARSER` 开关，include 从 PUBLIC 改为 PRIVATE
- **tests/CMakeLists.txt 重构**：提取 `hical_add_test()` 函数消除重复样板
- **bench_main.cpp 精简为 TFB 专用**：仅 `/json` + `/plaintext`
- **Conan/vcpkg 构建完善**：Conan 新增 `with_openapi` 选项和宏透传，vcpkg port 新增 picohttpparser 系统依赖

## [2.6.0] - 2026-05-12

### Breaking Changes
- **移除 Boost.Beast 依赖**：HTTP 解析/序列化和 WebSocket 全部替换为自研实现，`native()` 返回自研 `NativeRequest` / `NativeResponse`（原 Beast 类型）
- **新增 zlib 依赖**：WebSocket permessage-deflate 压缩需要 zlib

### Added
- **原生 HTTP 栈**：集成 picohttpparser 替代 Beast HTTP parser（CPU 占比 10% → 0.08%），零拷贝 `NativeRequest`（`string_view` 引用连接级 `readBuf`，栈上 `RequestHeaders` 零堆分配），自研 `NativeResponse` 序列化（`FixedBuffer<512>` 栈缓冲 + scatter-gather I/O）
- **自研 WebSocket 栈**（RFC 6455 完整实现）：手写帧解析/构造（`WsFrame.h`）、握手协议（`WsHandshake.h`）、permessage-deflate 压缩（`WsDeflate.h/cpp`，pimpl 封装 zlib），`WebSocketSession` 完全重写为 raw socket 实现，支持消息分片重组 + 控制帧穿插
- **`HeaderMap` 头部容器**：`vector<pair>` 实现，大小写不敏感查找，延迟 reserve（默认构造零堆分配）
- **SO_REUSEPORT 多 acceptor**：每个 worker loop 独立 acceptor，accept 与 I/O 同线程零跨线程调度（Windows 自动回退单 acceptor）
- **连接级 atomic 时间戳超时**：替代 per-request timer，keep-alive 连接 `epoll_ctl` 调用降至 0
- **Chunked Transfer-Encoding 支持**（RFC 7230）
- **错误码统一映射**（`Error.h/cpp`）：36 个 `ErrorCode` + `NetworkError` 结构，隔离上层对 Asio 错误码的直接依赖
- **`coSpawn()` 扩展**：任意 executor 重载 + `logOnException` 替代 `detached`（未捕获异常输出到 stderr）
- **Docker 压测基础设施**：bench-server / bench-wrk Dockerfile + docker-compose 编排

### Changed
- **QPS 从 27K 提升至 159K**（+489%），与 Cinatra/Drogon 持平
- **`HttpServer` 多 acceptor 架构**：`acceptLoop` 参数化支持并发运行，`gracefulStop()` 改为串行化关闭所有 acceptor
- **热路径微优化**：header 按长度+首字符快速过滤、响应头 `insert()` O(1) 替代 `set()` O(N)、`attributes_` 延迟构造、`FixedBuffer` 从 4096 降至 512、200 OK 状态行预计算
- **RouteGroup 同步中间件链优化**：纯同步场景零协程帧开销（10 层同步中间件仅比无中间件低 2.1%）

### Fixed
- **readBuf 残留数据丢弃**：TCP 粘包场景下 keep-alive 请求解析错误，`bufUsed` 移到循环外修复

### Security
- **WebSocket 协议校验强化**：客户端掩码强制检查、RSV 位限制、控制帧大小限制（≤125B）、zip bomb 防护
- **HTTP 头部/Body 大小限制**：头部超限返回 431，Body 超限返回 413

## [2.5.2] - 2026-05-10

### Changed
- **Router 同步快速路径**：
  - 新增 `Router::dispatchSync()` 方法，同步注册的 handler 直接调用返回结果，跳过协程帧分配（~40-130ns/req）
  - 新增 `RouteEntry` 结构体（`asyncHandler` + 可选 `syncHandler`）替代裸 `RouteHandler` 存储
  - 新增 `Router::resolveRoute()` 内部方法，统一 URL 解码、路径深度校验、静态/参数路由查找、405 检测，`dispatch()` 和 `dispatchSync()` 共用，消除 ~40 行重复代码和 404/405 场景的 double-lookup
  - `dispatch()` 也优先检查 `syncHandler`，有值时 `co_return syncHandler(req)` 跳过 `co_await asyncHandler(req)`
  - 同步 handler 注册只存 `syncHandler`，不再创建 asyncHandler wrapper，消除 `std::function` 拷贝
  - `HttpSessionImpl.cpp` 无中间件路径先尝试 `dispatchSync()`，有值直接返回（零协程帧），`nullopt` 时 fallback 到 `co_await dispatch()`
- **GenericConnection 编译防火墙**：
  - 模板方法实现从 `GenericConnection.h`（~780 行）提取到 `GenericConnection.hci`，头文件仅保留声明 + `extern template` 声明
  - 显式实例化集中在 `GenericConnection.cpp`（`tcp::socket` + `ssl::stream<tcp::socket>`）
  - `.hci` 文件有 `#ifndef HICAL_BUILDING_GENERIC_CONNECTION` 防误包含守卫
- **HttpServer 编译防火墙**：
  - `handleSession()` + `handleWebSocket()` 从 `HttpServer.cpp` 移至 `HttpSessionImpl.cpp`，隔离 Beast HTTP parser/serializer 和 WebSocket 重模板代码
  - 修改 HttpServer 配置逻辑不再触发 Beast 模板重编译
- **MetaJson 错误抛出优化**：
  - `throw std::runtime_error(...)` 从模板代码提取为 `MetaJsonError.h/cpp` 中的 `[[noreturn]]` 非模板函数（`throwTypeMismatch` / `throwMissingField` / `throwParseError`）
  - 减少每个用户类型 `HICAL_JSON` 实例化的代码体积，改善 icache 利用率

### Fixed
- `SyncAfterHandler` 安全约束：禁止修改 body（仅允许 `setHeader`/`setCookie`），因 `Content-Length` 已由 `prepare_payload()` 固定，修改 body 会导致 HTTP Content-Length 不一致

## [2.5.1] - 2026-05-08

### Added
- **SyncMiddleware 快速路径**（零协程帧中间件）：
  - 新增 `SyncBeforeHandler` / `SyncAfterHandler` / `SyncMiddlewareResult` 类型（`Middleware.h`）
  - 新增 `MiddlewareEntry` 统一存储结构（Async/Sync 标签联合）
  - `MiddlewarePipeline` 新增 3 个同步 `use()` 重载，`RouteGroup` 新增 2 个
  - `buildOptimizedChain()` 算法：连续 Sync 条目合并为单个协程帧，N 层同步中间件仅 1 次协程帧堆分配
- `MemoryPool::threadLocalPool()` 公共方法，暴露 thread-local 无锁池资源指针

### Changed
- **HttpServer 多 io_context 架构**：
  - `ioContext_` 替换为 `AsioEventLoop baseLoop_` + `EventLoopPool ioPool_`（1 Thread : 1 io_context）
  - `acceptLoop()` 去除 `make_strand`，round-robin 分发到 worker loop（单线程天然串行）
  - accept 后立即设置 `TCP_NODELAY` 减少 Nagle 延迟
  - `stop()` / `gracefulStop()` 改为停止所有 loop（baseLoop + ioPool）
- `handleSession()` 请求级 PMR 池改为栈上 `monotonic_buffer_resource` + thread-local 无锁池作 upstream，消除 `make_unique` 堆分配和同步锁竞争
- `MiddlewarePipeline::build()` / `buildFor()` 非 profiling 路径改用 `buildOptimizedChain(entries_)`
- `MiddlewarePipeline::size()` 改为返回 `entries_.size()`（支持纯 Sync 中间件计数）
- 新增 `HttpServer::recommendedMaxConnections(availableMemoryMB)` 静态方法：按 25KB/连接 估算并预留 30% 内存，上限 65535，供使用者根据机器内存自行设置 `maxConnections`（默认值仍为 10000）
- `PoolConfig::requestPoolInitialSize` 默认值从 8192 降至 4096
- `handleSession()` 主路径去除冗余 `prepare_payload()` 调用（已由 `setBody()` / `setJsonBody()` 内部完成）
- **RouteGroup 中间件预构建优化**：`RouteGroup` 同步分支接入 `buildOptimizedChain()`，组级 Sync 中间件同样享受零协程帧合并

## [2.5.0] - 2026-05-05

### Added
- **日志系统**（6 级日志，零开销设计）：
  - `Log.h/cpp` — `Logger` 单例 + 6 级 `LogLevel`（Trace/Debug/Info/Warn/Error/Fatal），`std::format` 风格 API（`HICAL_LOG_INFO("port={}", 8080)`）、流式 API（`HICAL_LOG_INFO_STREAM << val`）、条件宏（`HICAL_LOG_INFO_IF`）、结构化字段 API（`HICAL_LOG_INFO_F`），NDEBUG 下 TRACE 编译期消除，`thread_local` 时间戳/线程 ID 缓存，可配置 flush 级别，Fatal 自动 abort
  - `LogRecord.h` — 结构化日志条目（level/timestamp/threadId/file/line/message + `boost::json::object` fields + traceId）
  - `LogFormatter.h/cpp` — 格式化器接口 + `TextFormatter`（`thread_local` 时间戳缓存）+ `JsonFormatter`（JSON Lines，UTC 时间戳）
  - `LogSink.h/cpp` — 可插拔输出后端接口 + `StderrSink`（fprintf）+ `FileSink`（同步 fwrite + LogFile 轮转）+ `OStreamSink`（线程安全 ostream 包装）
  - `LogFile.h/cpp` — 日志文件轮转引擎（按大小轮转默认 100MB、最大文件数限制、时间戳序列命名、严格文件名匹配清理）
  - `AsyncFileSink.h/cpp` — 异步双缓冲文件 Sink（`std::jthread` + `stop_token` 后台线程、4MB 前后缓冲交换、背压保护丢弃 + 计数、1 秒超时刷盘、优雅关闭含最终缓冲排空）
  - `FixedBuffer.h` — 栈上固定缓冲区模板（默认 4KB），`std::to_chars` 整数/浮点格式化，溢出自动 fallback 到堆
  - `LogChannel.h/cpp` — 命名日志通道（独立 level/formatter/sinks），`LogChannelRegistry`（`shared_mutex` 读多写少），`HICAL_LOG_TO` 通道路由宏
  - `LogMiddleware.h/cpp` — 洋葱模型日志中间件（OpenSSL RAND_bytes 128 位 trace-id 自动生成，结构化访问日志到命名通道）
  - `LogAdmin.h/cpp` — 动态日志级别管理端点（`GET /admin/log-level` 查询 + `PUT /admin/log-level` 运行时调整）
  - 9 个测试文件（97 个用例）：test_log / test_log_ndebug / test_fixed_buffer / test_log_file / test_async_file_sink / test_log_formatter / test_log_channel / test_log_middleware / test_log_admin
- **OpenAPI 3.0 自动生成模块**（`HICAL_WITH_OPENAPI=ON`，默认开启，零新依赖）：
  - `OpenApiSchema.h` — 从 `HICAL_JSON` 宏自动生成 JSON Schema（基本类型/vector/嵌套结构体/$ref），`HICAL_SCHEMA_NAME` 注册类型名，`collectSchemas<T>()` 递归收集
  - `OpenApiRegistry.h/cpp` — 路由元数据注册表（`HICAL_API()` 综合标注宏 + `builder::*` 辅助函数、`HICAL_ROUTES_WITH_API()` 增强版路由收集、`registerRoutesWithOpenApi()` 同时注册路由和元数据）
  - `OpenApiDocument.h/cpp` — 文档组装（惰性生成 + 缓存、自动路径参数提取、同路径不同 method 合并为同一 Path Item）
  - `OpenApiEndpoint.h` — `serveOpenApi()` 一键注册 `/openapi.json` + `/docs`（Swagger UI CDN），`boost::json::serialize()` 安全转义防 JS 注入
  - 1 个测试文件（35 个用例）：test_openapi
  - 1 个示例：examples/openapi_server.cpp

## [2.4.0] - 2026-05-01

### Added
- **CORS 中间件**：
  - `Cors.h` — `makeCorsMiddleware(CorsOptions{...})`，支持 Origin 通配符/精确匹配、Preflight OPTIONS 自动响应、`Vary: Origin` 缓存提示、凭证模式下禁止通配符安全校验、可配置 maxAge
- **路由组（Route Group）**：
  - `RouteGroup.h/cpp` — `router.group("/api/v1")` 前缀分组，支持组级中间件继承、多层嵌套、镜像 Router API（get/post/put/del/route）
- **HTTP 核心增强**：
  - `HttpRequest` 新增查询参数解析 API（`queryParam()`/`queryParams()`）和表单参数解析 API（`formParam()`/`formParams()`）
  - `HttpResponse` 新增 `redirect()` 便捷方法
  - `HttpServer` 新增全局错误处理器 `setErrorHandler()`，优雅关闭 `shutdown()` 支持
  - 7 个测试文件：test_query_params / test_form_params / test_redirect / test_cors / test_route_group / test_error_handler / test_graceful_shutdown

## [2.3.0] - 2026-04-30

### Added
- **数据库中间件**（`HICAL_WITH_DATABASE=ON`，默认 OFF）：
  - `DbConfig` — 数据库连接配置（池大小、超时、健康检查、PreparedStatement 缓存容量）
  - `DbResult` — 统一查询结果集（string-based 列/行、affectedRows、insertId、columnIndex）
  - `DbConnection` — 数据库连接纯虚接口（参数化 query/execute、事务控制、健康检查，deprecated 非参数化重载）
  - `DbConnectionPool` — 协程化连接池（steady_timer 协程信号量、LIFO 复用、后台 healthCheck/idleCheck、pingGracePeriod 优化、事务残留自动回滚）
  - `DbMiddleware` — HTTP 中间件集成（makeDbMiddleware 连接注入 + autoTransaction 自动提交/回滚、getDbConnection/getDbPool 辅助函数）
  - `DbQueryLog` — 查询日志中间件（装饰器模式 LoggingDbConnection、慢查询检测、onRequestComplete 回调）
  - `MysqlConnection` — MySQL 后端（Boost.MySQL any_connection、完整类型转换、PreparedStatement 失效重试、validateCharset 白名单防注入）
  - `StmtCache` — 每连接 LRU PreparedStatement 缓存（透明哈希 string_view 查找、淘汰 statement 返回调用方异步 close）
  - 5 个测试文件（42 个用例）：test_db_pool / test_db_middleware / test_db_query_log / test_stmt_cache / test_mysql_integration

## [2.2.0] - 2026-04-28

### Breaking Changes
- **Boost 最低版本**：1.78 → 1.85（DB 中间件依赖 `Boost.MySQL` 1.84+ 的 `any_connection` + `Boost.Charconv` 1.85+）；框架核心仍为 >= 1.82

### Added
- **内存池 GC**：`MemoryPool::gc(maxIdleSeconds)` 遍历线程本地池，空闲超时的池由拥有线程延迟执行 `release()`（避免跨线程操作 `unsynchronized_pool_resource` 的 UB）；`HttpServer::setGcInterval()` + `gcLoop()` 协程定时触发；`Stats` 扩展 `threadPoolCount` / `gcCycles` / `gcReclaimedPools` 字段
- **中间件 Profiling**：编译选项 `HICAL_ENABLE_MIDDLEWARE_PROFILING`，开启后记录每层中间件调用次数、总耗时、平均/最大/最小耗时（原子 CAS 更新，lock-free）；`HttpServer::middlewareStats()` 获取快照
- **命名中间件**：`MiddlewarePipeline::use(name, handler)` / `HttpServer::use(name, handler)`，Profiling 报告中显示中间件名称
- **WebSocket permessage-deflate 压缩**：`WsOptions` 新增 `enableCompression` / `serverMaxWindowBits` / `clientMaxWindowBits` / `serverNoContextTakeover` 配置；`WsCompressionConfig` 结构体传入 `WebSocketSession`
- **PmrBufferWriteNode**：零拷贝持有 `PmrBuffer`（move 语义），消除 `PmrBuffer → string` 数据拷贝；`WriteNode` 基类新增 `asBuffer()` 虚方法
- **协程异常日志**：`coSpawn()` 默认使用 `logOnException` 回调替代 `detached`，未捕获异常输出到 stderr
- **Cookie 属性 CRLF 防护**：`setCookie()` 的 `path` / `domain` / `sameSite` 属性新增 CR/LF 注入检查
- **URL 解码 NULL 字节防御**：`Router::urlDecode()` 跳过 `%00`，防止 C API 路径截断攻击
- **DB 中间件基础设施**（本次未发布代码，仅构建系统支持）：`HICAL_WITH_DATABASE` CMake 开关（默认 OFF）、vcpkg/Conan `database` feature、`hicalConfig.cmake.in` 自动传递 `charconv` 依赖

### Changed
- **Session 读写锁细化**：`Session::mutex_` 从 `std::mutex` 升级为 `std::shared_mutex`，`get()` / `has()` / `isDirty()` 使用 `shared_lock`；`get<T>()` 用 `typeid` 比较替代 `try-catch bad_any_cast`
- **WebSocket 读缓冲区复用**：`WebSocketSession::readBuffer_` 跨 `receive()` 调用复用，避免每次堆分配
- **WebSocket 超时 timer 生命周期修正**：timer 声明移到 try 块外，catch 块取消 timer；新增 `wsAlive` 原子标志 + RAII 守卫，防止 timer 回调访问已析构 session
- **GenericConnection 批量发送**：统一通过 `asBuffer()` 多态调用，消除 `static_cast<MemoryWriteNode&>` 向下转型
- **Router 参数路由**：`ParamList` 提到匹配循环外复用；`findWsRoute()` 参数 `string` → `string_view`
- **StaticFiles PathCache**：引入 LRU 淘汰策略（`std::list` + `unordered_map`）
- **Cookie 编码优化**：`setCookie()` 从 `ostringstream` 改为直接字符串拼接 + 查表法百分号编码
- **MetaRoutes 宏重构**：`HICAL_ROUTES` 从硬编码 `E_1` ~ `E_16` 改为 `__VA_OPT__` 递归展开（支持最多 243 个路由），代码量大幅缩减

### Security
- Cookie `Set-Cookie` 属性字段（path/domain/sameSite）CRLF 注入防护
- URL 解码 `%00` NULL 字节截断防护
- WebSocket timer 生命周期修正（消除 use-after-free 竞态）

## [2.1.0] - 2026-04-24

### Breaking Changes
- **HttpRequest 返回类型收紧**：`header()` 返回 `string_view`（原 `string`）、`cookie()` / `param()` 返回 `const string&`（原 `string`）、`contentType()` 返回 `string_view`、`jsonBody()` 返回 `const json::value&`（原值拷贝）——大多数调用点源码兼容，仅存储到 `auto` 的场景可能需确认
- **serveStatic 异步化**：返回类型从 `function<HttpResponse(...)>` 改为 `function<Awaitable<HttpResponse>(...)>`，调用方需在协程中 `co_await`
- **Boost 最低版本**：1.70 → 1.78（`random_access_file` 依赖）
- **Linux 新增依赖**：`liburing-dev`（Boost.Asio `random_access_file` 在 Linux 上依赖 io_uring）

### Added
- **文件异步发送**：`TcpConnection::sendFile(path, offset, length)` + `WriteNode` 多态写队列（`MemoryWriteNode` / `FileWriteNode`），支持 `random_access_file` 异步读取 + 64KB 分块发送
- **macOS 平台回退**：`GenericConnection::sendFileNode()` 和 `serveStatic()` 在无 `BOOST_ASIO_HAS_FILE` 平台自动回退到 `std::ifstream` 同步读取
- **WebSocket Origin 白名单**：`Router::ws()` 新增 `WsOptions` 重载，支持 `allowedOrigins` 集合，不在白名单内的 Origin 返回 403（CSWSH 防护）
- **WebSocket 中间件链**：WS 升级请求经过预构建的中间件链，中间件可返回非 200 阻止升级（认证/限流）
- **WebSocket 空闲超时**：复用 `idleTimeout_` 设置，超时无消息的 WS 连接自动断开
- **空闲连接超时清理**：`TcpServer::setIdleTimeout(seconds)` + `idleCheckLoop()` 协程定期扫描，断开超时连接
- **fd 耗尽防护**：`IdleFd` 类（POSIX 预留 `/dev/null` fd），EMFILE 时临时释放→accept→close→重新预留，避免 accept 循环忙转
- **Session 重建**：`SessionManager::regenerate(oldId)` 生成新 ID 并迁移数据，旧 ID 失效（Session 固定攻击防护）
- **Session 数据迁移**：`Session::migrateFrom(other)` 原子迁移数据，地址序双锁防死锁
- **MetaJson 装饰器**：C++20 宏路径新增 `ALIAS(field, "key")`、`REQUIRED(field)`、`REQUIRED_ALIAS(field, "key")`、`HICAL_IGNORE(field)`，`__VA_OPT__` 递归展开无字段数上限
- **MetaJson C++26 属性**：`[[hical::json_name("alias")]]`、`[[hical::json_required]]`、`[[hical::json_ignore]]` + `jsonSchema<T>()` / `toJsonSnakeCase<T>()`
- **MetaJson unsigned 支持**：`valueToJson()` / `valueFromJson()` 正确处理 `uint64_t`，防止大无符号数据丢失
- **中间件预构建 API**：`MiddlewarePipeline::buildFor(finalHandler)` 返回可缓存的 `MiddlewareNext`
- **Multipart 双 API**：新增 `getFile(parts, fieldName)` / `getField(parts, fieldName)` 重载，搜索预解析结果避免重复解析
- **HTTP Header 注入防护**：`HttpRequest::setHeader()` 和 `HttpResponse::setHeader()` 拒绝含 CR/LF 的头部名/值
- **SSL 懒包含**：`SslConnection.h` 独立类型别名头文件，非 SSL 场景不拉 OpenSSL 头文件
- **Session 测试补充**：`RegenerateSession`、`RegenerateNonExistent`、`MigrateFromSession`、`RegenerateConcurrent` 4 个测试

### Changed
- **Session 读写锁**：`SessionManager::mutex_` 从 `std::mutex` 升级为 `std::shared_mutex`，`find()` 使用 `shared_lock` 提升读并发
- **Session 懒 GC**：`find()` 不再立即删除过期条目（避免 shared→unique 锁升级竞态），过期条目由 `gc()` 定期清理；`create()` 达上限时先强制 GC 再拒绝
- **路由参数分组**：参数路由从全局 `vector` 改为 `unordered_map<HttpMethod, vector>`，dispatch 仅扫描匹配方法的子集
- **路径 DoS 防护**：单遍扫描同时计算 URL 解码需求和段深度，>256 段早期拒绝（`hMaxPathSegments`）
- **连接存储优化**：`TcpServer` 连接集合从 `set` 改为 `unordered_set`（O(1) 插入/删除）
- **连接活跃时间**：`GenericConnection` 新增 `lastActiveTimeMs_` 原子字段，读写循环更新
- **HttpRequest path params**：存储从 `unordered_map` 改为 `vector<pair>`，小参数集更少分配
- **jsonBody() 缓存**：多次调用只解析一次，后续返回缓存引用
- **PmrBuffer 自适应**：`retrieveAll()` 超过 2× 初始容量时自动缩容；`ensureWritableBytes()` 改为 2× 指数增长
- **StaticFiles 路径缓存**：`PathCache`（4096 条目 / 60s TTL）避免每请求 `canonical()` 系统调用
- **Multipart toLowerInPlace**：头部键原地小写，消除临时 string 拷贝
- **GenericConnection 写队列**：从 `deque<shared_ptr<string>>` 改为 `deque<shared_ptr<WriteNode>>`，统一内存/文件节点

### Security
- WebSocket Origin 白名单（CSWSH 防护）
- HTTP Header CR/LF 注入防护（Response Splitting 防护）
- Session `regenerate()` 防 Session 固定攻击
- 路径段深度限制防 DoS

## [2.0.0] - 2026-04-19

### Fixed
- **[P0] Middleware 悬空引用**：`build()` 和 `execute()` 中 lambda 按引用捕获 `middlewares_[i]` 改为按值捕获，防止协程帧中 use-after-free
- **[P0] HttpServer timer 竞态**：超时 `steady_timer` 移到循环外复用，引入 `shared_ptr<atomic<bool>>` 存活标志 + RAII 守卫，消除 timer 回调访问已销毁 socket 的竞态
- **[P0] GenericConnection 数据竞争**：`reading_` 从 `bool` 改为 `std::atomic<bool>`，修复 `stopRead()` 跨线程写入与 `readLoop()` 读取之间的数据竞争
- **[P0] TcpServer acceptLoop use-after-this**：协程 lambda 捕获 `alive_` 标志，循环条件和 `co_await` 恢复后均检查存活性，防止析构后访问 `this`
- **[P1] Multipart DoS 检查位置**：Part 数量上限检查从 `push_back` 之后移到之前，避免先分配后丢弃

### Changed
- **Middleware 重构**：提取公共 `buildChain()` 方法消除 `build()`/`execute()` 逻辑重复；新增无参 `execute(HttpRequest&)` 重载走缓存路径，双参 `execute(req, finalHandler)` 始终动态构建
- **Session ID 生成**：从 `std::mt19937_64`（伪随机）改为 `OpenSSL RAND_bytes`（密码学安全），hex 编码改为查表法消除 `ostringstream` 开销
- **Cookie 安全默认值**：`CookieOptions` 默认 `httpOnly=true`、`secure=true`、`sameSite="Lax"`；`SessionOptions::secure` 同步改为 `true`
- **Session 嵌套锁消除**：`Session::lastAccess_` 从 `chrono::time_point`（mutex 保护）改为 `atomic<int64_t>` 纳秒时间戳，`touch()` / `lastAccess()` 无锁操作
- **send(PmrBuffer&&) 语义修复**：改为调用 `buffer.readAll()` 走 `send(std::string&&)` 的 move 通道，不再退化为拷贝
- **Router urlDecode 快速路径**：先扫描路径是否含 `%`/`+`，无编码字符时跳过 `urlDecode` 分配；`RouteKey` 引入透明哈希（`RouteKeyView` + `is_transparent`），`staticRoutes_.find()` 直接用 `string_view` 查找，消除每请求的 `std::string` 堆分配
- **HttpServer 中间件调用**：已 `build()` 场景改用无参 `execute(req)`，每请求省去一次 `std::function` 堆分配

### Added
- `SessionOptions::maxSessions`（默认 100000）：Session 存储上限，`create()` 达到上限时返回 `nullptr`，中间件返回 503

## [1.0.1] - 2026-04-12

### Fixed
- `INSTALL_INTERFACE` include 路径修正为 `include/hical/core` 和 `include/hical/asio`，修复 `find_package` 后头文件找不到的问题
- Windows 下 `ws2_32`/`mswsock` 统一移至 `hical_core` 目标 `PRIVATE` 链接，消费者无需手动添加

### Changed
- 移除根 `CMakeLists.txt` 中全局 `include_directories`，改为完全依赖 `target_include_directories` 传递

### Added
- 新增 `HICAL_BUILD_TESTS` / `HICAL_BUILD_EXAMPLES` CMake 选项（默认 ON），作为库分发时可关闭以加快构建
- `GTest` 改为按需查找，仅 `HICAL_BUILD_TESTS=ON` 时才 `find_package`
- 新增 `hical::hical_core` ALIAS 目标，方便消费者使用命名空间形式链接
- 新增 `ports/hical61-hical/` vcpkg overlay port（`portfile.cmake`、`vcpkg.json`、`usage`）
- 新增 `docs/integration_guide.md`：vcpkg overlay、FetchContent、cmake install 三种集成方式说明

## [1.0.0] - 2026-04-12

首次公开发布。

### Added

**网络层 & 异步核心**
- Boost.Asio/Beast 异步网络层，协程 I/O（`co_await` + `boost::asio::use_awaitable`）
- `EventLoopPool` 多线程池（1 线程 : 1 io_context，轮询分发）
- SSL/TLS 支持（模板化 `GenericConnection<ssl::stream<tcp::socket>>`，编译期分支）

**HTTP 框架**
- 静态路由（hash map O(1)）+ 参数路由（`{id}` 模式）+ WebSocket 路由
- 洋葱模型中间件管线（`MiddlewarePipeline`）
- `HttpServer` 一键启动门面

**内存 & 性能**
- 三层 PMR 内存策略：全局同步池 / 线程本地池 / 请求级单调缓冲
- `PmrBuffer` 统一缓冲区（prepend 区域 + 自动扩容）

**类型系统**
- C++20 Concepts 编译期约束（`EventLoopLike`、`TcpConnectionLike`、`TimerLike`、`NetworkBackend`）

**C++26 反射层（双轨设计）**
- `Reflection.h` — 特性检测 `HICAL_HAS_REFLECTION`，`RouteInfo`，类型萃取
- `MetaJson.h` — 自动 JSON 序列化/反序列化（`toJson` / `fromJson` / `req.readJson<T>()`）
- `MetaRoutes.h` — 自动路由注册（`HICAL_HANDLER` / `HICAL_ROUTES` 宏 + `registerRoutes()`）

**Cookie / Session / 静态文件 / 文件上传**
- Cookie：RFC 6265 兼容解析（懒解析 + 缓存，first-wins 语义），`HttpResponse::setCookie` 含 CRLF 注入防护
- 静态文件服务：`serveStatic` 工厂函数，27 种 MIME 类型、ETag/304 缓存、路径遍历防护、64 MB 大小限制
- Multipart 文件上传：RFC 7578 `multipart/form-data` 解析，Part 数量上限 256（DoS 防护）
- Session 会话管理：内存 `SessionManager`，懒 GC，128 位随机 ID，线程安全 `Session`，`makeSessionMiddleware` 中间件工厂
- `HttpRequest` 请求级属性存储（`setAttribute` / `getAttribute<T>`）
- `HttpTypes.h` 新增 `hPayloadTooLarge = 413` 状态码

**工程**
- GitHub Actions CI（GCC 14 / Clang 20 / MSYS2 MINGW64 / MSVC + vcpkg 四平台矩阵）
- 232 个测试用例（GTest + CTest 集成），clang-format / clang-tidy 检查
- MSVC + vcpkg 支持（`vcpkg.json` 清单）
- 社区基础设施：`CONTRIBUTING.md`、`CODE_OF_CONDUCT.md`、`SECURITY.md`、PR/Issue 模板

### Security

- Cookie `Set-Cookie` 头 CRLF 注入防护
- 静态文件路径遍历防护
- Multipart Part 数量上限（DoS 防护）
- Session ID 使用密码学安全的随机数生成

[Unreleased]: https://github.com/Hical61/Hical/compare/v2.6.5...HEAD
[2.6.5]: https://github.com/Hical61/Hical/compare/v2.6.4...v2.6.5
[2.6.4]: https://github.com/Hical61/Hical/compare/v2.6.3...v2.6.4
[2.6.3]: https://github.com/Hical61/Hical/compare/v2.6.2...v2.6.3
[2.6.2]: https://github.com/Hical61/Hical/compare/v2.6.1...v2.6.2
[2.6.1]: https://github.com/Hical61/Hical/compare/v2.6.0...v2.6.1
[2.6.0]: https://github.com/Hical61/Hical/compare/v2.5.2...v2.6.0
[2.5.2]: https://github.com/Hical61/Hical/compare/v2.5.1...v2.5.2
[2.5.1]: https://github.com/Hical61/Hical/compare/v2.5.0...v2.5.1
[2.5.0]: https://github.com/Hical61/Hical/compare/v2.4.0...v2.5.0
[2.4.0]: https://github.com/Hical61/Hical/compare/v2.3.0...v2.4.0
[2.3.0]: https://github.com/Hical61/Hical/compare/v2.2.0...v2.3.0
[2.2.0]: https://github.com/Hical61/Hical/compare/v2.1.0...v2.2.0
[2.1.0]: https://github.com/Hical61/Hical/compare/v2.0.0...v2.1.0
[2.0.0]: https://github.com/Hical61/Hical/compare/v1.0.1...v2.0.0
[1.0.1]: https://github.com/Hical61/Hical/compare/v1.0.0...v1.0.1
[1.0.0]: https://github.com/Hical61/Hical/releases/tag/v1.0.0
