# Hical 项目代码结构

> 最后更新：2026-04-24

## 项目概述

Hical 是一个基于 Boost.Asio，利用 C++26 反射和 pmr 内存池构建高性能的现代 C++ Web 框架

## 目录结构

```
hical/
├── CMakeLists.txt              # 顶层 CMake 配置（C++20，依赖 Boost、GTest、OpenSSL）
├── .clang-format               # 代码格式化配置
├── .clang-tidy                 # 静态分析配置
│
├── src/                        # 框架源码
│   ├── CMakeLists.txt          # 构建 hical_core 静态库
│   ├── core/                   # 核心抽象层（接口 + 基础设施 + HTTP 框架）
│   │   ├── EventLoop.h         # 事件循环抽象基类
│   │   ├── TcpConnection.h     # TCP 连接抽象基类（含 sendFile/lastActiveTime 虚方法）
│   │   ├── Timer.h             # 定时器抽象基类
│   │   ├── Concepts.h          # C++20 Concepts 约束
│   │   ├── Error.h/.cpp        # 错误码枚举 + boost error_code 转换
│   │   ├── MemoryPool.h/.cpp   # pmr 内存池管理器（TrackedResource + PoolConfig + thread_local）
│   │   ├── PmrBuffer.h         # 基于 pmr 的统一缓冲区（指数增长 + 自适应缩容）
│   │   ├── InetAddress.h/.cpp  # 网络地址封装（IPv4/IPv6）
│   │   ├── SslContext.h/.cpp   # SSL/TLS 上下文配置封装
│   │   ├── Coroutine.h         # 协程工具（Awaitable/sleep/coSpawn，header-only）
│   │   ├── HttpTypes.h         # HTTP 方法/状态码枚举 + 字符串转换
│   │   ├── HttpRequest.h/.cpp  # HttpRequest 封装（string_view 返回类型，jsonBody 缓存）
│   │   ├── HttpResponse.h/.cpp # HttpResponse 封装 + 工厂方法（setHeader CR/LF 防护）
│   │   ├── Cookie.h            # CookieOptions 结构体
│   │   ├── Router.h/.cpp       # 路由器（哈希表静态路由 + 按方法分桶参数路由 + WebSocket + WsOptions）
│   │   ├── Middleware.h/.cpp   # 中间件系统（洋葱模型管道 + buildFor 预构建）
│   │   ├── HttpServer.h/.cpp   # HTTP 服务器（Router + Middleware + WS 中间件预构建 + fd 耗尽防护）
│   │   ├── WebSocket.h/.cpp    # WebSocket 会话封装（beast::websocket）
│   │   ├── StaticFiles.h       # 静态文件服务（异步 I/O + PathCache + ETag/304）
│   │   ├── Multipart.h/.cpp    # multipart/form-data 解析（dual API: req 版 + parts 版）
│   │   ├── Session.h/.cpp      # Session 会话管理（shared_mutex + regenerate + migrateFrom）
│   │   ├── IdleFd.h            # 空闲 fd 预留（POSIX /dev/null，Windows no-op）
│   │   ├── WriteNode.h         # 多态写队列节点（MemoryWriteNode / FileWriteNode）
│   │   ├── Reflection.h        # C++26 反射特性检测 + RouteInfo + 类型萃取
│   │   ├── MetaJson.h          # 自动 JSON 序列化（ALIAS/REQUIRED/IGNORE 装饰器）
│   │   ├── MetaRoutes.h        # 自动路由注册（HICAL_HANDLER/HICAL_ROUTES 宏）
│   │   └── Version.h.in        # CMake 配置版本头（唯一版本号来源）
│   │
│   └── asio/                   # Boost.Asio 适配实现
│       ├── AsioEventLoop.h/.cpp      # 基于 io_context 的事件循环
│       ├── GenericConnection.h/.cpp  # 模板化连接（TCP/SSL 统一，WriteNode 写队列，sendFile 异步文件发送）
│       ├── SslConnection.h           # SSL 连接类型别名（懒包含 OpenSSL）
│       ├── AsioTimer.h/.cpp          # 基于 steady_timer 的定时器
│       ├── EventLoopPool.h/.cpp      # 多线程事件循环池（1 Thread : 1 io_context）
│       └── TcpServer.h/.cpp          # TCP 服务器（accept + 连接管理 + 空闲超时 + IdleFd 防护）
│
├── tests/                      # 单元测试（Google Test）
│   ├── CMakeLists.txt          # 测试构建配置
│   ├── test_basic.cpp          # 基础测试
│   ├── test_error.cpp          # 错误码转换测试
│   ├── test_asio_event_loop.cpp # AsioEventLoop 测试
│   ├── test_asio_tcp_connection.cpp # TcpConnection 测试
│   ├── test_memory_pool.cpp    # MemoryPool + PmrBuffer 测试（含 TrackedResource）
│   ├── test_asio_timer.cpp     # AsioTimer 测试
│   ├── test_ssl_connection.cpp # SSL 连接测试
│   ├── test_coroutine.cpp      # 协程工具测试
│   ├── test_http_types.cpp     # HTTP 类型测试
│   ├── test_router.cpp         # 路由器测试（含路径参数）
│   ├── test_router_perf.cpp    # 路由性能基准测试（静态/参数/大量路由）
│   ├── test_tcp_server.cpp     # TcpServer + EventLoopPool 测试
│   ├── test_middleware.cpp     # 中间件测试（洋葱模型/拦截）
│   ├── test_http_server.cpp    # HttpServer 集成测试
│   ├── test_websocket.cpp      # WebSocket 测试
│   ├── test_concepts.cpp       # C++20 Concepts 编译期约束测试
│   ├── test_reflection.cpp     # MetaJson + MetaRoutes 反射层测试（35 个）
│   ├── test_cookie.cpp         # Cookie 解析与 Set-Cookie 测试
│   ├── test_static_files.cpp   # 静态文件服务 / ETag / 路径遍历测试
│   ├── test_multipart.cpp      # multipart/form-data 解析测试
│   ├── test_session.cpp        # Session 生命周期 / 线程安全 / regenerate 测试
│   └── test_integration.cpp    # 完整 HTTP 请求/响应周期集成测试
│
├── examples/                   # 示例程序
│   ├── CMakeLists.txt          # 示例构建配置
│   ├── echo_server.cpp         # 协程式 Echo Server
│   ├── pmr_poc.cpp             # pmr 内存池验证
│   ├── benchmark.cpp           # Echo Server 压力测试工具
│   ├── http_server.cpp         # HTTP Server 示例（HttpServer API + WebSocket + 中间件）
│   ├── http_benchmark.cpp      # HTTP 基准测试工具（QPS/P50/P99 延迟统计）
│   └── pmr_benchmark.cpp       # pmr 内存池基准测试（多策略性能对比）
│
├── docs/                       # 文档
│   ├── project_structure.md    # 本文件 — 项目代码结构说明
│   ├── build_and_test_guide.md # 编译与测试指南
│   ├── api_reference.md        # 完整的 hical 框架公共 API 说明
│   ├── quickstart.md           # Hical 快速上手指南（5 分钟入门）
│   ├── examples_guide.md       # 使用示例（8 个由浅入深的完整示例）
│   ├── architecture.md         # 架构设计文档（PMR 内存池/反射层/Concepts 等）
│   └── performance_report.md   # 性能测试报告（基准测试方法与调优指南）

```

## 核心组件关系

```
┌─────────────────────────────────────────────────────┐
│                    HttpServer                        │
│  ┌──────────┐  ┌───────────────┐  ┌──────────────┐ │
│  │  Router   │  │  Middleware   │  │  WebSocket   │ │
│  │  {param}  │  │  Pipeline    │  │  Session     │ │
│  └──────────┘  └───────────────┘  └──────────────┘ │
│         │              │                   │         │
│  ┌──────┴──────────────┴───────────────────┘        │
│  │     HttpRequest / HttpResponse (Beast)           │
│  └──────────────────────────────────────────────────│
└─────────────────────────────────────────────────────┘
                         │
┌─────────────────────────────────────────────────────┐
│                    TcpServer                         │
│  ┌──────────────┐  ┌───────────────────────────┐   │
│  │ EventLoopPool│  │ GenericConnection<Socket>  │   │
│  │ (IO threads) │  │  PlainConnection / SslConn │   │
│  └──────────────┘  └───────────────────────────┘   │
└─────────────────────────────────────────────────────┘
                         │
┌─────────────────────────────────────────────────────┐
│              Core Infrastructure                     │
│  EventLoop / Timer / MemoryPool / PmrBuffer         │
│  SslContext / Error / InetAddress / Coroutine        │
└─────────────────────────────────────────────────────┘
```

## 构建系统

- **编译标准**: C++20（C++26 反射待编译器支持后启用）
- **构建工具**: CMake 3.20+
- **依赖库**:
  - Boost 1.78+（system, json; Asio/Beast header-only）
  - OpenSSL 3.x（SSL/TLS 支持）
  - Google Test（单元测试）
  - ws2_32, mswsock（Windows 网络库）
- **构建产物**:
  - `hical_core` — 框架核心静态库
  - `test_*` — 各组件单元测试（22 个测试套件）
  - `echo_server` / `pmr_poc` / `benchmark` / `http_server` — 示例程序
  - `http_benchmark` / `pmr_benchmark` — 性能基准测试工具

## 已完成阶段

### 阶段一：前期准备与技术验证
- 环境搭建（C++20, Boost, GTest, CMake）
- Echo Server PoC（协程式异步）
- pmr 内存池 PoC

### 阶段二：适配层核心设计
- **统一内存池**: MemoryPool（全局同步池 + 线程本地池 + 请求级单调池）+ PmrBuffer（pmr 统一缓冲区）
- **抽象接口层**: EventLoop / TcpConnection / Timer 纯虚基类 + Concepts 约束
- **AsioEventLoop**: 1 Thread : 1 io_context，dispatch/post，定时器管理
- **GenericConnection**: 协程式 readLoop/writeLoop，WriteNode 多态写队列（内存/文件），sendFile 异步发送，高水位回调（PlainConnection / SslConnection）
- **AsioTimer**: 基于 steady_timer 的单次/周期定时器
- **错误码体系**: ErrorCode 枚举 + boost error_code 转换（含 SSL 错误预留）
- **InetAddress**: IPv4/IPv6 地址封装，跨平台

### 阶段三：SSL/TLS、协程与反射 API 包装层
- **GenericConnection 模板化**: TCP/SSL 统一连接实现 + SSL 握手协程
- **SslContext**: SSL 上下文配置（证书/私钥/CA/验证模式）
- **协程工具（Coroutine.h）**: Awaitable<T> 别名、sleep()、coSpawn() 封装
- **HTTP 类型体系**: HttpMethod/HttpStatusCode 枚举 + 字符串转换
- **HttpRequest/HttpResponse**: Beast HTTP 请求/响应的 hical 风格封装
- **Router**: 路由注册 + 协程分发 + HICAL_ROUTE 宏

### 阶段四：hical 框架层构建
- **EventLoopPool**: 多线程事件循环池（1 Thread : 1 io_context，round-robin 分发）
- **TcpServer**: 协程式 accept 循环、连接管理、IO 线程分发、SSL 支持、优雅关闭
- **中间件系统**: 洋葱模型管道（MiddlewarePipeline），支持前置/后置逻辑和拦截
- **路由增强**: 路径参数 `{param}` 匹配和提取（`/users/{id}` -> `req.param("id")`）
- **HttpServer**: 高层封装，整合 Router + Middleware + Beast HTTP 读写 + Keep-Alive
- **WebSocket**: 基于 beast::websocket 的会话封装（send/receive/close），Router 注册 ws 路由
- **HTTP Server 示例**: 完整示例（路由 + 中间件 + 路径参数 + WebSocket）

### 阶段五：性能深度调优
- **零拷贝优化**:
  - readLoop 直读 inputBuffer（消除栈缓冲区中转 memcpy）
  - HttpServer 使用 `basic_flat_buffer<pmr::allocator>` + 请求级 monotonic pool
  - Scatter-Gather I/O（writeLoop 批量取出消息，`async_write(buffers)` 一次发送）
  - `send(string&&)` move 语义直接转移到 writeQueue
- **pmr 内存池深度调优**:
  - thread_local 无锁获取线程本地池（消除 mutex + map 查找）
  - TrackedResource 追踪型内存资源（原子计数：分配/释放次数、当前字节、峰值字节）
  - PoolConfig 可配置池参数（全局池/线程本地池/请求池）
- **路由分发优化**:
  - 静态路由使用 `unordered_map<RouteKey, handler>` O(1) 哈希查找
  - 参数路由按 HTTP 方法分桶，保持桶内线性扫描（单方法路由数量少）
  - matchPath 改用 `string_view` 原地切分（消除 `vector<string>` / `istringstream` 临时分配）
- **性能基准测试套件**:
  - `http_benchmark` — 多线程 HTTP 压测（QPS / P50 / P90 / P95 / P99 延迟）
  - `pmr_benchmark` — 内存池策略对比（new/delete vs sync_pool vs unsync_pool vs monotonic）
  - `test_router_perf` — 路由查找性能（100/1000 路由静态/参数/未命中）

### 阶段六：文档与交付
- **API 文档** (`docs/api_reference.md`) — 所有公共类和方法的完整说明
- **架构设计文档** (`docs/architecture.md`) — 两层架构、PMR 三层内存池、协程模型、路由/中间件/SSL 设计、Concepts 后端抽象、反射 API 包装层（当前宏降级方案 + C++26 迁移路径）
- **性能测试报告** (`docs/performance_report.md`) — PMR 内存池基准测试方法、HTTP 吞吐量测试场景、调优指南、复现方法
- **使用示例文档** (`docs/examples_guide.md`) — 8 个由浅入深的完整示例（最小服务器 / RESTful API / 中间件 / WebSocket / SSL / 协程 / PMR / 完整应用）

## 命名风格

- **命名空间**: `hical`
- **类名**: 大驼峰（`AsioEventLoop`, `PmrBuffer`）
- **方法名**: 小驼峰（`runAfter`, `isInLoopThread`）
- **回调**: `onMessage`, `onClose`, `onWriteComplete`（hical 风格）
- **常量**: `h` 前缀 + 大驼峰（`hDefaultSize`, `hInvalidTimerId`）
- **错误码**: `ErrorCode::hNoError` 风格
