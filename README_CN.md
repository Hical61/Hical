# hical

[![CI](https://github.com/Hical61/Hical/actions/workflows/ci.yml/badge.svg)](https://github.com/Hical61/Hical/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![C++ Standard](https://img.shields.io/badge/C%2B%2B-20%20%7C%2026-blue.svg)](https://isocpp.org/)
[![Platform](https://img.shields.io/badge/Platform-Linux%20%7C%20Windows%20%7C%20macOS-green.svg)]()
[![Boost](https://img.shields.io/badge/Boost-%E2%89%A51.82-orange.svg)](https://www.boost.org/)
[![GitHub release](https://img.shields.io/github/v/release/Hical61/Hical?include_prereleases&sort=semver)](https://github.com/Hical61/Hical/releases)
[![GitHub stars](https://img.shields.io/github/stars/Hical61/Hical?style=flat)](https://github.com/Hical61/Hical/stargazers)
[![PRs Welcome](https://img.shields.io/badge/PRs-welcome-brightgreen.svg)](CONTRIBUTING.md)

Hical 是一个基于 Boost.Asio/Beast，利用 C++26 反射和 PMR 内存池构建高性能的现代 C++ Web 框架。

> C++20/26 双轨反射 · PMR 内存池 · 协程异步 I/O · Cookie / Session / 静态文件 / 文件上传 内置 · CORS · RouteGroup 路由分组 · 日志系统 · OpenAPI 3.0 自动文档 · 可选 DB 中间件（Boost.MySQL）

[English](README.md) | 简体中文

## 特性

- **C++26 反射** — 面向 C++26 反射特性设计，支持自动路由注册、JSON 序列化/反序列化等编译期元编程能力；C++20 宏回退提供相同 API
- **Boost.Asio/Beast 后端** — 工业级网络库，每线程独立 `io_context` 模型
- **PMR 内存池** — 三层 `std::pmr` 分配器策略（全局同步池、线程局部池、请求级单调缓冲区），缓冲区、HTTP 消息体、JSON 对象共享内存池
- **协程支持** — `asio::awaitable<T>` + `co_spawn`，简洁的异步代码风格
- **C++ Concepts** — 编译期 `NetworkBackend` 类型约束，保障类型安全
- **SSL/TLS** — 模板化 `GenericConnection<SocketType>`，编译期 `if constexpr` 分支同时支持明文和加密连接
- **WebSocket** — WebSocket 升级与双向通信，支持 Origin 白名单
- **路由 & 中间件** — 洋葱模型中间件管线，支持路径参数（`{id}`）、静态路由 O(1) 哈希查找、参数路由按方法分组
- **RouteGroup 路由分组** — 前缀分组 + 组级中间件，支持嵌套分组
- **CORS 中间件** — 内置跨域资源共享中间件，支持预检请求自动处理
- **HTTP 服务器** — 基于 Boost.Beast 的完整 HTTP/1.1 支持（分块传输、Keep-Alive），fd 耗尽保护
- **Cookie 支持** — RFC 6265 兼容解析（first-wins 语义）+ `Set-Cookie`（CRLF 注入防护）
- **静态文件服务** — MIME 自动推断、ETag/304 缓存验证、路径遍历防护、异步文件 I/O、PathCache（4096 条/60s TTL）、64 MB 大小限制
- **Multipart 文件上传** — RFC 7578 `multipart/form-data` 解析，含 DoS 防护（≤256 个 Part）
- **Session 会话管理** — 内存 `SessionManager`，懒 GC，128 位随机 ID，线程安全，会话固定防护（`regenerate()`）、原子数据迁移（`migrateFrom()`）
- **日志系统** — 6 级日志（Trace~Fatal）、`std::format` + 流式 + 条件宏三种 API、可插拔 Sink（Stderr/File/AsyncFile/OStream）、命名通道（Channel）、JSON/Text 格式化器、异步双缓冲文件写入、NDEBUG 下 TRACE 编译期消除、动态级别管理端点（LogAdmin）
- **OpenAPI 3.0 自动文档** — 从 `HICAL_JSON` 宏自动生成 JSON Schema，`HICAL_API()` 路由标注，一键暴露 `/openapi.json` + Swagger UI `/docs`
- **可选数据库中间件** — 基于协程的 Boost.MySQL 后端，连接池（LIFO 复用、健康检查、空闲驱逐）、自动事务、查询日志与慢查询检测、LRU 预处理语句缓存

## 快速开始

```cpp
#include "core/HttpServer.h"
#include "core/WebSocket.h"

using namespace hical;

int main()
{
    HttpServer server(8080);

    // 中间件 — 日志
    server.use([](const HttpRequest& req, MiddlewareNext next)
                   -> Awaitable<HttpResponse> {
        std::cout << httpMethodToString(req.method()) << " "
                  << req.path() << std::endl;
        co_return co_await next(req);
    });

    // GET / — JSON 响应
    server.router().get("/", [](const HttpRequest&) -> HttpResponse {
        return HttpResponse::json({
            {"status", "running"},
            {"framework", "hical"}
        });
    });

    // GET /users/{id} — 路径参数
    server.router().get("/users/{id}",
        [](const HttpRequest& req) -> HttpResponse {
            return HttpResponse::json({{"userId", req.param("id")}});
        });

    // WebSocket Echo
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

> 完整教程请参考 [docs/quickstart.md](docs/quickstart.md)，更多示例见 [examples/](examples/)。

## 项目结构

```
hical/
├── src/
│   ├── core/            # 抽象接口、共享类型、HTTP 框架与反射层
│   │   ├── EventLoop.h, Timer.h, TcpConnection.h   # 抽象基类
│   │   ├── Concepts.h, Coroutine.h                  # C++20 Concepts & 协程
│   │   ├── MemoryPool.h/cpp, PmrBuffer.h            # 三层 PMR 内存池
│   │   ├── HttpServer.h/cpp, HttpRequest.h/cpp      # HTTP 服务器
│   │   ├── HttpResponse.h/cpp, HttpTypes.h          # HTTP 响应 & 类型
│   │   ├── Router.h/cpp, RouteGroup.h/cpp           # 路由 & 路由分组
│   │   ├── Middleware.h/cpp, Cors.h                 # 中间件 & CORS
│   │   ├── WebSocket.h/cpp, SslContext.h/cpp        # WebSocket & SSL
│   │   ├── Cookie.h, Session.h/cpp                  # Cookie & 会话
│   │   ├── StaticFiles.h, Multipart.h/cpp           # 静态文件 & 文件上传
│   │   ├── Reflection.h, MetaJson.h, MetaRoutes.h   # C++26 反射层
│   │   ├── Log.h/cpp, LogRecord.h, LogFormatter.h/cpp  # 日志核心
│   │   ├── LogSink.h/cpp, LogFile.h/cpp             # 日志 Sink & 文件轮转
│   │   ├── AsyncFileSink.h/cpp, FixedBuffer.h       # 异步 Sink & 栈缓冲
│   │   ├── LogChannel.h/cpp, LogMiddleware.h/cpp    # 日志通道 & 中间件
│   │   ├── LogAdmin.h/cpp                           # 动态日志级别端点
│   │   ├── OpenApiSchema.h, OpenApiRegistry.h/cpp   # OpenAPI Schema & 注册表
│   │   ├── OpenApiDocument.h/cpp, OpenApiEndpoint.h # OpenAPI 文档 & 端点
│   │   └── IdleFd.h, WriteNode.h, Version.h.in      # 工具类
│   ├── asio/            # Boost.Asio 实现层
│   │   ├── AsioEventLoop.h/cpp, AsioTimer.h/cpp
│   │   ├── GenericConnection.h/cpp, SslConnection.h
│   │   ├── EventLoopPool.h/cpp, TcpServer.h/cpp
│   │   └── ...
│   └── db/              # 可选数据库中间件（HICAL_WITH_DATABASE=ON）
│       ├── DbConfig.h, DbResult.h, DbConnection.h  # 配置、结果、抽象接口
│       ├── DbConnectionPool.h/cpp                   # 协程连接池
│       ├── DbMiddleware.h                           # HTTP 中间件集成
│       ├── DbQueryLog.h/cpp                         # 查询日志 & 慢查询检测
│       ├── MysqlConnection.h/cpp                    # Boost.MySQL 后端
│       └── StmtCache.h/cpp                          # LRU 预处理语句缓存
├── tests/               # 单元测试（Google Test，30+ 个测试可执行文件）
├── examples/            # HTTP 服务器、WebSocket、OpenAPI、基准测试、PMR 演示
├── docs/                # 设计文档与指南
└── CMakeLists.txt
```

## 依赖

| 依赖项      | 版本要求                                                                   |
| ----------- | -------------------------------------------------------------------------- |
| C++ 标准    | C++20 / C++26                                                              |
| Boost       | >= 1.82（Asio、Beast、System、JSON）；DB 中间件 >= 1.85（MySQL、charconv） |
| CMake       | >= 3.20                                                                    |
| OpenSSL     | 必需                                                                       |
| Google Test | 必需                                                                       |
| 编译器      | GCC 14+ / Clang 20+ / MSVC 2022+                                           |

## 安装

### vcpkg（推荐）

```bash
vcpkg install hical61-hical
```

在 `CMakeLists.txt` 中使用：

```cmake
find_package(hical CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE hical::hical_core)
```

### Conan

从 [GitHub Releases](https://github.com/Hical61/Hical/releases) 下载 Conan 源码包并导入本地缓存：

```bash
# 下载并解压（将 VERSION 替换为实际版本号）
curl -LO https://github.com/Hical61/Hical/releases/download/vVERSION/hical-VERSION-conan-src.tar.gz
tar xzf hical-VERSION-conan-src.tar.gz

# 导出到本地 Conan 缓存
cd hical
conan export . --version=VERSION
```

在 `conanfile.txt` 中添加：

```ini
[requires]
hical/VERSION

[generators]
CMakeDeps
CMakeToolchain
```

安装并构建：

```bash
conan install . --build=missing
```

在 `CMakeLists.txt` 中使用：

```cmake
find_package(hical REQUIRED)
target_link_libraries(my_app PRIVATE hical::hical_core)
```

### 从源码构建

#### Linux / macOS

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
```

#### Windows（MSYS2 MINGW64）

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

#### Windows（MSVC + vcpkg）

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
ctest --test-dir build --output-on-failure -C Release
```

#### 启用可选模块

```bash
# 数据库中间件（需要 Boost.MySQL >= 1.85）
cmake -B build -DHICAL_WITH_DATABASE=ON ...

# 禁用 OpenAPI 模块（默认启用）
cmake -B build -DHICAL_WITH_OPENAPI=OFF ...

# 启用 C++26 反射（需要兼容编译器）
cmake -B build -DHICAL_ENABLE_REFLECTION=ON ...
```

## 性能

Hical 内置三层 PMR 内存池架构：

- **线程本地池** — 无锁分配，线程间零竞争
- **请求级单调池** — 请求结束后整体释放，无逐对象析构开销
- **Scatter-Gather I/O** — 多条消息合并为一次系统调用

运行内置基准测试：

```bash
./build/examples/http_server 8080
./build/examples/http_benchmark localhost 8080 50 1000 /api/status GET
```

> 详见 [docs/performance_report.md](docs/performance_report.md)。

## 文档

- [快速上手](docs/quickstart.md) — 5 分钟教程
- [构建与测试指南](docs/build_and_test_guide.md) — 构建、测试、CI 配置
- [示例指南](docs/examples_guide.md) — 从简到繁的完整示例
- [API 参考](docs/api_reference.md) — 完整公共 API
- [架构设计](docs/architecture.md) — 设计决策与内部实现
- [集成指南](docs/integration_guide.md) — vcpkg / Conan / CMake 集成
- [项目结构](docs/project_structure.md) — 源码目录与模块说明
- [性能报告](docs/performance_report.md) — 基准测试方法与分析
- [贡献指南](CONTRIBUTING.md) — 如何参与贡献

## 许可证

[MIT](LICENSE)
