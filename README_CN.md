# hical

[![CI](https://github.com/Hical61/Hical/actions/workflows/ci.yml/badge.svg)](https://github.com/Hical61/Hical/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![C++ Standard](https://img.shields.io/badge/C%2B%2B-20%20%7C%2026-blue.svg)](https://isocpp.org/)
[![Platform](https://img.shields.io/badge/Platform-Linux%20%7C%20Windows%20%7C%20macOS-green.svg)]()
[![Boost](https://img.shields.io/badge/Boost-%E2%89%A51.82-orange.svg)](https://www.boost.org/)
[![GitHub release](https://img.shields.io/github/v/release/Hical61/Hical?include_prereleases&sort=semver)](https://github.com/Hical61/Hical/releases)
[![GitHub stars](https://img.shields.io/github/stars/Hical61/Hical?style=flat)](https://github.com/Hical61/Hical/stargazers)
[![PRs Welcome](https://img.shields.io/badge/PRs-welcome-brightgreen.svg)](CONTRIBUTING.md)

**Hical** 是一个基于 Boost.Asio 的现代 C++20/26 高性能 Web 框架，采用自研 HTTP/WebSocket 原生栈，利用 C++26 反射和 PMR 内存池实现极致性能。

[English](README.md) | 简体中文

## 特性

- **自研原生栈** — picohttpparser HTTP 解析 + 自研 WebSocket (RFC 6455)，性能与 Drogon/Cinatra 持平
- **C++26 反射** — 自动路由注册 + JSON 序列化/反序列化；C++20 宏回退
- **协程异步 I/O** — `asio::awaitable<T>` + `co_await`，简洁高效
- **PMR 三层内存池** — 全局同步池 / 线程本地无锁池 / 请求级单调缓冲
- **高性能路由** — 静态路由 O(1) 哈希查找，同步快速路径零协程帧开销
- **洋葱中间件** — 异步 + 同步双模中间件，`SyncMiddleware` 零协程帧
- **WebSocket** — permessage-deflate 压缩，Origin 白名单，分片重组
- **SSL/TLS** — 模板化 `GenericConnection`，编译期分支
- **SO_REUSEPORT** — 多 acceptor，accept 与 I/O 同线程
- **RouteGroup** — 前缀分组 + 组级中间件 + 嵌套
- **CORS** — 内置跨域中间件，自动预检
- **Cookie / Session** — RFC 6265 + 会话固定防护 + 原子迁移
- **静态文件** — ETag/304 + 异步 I/O + PathCache
- **Multipart** — RFC 7578 文件上传，DoS 防护
- **日志系统** — 6 级日志，异步双缓冲，命名通道，动态级别管理
- **OpenAPI 3.0** — 从宏自动生成文档，一键暴露 Swagger UI
- **数据库中间件** — 可选，协程连接池 + 自动事务 + 慢查询检测（Boost.MySQL）

## 快速开始

```cpp
#include "core/HttpServer.h"

using namespace hical;

int main()
{
    HttpServer server(8080);

    server.router().get("/", [](const HttpRequest&) -> HttpResponse {
        return HttpResponse::json({{"message", "Hello, Hical!"}});
    });

    server.router().get("/users/{id}",
        [](const HttpRequest& req) -> HttpResponse {
            return HttpResponse::json({{"userId", req.param("id")}});
        });

    server.start();
}
```

```bash
curl http://localhost:8080/
# {"message":"Hello, Hical!"}
```

> 完整教程见 [docs/quickstart.md](docs/quickstart.md)，更多示例见 [examples/](examples/)。

## 性能

自研 HTTP/WebSocket 栈，性能与 Drogon、Cinatra 等主流 C++ 框架持平。详见 [性能报告](docs/performance_report.md)。

```bash
# Docker 一键压测
docker compose -f docker-compose.bench.yml up
```

## 依赖

| 依赖项                           | 说明                                    |
| -------------------------------- | --------------------------------------- |
| C++20/26                         | C++26 可选（反射）                      |
| Boost >= 1.82                    | Asio, System, JSON；DB 中间件需 >= 1.85 |
| OpenSSL                          | 必需                                    |
| zlib                             | 必需（WebSocket 压缩）                  |
| CMake >= 3.20                    | 构建系统                                |
| GCC 14+ / Clang 20+ / MSVC 2022+ | 编译器                                  |

## 构建

```bash
# Linux / macOS
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure

# Windows (MSYS2 MINGW64)
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build

# Windows (MSVC + vcpkg)
cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
```

可选模块：

```bash
cmake -B build -DHICAL_WITH_DATABASE=ON ...    # 数据库中间件
cmake -B build -DHICAL_WITH_OPENAPI=OFF ...    # 禁用 OpenAPI
cmake -B build -DHICAL_ENABLE_REFLECTION=ON ...# C++26 反射
```

## 安装

### vcpkg（推荐）

```bash
vcpkg install hical61-hical
```

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
conan install . --build=missing
```

```cmake
find_package(hical REQUIRED)
target_link_libraries(my_app PRIVATE hical::hical_core)
```

> 详见 [集成指南](docs/integration_guide.md)。

## 文档

- [快速上手](docs/quickstart.md)
- [构建与测试](docs/build_and_test_guide.md)
- [示例指南](docs/examples_guide.md)
- [API 参考](docs/api_reference.md)
- [架构设计](docs/architecture.md)
- [集成指南](docs/integration_guide.md)
- [性能报告](docs/performance_report.md)
- [性能分析实战指南](docs/perf-analysis-guide.md)
- [贡献指南](CONTRIBUTING.md)

## 许可证

[MIT](LICENSE)
