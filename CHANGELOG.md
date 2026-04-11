# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [0.2.0] - 2026-04-12

### Added
- C++26 反射层（双轨设计）：原生 P2996 + C++20 宏回退
  - `MetaJson.h` — 自动 JSON 序列化/反序列化（`toJson` / `fromJson` / `req.readJson<T>()`）
  - `MetaRoutes.h` — 自动路由注册（`HICAL_HANDLER` / `HICAL_ROUTES` 宏 + `registerRoutes()`）
  - `Reflection.h` — 特性检测 `HICAL_HAS_REFLECTION`，`RouteInfo`，`HasRouteTable` / `HasJsonFields` 类型萃取
- MSVC + vcpkg CI 支持
- 安全策略文件 `SECURITY.md`
- 社区基础设施：PR 模板、Issue 模板（Bug/Feature）、`CODEOWNERS`
- `CONTRIBUTING.md` 贡献指南、`CODE_OF_CONDUCT.md` 行为准则
- `.editorconfig` / `.gitattributes` 统一编辑器与行尾配置
- 多平台构建文档（Linux / macOS / MSYS2 / MSVC）
- 技术博客系列（`docs/blog/`）

### Changed
- 移除 `AsioTcpConnection` 向后兼容层，统一使用 `GenericConnection<SocketType>`

### Fixed
- 添加 `/Zc:__cplusplus` 修复 MSVC CI 测试失败
- 修复 CI 格式检查和 MSVC 构建失败
- 使用 CI 默认 clang-format（v18）统一代码格式

### Performance
- 全面性能优化（PMR 内存池、路由分发、中间件管线）
- 全面代码审查，修复 P0/P1 级问题

## [0.1.0] - 2026-04-07

### Added
- 项目初始化
- Boost.Asio/Beast 异步网络层
- 协程异步 I/O（`co_await` + `boost::asio::use_awaitable`）
- 三级 PMR 内存策略（全局同步 / 线程本地 / 请求级单调缓冲）
- C++20 Concepts 编译期类型约束（`EventLoopLike`、`TcpConnectionLike`、`TimerLike`、`NetworkBackend`）
- 静态路由（hash map O(1)）+ 参数路由（`{id}` 模式）+ WebSocket 路由
- 洋葱模型中间件管线
- SSL/TLS 支持（`GenericConnection<ssl::stream<tcp::socket>>`）
- `EventLoopPool` 多线程池（1 线程 : 1 io_context，轮询分发）
- 18 个测试用例（GTest + CTest 集成）
- GitHub Actions CI（GCC / Clang / MSYS2 三平台矩阵）
- README 及文档（快速入门、架构概览、API 参考、性能报告）

[Unreleased]: https://github.com/Hical61/Hical/compare/v0.2.0...HEAD
[0.2.0]: https://github.com/Hical61/Hical/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/Hical61/Hical/releases/tag/v0.1.0
