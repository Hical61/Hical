# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

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
- 新增 `ports/hical/` vcpkg overlay port（`portfile.cmake`、`vcpkg.json`、`usage`）
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
- GitHub Actions CI（GCC 14 / Clang 18 / MSYS2 MINGW64 / MSVC + vcpkg 四平台矩阵）
- 232 个测试用例（GTest + CTest 集成），clang-format / clang-tidy 检查
- MSVC + vcpkg 支持（`vcpkg.json` 清单）
- 社区基础设施：`CONTRIBUTING.md`、`CODE_OF_CONDUCT.md`、`SECURITY.md`、PR/Issue 模板

### Security

- Cookie `Set-Cookie` 头 CRLF 注入防护
- 静态文件路径遍历防护
- Multipart Part 数量上限（DoS 防护）
- Session ID 使用密码学安全的随机数生成

[Unreleased]: https://github.com/Hical61/Hical/compare/v1.0.0...HEAD
[1.0.0]: https://github.com/Hical61/Hical/releases/tag/v1.0.0
