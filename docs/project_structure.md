# Hical 项目代码结构

> 最后更新：2026-07-17

## 项目概述

Hical 是基于 Boost.Asio、采用原生 HTTP/WebSocket 网络栈（picohttpparser + 自研 WebSocket）的现代 C++20 高性能 Web 框架。核心特性：PMR 三层内存池、协程化 I/O（`asio::awaitable`）、C++20 Concepts 后端约束、C++26 反射双轨设计（原生 P2996 / C++20 宏回退）、可选数据库中间件（Boost.MySQL）、可选 OpenAPI 3.0 元数据层、生产级日志系统。

## 根目录文件

```
hical/
├── CMakeLists.txt              # 顶层 CMake（C++20，project(VERSION 2.6.7)，Boost/OpenSSL/GTest）
├── README.md / README_CN.md    # 项目主页（英文 / 中文双语，含 CI/License/平台徽章）
├── LICENSE                     # MIT 协议
├── CHANGELOG.md                # 版本变更日志（按版本倒序）
├── CONTRIBUTING.md             # 贡献指南（双语：开发流程、PR 规范、代码风格）
├── CODE_OF_CONDUCT.md          # Contributor Covenant 行为准则
├── SECURITY.md                 # 安全策略（漏洞私密上报渠道）
├── AGENTS.md                   # AI 编码助手通用规范（跨工具兼容）
├── .clang-format               # clang-format 22+ 规则（Allman/4 空格/120 列）
├── .clang-tidy                 # clang-tidy 检查项（readability/bugprone/cppcoreguidelines 等）
├── .editorconfig               # 编辑器跨平台编码规范（UTF-8 + LF）
├── .gitignore                  # 忽略 build/、build-*/、IDE 缓存等
│
├── .github/                    # GitHub 社区与 CI 配置
│   ├── workflows/              #   CI 工作流
│   │   ├── ci.yml              #     Ubuntu(GCC14/Clang20) + Windows(MSYS2/MSVC+vcpkg) 矩阵
│   │   ├── sanitizer.yml       #     ASan/UBSan/TSan 检查
│   │   ├── release.yml         #     Release 发布流程
│   │   └── conan-publish.yml   #     Conan 包发布
│   ├── ISSUE_TEMPLATE/         #   Issue 模板（bug_report / feature_request / config）
│   ├── pull_request_template.md#   PR 模板
│   ├── CODEOWNERS              #   代码所有者
│   ├── labeler.yml             #   PR 自动打标签规则
│   └── release.yml             #   Release 自动生成 changelog 规则
│
├── ports/                      # 包管理器 overlay
│   ├── hical61-hical/          #   vcpkg overlay port（portfile.cmake + vcpkg.json + versions 模板）
│   └── picohttpparser/         #   picohttpparser 的 vcpkg port（hical 内嵌依赖的可选系统版本）
│
├── src/                        # 框架源码（详见下节）
├── tests/                      # 单元测试
├── examples/                   # 示例程序
└── docs/                       # 文档
```

## 目录结构

```
hical/
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
│   │   ├── Router.h/.cpp       # 路由器（O(1) 静态 + 按方法分桶参数 + WebSocket + dispatchSync 同步快路径）
│   │   ├── Middleware.h/.cpp   # 中间件系统（异步/同步双轨 + buildOptimizedChain 合并连续同步帧）
│   │   ├── HttpServer.h/.cpp   # HTTP 服务器（SO_REUSEPORT 多 acceptor + fd 耗尽防护 + 中间件预构建）
│   │   ├── HeaderMap.h         # HTTP 头部容器（vector<pair> 实现，大小写不敏感）
│   │   ├── HttpSessionImpl.cpp # HTTP/WS 会话编译防火墙（隔离 picohttpparser + WebSocket）
│   │   ├── WebSocket.h/.cpp    # WebSocket 会话封装（自研实现，子协议/心跳/上下文）
│   │   ├── WsFrame.h           # WebSocket 帧解析/构造（RFC 6455，掩码/RSV/控制帧）
│   │   ├── WsHandshake.h       # WebSocket 握手协议（Sec-WebSocket-Key/Accept + 子协议协商）
│   │   ├── WsDeflate.h/.cpp    # WebSocket permessage-deflate 压缩（pimpl + zlib，zip bomb 防护）
│   │   ├── WsHub.h/.cpp        # WebSocket 广播管理器（weak_ptr 存储，房间/单播/广播）
│   │   ├── WriteNode.h         # 多态写队列节点（MemoryWriteNode / PmrBufferWriteNode / FileWriteNode）
│   │   ├── StaticFiles.h       # 静态文件服务（异步 I/O + PathCache + ETag/304 + 路径遍历防护）
│   │   ├── Multipart.h/.cpp    # multipart/form-data 解析（dual API: req 版 + parts 版）
│   │   ├── Session.h/.cpp      # Session 会话管理（shared_mutex + regenerate + migrateFrom）
│   │   ├── Cors.h              # CORS 中间件（makeCorsMiddleware + CorsOptions）
│   │   ├── JwtAuth.h/.cpp      # JWT 认证中间件（HMAC-SHA256 签发/验证 + SyncBeforeHandler）
│   │   ├── RouteGroup.h/.cpp   # 路由组（前缀分组 + 组级中间件 + 嵌套子组）
│   │   ├── ConfigLoader.h/.cpp # JSON 配置加载器（层级 key + 环境变量覆盖 + env 缓存）
│   │   ├── Reflection.h        # C++26 反射特性检测 + RouteInfo + 类型萃取
│   │   ├── MetaJson.h          # 自动 JSON 序列化（ALIAS/REQUIRED/IGNORE 装饰器 + MIN/MAX/PATTERN/NOT_EMPTY/LENGTH DTO 校验）
│   │   ├── MetaJsonError.h/.cpp # MetaJson 错误辅助（[[noreturn]] 非模板函数，编译防火墙）
│   │   ├── MetaRoutes.h        # 自动路由注册（HICAL_HANDLER/HICAL_ROUTES 宏）
│   │   ├── Log.h/.cpp          # 日志系统（6 级 LogLevel + format/流式/条件/字段四种 API）
│   │   ├── LogRecord.h         # 结构化日志条目（level/timestamp/threadId/file/line/message/fields/traceId）
│   │   ├── LogFormatter.h/.cpp # 日志格式化器接口 + TextFormatter + JsonFormatter
│   │   ├── LogSink.h/.cpp      # 可插拔日志后端（LogSink 抽象 + StderrSink + FileSink + OStreamSink）
│   │   ├── LogFile.h/.cpp      # 日志文件轮转引擎（按大小轮转 + 最大文件数限制）
│   │   ├── AsyncFileSink.h/.cpp # 异步双缓冲文件 Sink（jthread + stop_token + 背压保护）
│   │   ├── FixedBuffer.h       # 栈上固定缓冲区模板（4KB，std::to_chars，溢出 fallback）
│   │   ├── LogChannel.h/.cpp   # 命名日志通道 + LogChannelRegistry（shared_mutex 读多写少）
│   │   ├── LogMiddleware.h/.cpp # 日志中间件（trace-id 自动注入 + 结构化访问日志）
│   │   ├── LogAdmin.h/.cpp     # 动态日志级别管理端点（GET/PUT /admin/log-level）
│   │   ├── OpenApiSchema.h     # JSON Schema 生成（jsonSchema<T>/collectSchemas/HICAL_SCHEMA_NAME）
│   │   ├── OpenApiRegistry.h/.cpp  # 路由元数据注册表（RouteApiInfo/HICAL_API/builder::*）
│   │   ├── OpenApiDocument.h/.cpp  # OpenAPI 3.0 文档组装（惰性缓存/路径合并/参数提取）
│   │   ├── OpenApiEndpoint.h   # 端点暴露（serveOpenApi，/openapi.json + /docs）
│   │   ├── IdleFd.h            # 空闲 fd 预留（POSIX /dev/null，Windows no-op）
│   │   ├── IdleScanner.h/.cpp  # 集中式空闲连接扫描器（per-io_context，侵入式链表 + 单 timer）
│   │   └── Version.h.in        # CMake 配置版本头（唯一版本号来源）
│   │
│   ├── asio/                   # Boost.Asio 适配实现
│   │   ├── AsioEventLoop.h/.cpp      # 基于 io_context 的事件循环
│   │   ├── GenericConnection.h/.cpp/.hci  # 模板化连接（TCP/SSL 统一，.hci 编译防火墙 + extern template）
│   │   ├── SslConnection.h           # SSL 连接类型别名（懒包含 OpenSSL）
│   │   ├── AsioTimer.h/.cpp          # 基于 steady_timer 的定时器
│   │   ├── EventLoopPool.h/.cpp      # 多线程事件循环池（1 Thread : 1 io_context）
│   │   └── TcpServer.h/.cpp          # TCP 服务器（SO_REUSEPORT 多 acceptor + 空闲超时 + IdleFd）
│   │
│   └── db/                     # 数据库中间件（可选，HICAL_WITH_DATABASE=ON）
│       ├── DbConfig.h          # 数据库连接配置（池大小/超时/健康检查/字符集）
│       ├── DbResult.h          # 查询结果封装（columns/rows/affectedRows/insertId）
│       ├── DbConnection.h      # 数据库连接抽象接口（参数化查询/事务/ping/touch）
│       ├── DbConnectionPool.h/.cpp # 协程化连接池（steady_timer 协程信号量 + 健康检查 + 空闲淘汰）
│       ├── DbMiddleware.h      # HTTP 数据库中间件（连接注入 + 自动事务）
│       ├── DbQueryLog.h/.cpp   # 查询日志中间件（装饰器模式 + 慢查询检测）
│       ├── MysqlConnection.h/.cpp  # MySQL 后端（Boost.MySQL any_connection + charset 白名单校验）
│       └── StmtCache.h/.cpp    # PreparedStatement LRU 缓存（透明哈希 string_view 查找）
│
├── tests/                      # 单元测试（Google Test）— 52 个基础测试套件（+ 1 个可选 OpenAPI + 5 个可选 DB 测试）
│   ├── CMakeLists.txt          # gtest_discover_tests 自动注册 + Windows ws2_32/mswsock 链接
│   │
│   │ # —— 基础设施 ——
│   ├── test_basic.cpp                # 基础测试
│   ├── test_error.cpp                # 错误码转换
│   ├── test_memory_pool.cpp          # MemoryPool + PmrBuffer（含 TrackedResource）
│   ├── test_coroutine.cpp            # 协程工具
│   ├── test_concepts.cpp             # C++20 Concepts 编译期约束
│   │ # —— Asio 适配层 ——
│   ├── test_asio_event_loop.cpp      # AsioEventLoop
│   ├── test_asio_timer.cpp           # AsioTimer
│   ├── test_asio_tcp_connection.cpp  # TcpConnection
│   ├── test_tcp_server.cpp           # TcpServer + EventLoopPool
│   ├── test_ssl_connection.cpp       # SSL 连接
│   │ # —— HTTP 核心 ——
│   ├── test_http_types.cpp           # HTTP 类型
│   ├── test_router.cpp               # 路由器（含路径参数）
│   ├── test_router_perf.cpp          # 路由性能基准（dispatchSync 对比 + 100/1000 路由）
│   ├── test_http_server_perf.cpp     # HTTP 全链路吞吐量基准（单连接/并发）
│   ├── test_middleware.cpp           # 中间件（洋葱模型/拦截）
│   ├── test_http_server.cpp          # HttpServer 集成
│   ├── test_integration.cpp          # 完整请求/响应周期集成
│   ├── test_cookie.cpp               # Cookie 解析与 Set-Cookie
│   ├── test_static_files.cpp         # 静态文件 / ETag / 路径遍历
│   ├── test_multipart.cpp            # multipart/form-data
│   ├── test_session.cpp              # Session 生命周期 / 线程安全 / regenerate
│   ├── test_query_params.cpp         # 查询参数解析
│   ├── test_form_params.cpp          # 表单参数解析
│   ├── test_redirect.cpp             # 重定向
│   ├── test_cors.cpp                 # CORS 中间件
│   ├── test_jwt_auth.cpp             # JWT Auth 中间件（15 个）
│   ├── test_config_loader.cpp        # ConfigLoader 配置加载（17 个）
│   ├── test_route_group.cpp          # 路由组
│   ├── test_error_handler.cpp        # 全局错误处理器
│   ├── test_graceful_shutdown.cpp    # 优雅关闭
│   ├── test_rate_limiter.cpp         # 令牌桶限流
│   ├── test_helmet.cpp               # 安全头中间件
│   ├── test_health.cpp               # 健康检查端点
│   ├── test_wildcard_route.cpp       # 通配路由（*path 模式）
│   ├── test_compression.cpp          # Gzip 压缩中间件
│   ├── test_chunked_sse.cpp          # Chunked 传输 + SSE 流
│   ├── test_expect_continue.cpp      # Expect: 100-continue
│   ├── test_http_client.cpp          # HTTP 客户端测试工具
│   ├── test_optimistic_write.cpp     # 乐观同步写
│   │ # —— WebSocket ——
│   ├── test_websocket.cpp            # WebSocket 基础
│   ├── test_ws_advanced.cpp          # WebSocket 进阶（子协议/心跳/压缩/Hub 广播）
│   │ # —— 反射 / OpenAPI ——
│   ├── test_reflection.cpp           # MetaJson + MetaRoutes（61 个，含 DTO 校验）
│   ├── test_openapi.cpp              # OpenAPI 自动生成（35 个）
│   │ # —— 日志系统 ——
│   ├── test_log.cpp                  # Log 核心（36 个）
│   ├── test_log_ndebug.cpp           # NDEBUG TRACE 消除（3 个）
│   ├── test_fixed_buffer.cpp         # FixedBuffer（19 个）
│   ├── test_read_buffer_pool.cpp       # ReadBufferPool 借还
│   ├── test_log_file.cpp             # LogFile 轮转（7 个）
│   ├── test_async_file_sink.cpp      # AsyncFileSink（7 个）
│   ├── test_log_formatter.cpp        # TextFormatter + JsonFormatter（12 个）
│   ├── test_log_channel.cpp          # LogChannel + Registry（12 个）
│   ├── test_log_middleware.cpp       # LogMiddleware trace-id（3 个）
│   ├── test_log_admin.cpp            # LogAdmin 端点（4 个）
│   │ # —— 数据库（可选，需 HICAL_WITH_DATABASE=ON） ——
│   ├── test_db_pool.cpp              # 连接池单元测试（Mock，12 个）
│   ├── test_db_middleware.cpp        # DB 中间件（Mock，8 个）
│   ├── test_db_query_log.cpp         # 查询日志（Mock，6 个）
│   ├── test_stmt_cache.cpp           # PreparedStatement 缓存（9 个）
│   └── test_mysql_integration.cpp    # MySQL 集成（需真实数据库，7 个）
│
├── examples/                   # 示例程序（8 个）
│   ├── CMakeLists.txt
│   ├── echo_server.cpp         # 协程式 Echo Server
│   ├── pmr_poc.cpp             # pmr 内存池验证
│   ├── benchmark.cpp           # Echo Server 压力测试工具
│   ├── http_server.cpp         # HTTP Server 完整示例（路由 + 中间件 + WebSocket）
│   ├── http_benchmark.cpp      # HTTP 基准测试工具（QPS/P50/P99）
│   ├── pmr_benchmark.cpp       # pmr 内存池基准测试（多策略对比）
│   ├── openapi_server.cpp      # OpenAPI 文档自动生成示例
│   └── reflection_server.cpp   # 反射层 DTO + 路由自动注册示例
│
└── docs/                       # 文档
    ├── project_structure.md    # 本文件 — 项目代码结构说明
    ├── api_reference.md        # 完整 API 参考（公共类/方法/枚举/宏）
    ├── architecture.md         # 架构设计（两层架构/PMR 三层池/协程模型/Concepts/反射）
    ├── build_and_test_guide.md # 编译与测试指南
    ├── quickstart.md           # 5 分钟快速上手
    ├── examples_guide.md       # 8 个由浅入深的完整示例讲解
    ├── integration_guide.md    # 集成指南（vcpkg overlay / FetchContent / cmake install）
    ├── coroutine-guide.md      # 协程入门教程
    ├── logging-guide.md        # 日志系统完全指南
    ├── openapi-guide.md        # OpenAPI/Swagger 集成指南
    ├── performance_report.md   # 性能测试报告（基准方法 + 调优指南）
    ├── perf-analysis-guide.md  # Linux 性能分析实战（perf/火焰图/heaptrack）
    └── production-deployment.md # 生产部署实践（编译优化/容器化/监控）
```

## CMake 构建选项

| 选项                              | 默认 | 说明                                                                          |
| --------------------------------- | ---- | ----------------------------------------------------------------------------- |
| `HICAL_WITH_DATABASE`             | OFF  | 启用 [src/db/](../src/db/) 数据库中间件，需 Boost.MySQL >= 1.85               |
| `HICAL_WITH_OPENAPI`              | ON   | 启用 OpenAPI 3.0 元数据层（`OpenApi*.h/cpp`），无额外依赖                     |
| `HICAL_ENABLE_REFLECTION`         | OFF  | 启用 C++26 原生反射（P2996），需兼容编译器；OFF 时回退到 C++20 宏             |
| `HICAL_USE_SYSTEM_PICOHTTPPARSER` | OFF  | 使用系统安装的 picohttpparser 替代内嵌副本                                    |
| `HICAL_WITH_MIMALLOC`             | OFF  | 使用 mimalloc 作为 PMR 最底层 upstream 分配器，替代默认 `new_delete_resource` |
| `HICAL_BUILD_TESTS`               | ON   | 构建 [tests/](../tests/) 单元测试                                             |
| `CMAKE_BUILD_TYPE`                | —    | `Debug` / `Release` / `RelWithDebInfo`                                        |

## 命名空间布局

| 命名空间        | 范围                                                                    | 头文件位置                                              |
| --------------- | ----------------------------------------------------------------------- | ------------------------------------------------------- |
| `hical::`       | 框架核心（HTTP/WebSocket/中间件/Session/日志/OpenAPI）                  | `<hical/core/>`                                         |
| `hical::meta::` | 反射层（MetaJson / MetaRoutes 的 `toJson`/`fromJson`/`registerRoutes`） | `<hical/core/MetaJson.h>` / `<hical/core/MetaRoutes.h>` |
| `hical::db::`   | 数据库中间件（可选）                                                    | `<hical/db/>`                                           |

**公共 vs 内部 API 边界**：

- `<hical/core/*.h>` / `<hical/db/*.h>` — 用户可直接 `#include`，遵循语义化版本兼容
- `<hical/asio/*.h>` — 视为实现细节，跨小版本可能变动；建议通过 `Concepts.h` 的 `NetworkBackend` 概念替换后端
- `*Impl.cpp` / `*.hci` — 编译防火墙文件，禁止用户 include

## 核心组件关系

```
┌─────────────────────────────────────────────────────────────┐
│                        用户应用层                            │
└─────────────────────────────────────────────────────────────┘
                              │
┌─────────────────────────────────────────────────────────────┐
│                    HttpServer (顶层 Facade)                  │
│  ┌──────────┐ ┌────────────┐ ┌─────────────┐ ┌───────────┐  │
│  │  Router  │ │ Middleware │ │ WsHub /     │ │ OpenApi   │  │
│  │  (O(1) + │ │  Pipeline  │ │ WsSession   │ │ (可选)    │  │
│  │  {param})│ │ (Async/Sync│ │ (子协议/    │ │           │  │
│  └──────────┘ │  双轨)     │ │  压缩/心跳) │ └───────────┘  │
│  ┌──────────┐ └────────────┘ └─────────────┘ ┌───────────┐  │
│  │RouteGroup│ ┌────────────┐ ┌─────────────┐ │   CORS    │  │
│  └──────────┘ │  Session   │ │ StaticFiles │ └───────────┘  │
│  ┌──────────┐ └────────────┘ └─────────────┘ ┌───────────┐  │
│  │ MetaJson │ ┌────────────┐ ┌─────────────┐ │ Multipart │  │
│  │MetaRoutes│ │ LogMiddle- │ │  DbMiddle-  │ └───────────┘  │
│  │ (反射)   │ │   ware     │ │   ware (可选)│                │
│  └──────────┘ └────────────┘ └─────────────┘                │
│  ┌─────────────────────────────────────────────────────────┐│
│  │       HttpRequest / HttpResponse / HeaderMap            ││
│  │       (原生 HTTP 栈：picohttpparser + 自研 WebSocket)   ││
│  └─────────────────────────────────────────────────────────┘│
└─────────────────────────────────────────────────────────────┘
                              │
┌─────────────────────────────────────────────────────────────┐
│                    TcpServer (网络层)                        │
│  ┌──────────────┐  ┌─────────────────────────────────────┐  │
│  │ EventLoopPool│  │ GenericConnection<SocketType>       │  │
│  │ (SO_REUSEPORT│  │  Plain (tcp::socket)                │  │
│  │  多 acceptor)│  │  SSL   (ssl::stream<tcp::socket>)   │  │
│  └──────────────┘  └─────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────┘
                              │
┌─────────────────────────────────────────────────────────────┐
│        DB Middleware (可选, HICAL_WITH_DATABASE=ON)          │
│  ┌──────────────┐ ┌─────────────┐ ┌──────────────────────┐  │
│  │ DbMiddleware │ │ DbQueryLog  │ │ DbConnectionPool     │  │
│  │ (连接注入 +  │ │ (装饰器 +   │ │ (协程信号量 +        │  │
│  │  自动事务)   │ │  慢查询)    │ │  健康检查 + LIFO)    │  │
│  └──────────────┘ └─────────────┘ └──────────────────────┘  │
│  ┌─────────────────────────────────────────────────────────┐│
│  │  MysqlConnection (Boost.MySQL) + StmtCache (LRU)        ││
│  └─────────────────────────────────────────────────────────┘│
└─────────────────────────────────────────────────────────────┘
                              │
┌─────────────────────────────────────────────────────────────┐
│                  Core Infrastructure (被所有层使用)          │
│  ┌──────────────┐ ┌──────────┐ ┌──────────────┐ ┌────────┐  │
│  │ EventLoop /  │ │ Memory-  │ │ Log System   │ │ Error  │ │
│  │ Timer / Tcp- │ │ Pool /   │ │ (Logger +    │ │ Inet-  │ │
│  │ Connection   │ │ PmrBuffer│ │  Channel +   │ │ Address│ │
│  │ (Concepts)   │ │          │ │  Sink + Fmt) │ │ SslCtx │ │
│  └──────────────┘ └──────────┘ └──────────────┘ └────────┘  │
└─────────────────────────────────────────────────────────────┘
```

## 构建系统

- **C++ 标准**：C++20（C++26 反射可选，由 `HICAL_ENABLE_REFLECTION` 控制）
- **构建工具**：CMake 3.20+，推荐 Ninja
- **编译器支持**：GCC 14+ / Clang 20+ / MSVC 2022+
- **依赖**：
  - Boost 1.82+（system / json，Asio header-only）
  - Boost 1.85+（DB 中间件额外需要 mysql / charconv）
  - OpenSSL 3.x（SSL/TLS、Session ID 生成）
  - zlib（WebSocket permessage-deflate）
  - picohttpparser（内嵌副本，可选系统版本）
  - Google Test（测试套件）
  - Windows 额外：`ws2_32`、`mswsock`
- **构建产物**：
  - `hical_core` — 框架核心静态库（仅静态库，DLL/ABI 不兼容）
  - `test_*` — 52 个基础测试套件（+ 1 个可选 OpenAPI + 5 个可选 DB 测试）
  - 8 个示例可执行文件
- **CI 工作流**：见 [.github/workflows/](../.github/workflows/) — `ci.yml`（多平台矩阵）、`sanitizer.yml`（ASan/UBSan/TSan）、`release.yml` / `conan-publish.yml`

详细构建步骤参见 [build_and_test_guide.md](build_and_test_guide.md)；分发与集成参见 [integration_guide.md](integration_guide.md)。

## 模块演进摘要

> 完整版本历史与逐次变更见 [CHANGELOG.md](../CHANGELOG.md)。本节仅给出主要里程碑，便于理解仓库当前状态来源。

| 阶段      | 关键模块                                                                                                       | 说明                                                        |
| --------- | -------------------------------------------------------------------------------------------------------------- | ----------------------------------------------------------- |
| 基础设施  | `MemoryPool` / `PmrBuffer` / `EventLoop` / `Concepts`                                                          | 三层 PMR 池 + Asio 抽象 + C++20 概念约束                    |
| 网络层    | `TcpServer` / `GenericConnection` / `SslContext`                                                               | SO_REUSEPORT 多 acceptor、TCP/SSL 模板统一、零分配写队列    |
| HTTP 框架 | `HttpServer` / `Router` / `Middleware` / `HttpRequest` / `HttpResponse`                                        | 协程化处理、洋葱中间件、参数路由、`dispatchSync` 同步快路径 |
| 协议增强  | `Session` / `Cookie` / `Cors` / `StaticFiles` / `Multipart` / `RouteGroup`                                     | 完整 HTTP 周边能力                                          |
| WebSocket | `WebSocket` / `WsFrame` / `WsHandshake` / `WsDeflate` / `WsHub`                                                | 自研 RFC 6455 栈，子协议/心跳/压缩/广播                     |
| 反射层    | `Reflection` / `MetaJson` / `MetaRoutes`                                                                       | 双轨设计：C++26 原生 P2996 + C++20 宏回退                   |
| 日志系统  | `Log` / `LogChannel` / `LogFormatter` / `LogSink` / `LogFile` / `AsyncFileSink` / `LogMiddleware` / `LogAdmin` | 6 级日志、命名通道、异步双缓冲、动态级别管理                |
| OpenAPI   | `OpenApiSchema` / `OpenApiRegistry` / `OpenApiDocument` / `OpenApiEndpoint`                                    | 从 `HICAL_JSON` 自动派生 OpenAPI 3.0 文档                   |
| 数据库    | `DbConfig` / `DbConnectionPool` / `DbMiddleware` / `DbQueryLog` / `MysqlConnection` / `StmtCache`              | 协程化连接池 + 装饰器查询日志 + PreparedStatement LRU       |

## 命名风格

- **命名空间**：`hical::`（核心）、`hical::meta::`（反射）、`hical::db::`（数据库）
- **类 / 结构体 / 枚举**：大驼峰，无前缀（`HttpServer`, `RouteInfo`, `HttpMethod`）
- **抽象接口**：大驼峰，无 `I` 前缀（`EventLoop`, `TcpConnection`）
- **枚举常量**：`h` 前缀 + 大驼峰（`hGet`, `hOk`, `hInvalidTimerId`）
- **成员变量**：尾下划线 + 小驼峰（`ioContext_`）
- **全局 / 静态变量**：`g_` / `s` 前缀 + 小驼峰
- **`static constexpr` 常量**：`k` 前缀 + 大驼峰（`kMaxHeaderSize`）
- **方法 / 函数 / 局部变量**：小驼峰（`runAfter`, `isInLoopThread`, `bytesRead`）
- **指针参数**：`p` 前缀 + 大驼峰（`pSocket`）
- **宏**：大写下划线（`HICAL_ROUTE`, `HICAL_LOG_INFO`）
- **模板参数**：大驼峰（`SocketType`）

> 完整约定与 clang-tidy 检查项参见 [AGENTS.md](../AGENTS.md) 与 [.clang-tidy](../.clang-tidy)。
