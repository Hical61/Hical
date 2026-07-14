# hical

[![CI](https://github.com/Hical61/Hical/actions/workflows/ci.yml/badge.svg)](https://github.com/Hical61/Hical/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![C++ Standard](https://img.shields.io/badge/C%2B%2B-20%20%7C%2026-blue.svg)](https://isocpp.org/)
[![Platform](https://img.shields.io/badge/Platform-Linux%20%7C%20Windows%20%7C%20macOS-green.svg)]()
[![Boost](https://img.shields.io/badge/Boost-%E2%89%A51.82-orange.svg)](https://www.boost.org/)
[![GitHub release](https://img.shields.io/github/v/release/Hical61/Hical?include_prereleases&sort=semver)](https://github.com/Hical61/Hical/releases)
[![GitHub stars](https://img.shields.io/github/stars/Hical61/Hical?style=flat)](https://github.com/Hical61/Hical/stargazers)
[![PRs Welcome](https://img.shields.io/badge/PRs-welcome-brightgreen.svg)](CONTRIBUTING.md)

> 📖 升级迁移指南请查阅 [CHANGELOG](./CHANGELOG.md)  
> 💬 问题或建议请开 [Discussion](https://github.com/Hical61/Hical/discussions)

**Hical** 是一个基于 Boost.Asio 的现代 C++20/26 高性能 Web 框架，采用原生 HTTP/WebSocket 网络栈（picohttpparser + 自研 WebSocket），利用 C++26 反射和 PMR 内存池实现极致性能。

> **项目状态**：Stable — 生产可用，遵循 [语义化版本](https://semver.org/lang/zh-CN/)。

[English](README.md) | 简体中文

## 为什么选择 Hical？

| 对比维度 | Hical | Drogon | Cinatra | Crow / oatpp |
|----------|-------|--------|---------|--------------|
| C++ 标准 | C++20/26 | C++17 | C++20 | C++11/14 |
| 反射/自动路由 | 原生 C++26 反射 + C++20 宏双轨 | 宏注册 | 宏注册 | 手动注册 |
| 内存模型 | ReadBufferPool + 栈缓冲 → 请求级零堆分配 | 传统 allocator | 传统 allocator | 传统 allocator |
| HTTP 解析 | picohttpparser 零拷贝（栈上 64 header） | 自研解析器 | picohttpparser | 自研 / http-parser |
| WebSocket | 自研 + permessage-deflate + Hub 广播 | 内置 | 内置 | 第三方库 |
| 协程模型 | `asio::awaitable<T>` 原生协程 | 自研协程 | `asio::awaitable` | 无 / 线程池 |

## 核心特性

- **原生网络栈** — picohttpparser 零拷贝 HTTP 解析 + 自研 WebSocket (RFC 6455)，零堆分配
- **C++26 反射** — 自动路由注册 + JSON 序列化；C++20 宏无缝回退
- **协程异步 I/O** — `asio::awaitable<T>` + `co_await`，同步快速路径零协程帧开销
- **PMR 三层内存池** — 全局同步池 / 线程本地无锁池（连接级分配）；请求路径用 ReadBufferPool + 栈数组 + FixedBuffer 实现零堆分配
- **全功能中间件** — 洋葱模型、CORS、Session、日志、OpenAPI 3.0 文档生成
- **WebSocket** — permessage-deflate 压缩、Hub 广播、子协议协商、心跳
- **数据库中间件** — 协程连接池 + 自动事务 + 慢查询检测（Boost.MySQL）
- **生产就绪** — SSL/TLS、SO_REUSEPORT 多 acceptor、静态文件 ETag/304、Docker 部署

> 完整特性列表见 [架构设计](docs/architecture.md)。

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

> 完整教程见 [快速上手](docs/quickstart_cn.md)，更多示例见 [examples/](examples/)。

## 性能

原生 HTTP/WebSocket 网络栈，关键路径零堆分配，同步快速路径路由分发 ~40-130 ns/req。

```bash
# Docker 一键对比压测（Hical vs Drogon / Cinatra / Crow / Gin / Actix 等）
cd benchmark && docker compose up
```

> 具体 QPS 数据因硬件差异较大，请在目标环境运行 benchmark 获取实际数值。详见 [性能报告](docs/performance_report.md)。

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

### Docker

```bash
# 生产部署
docker compose -f docker/prod/docker-compose.yml up -d
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

> 国内用户如遇 GitHub 下载困难，可使用 [GitHub Releases 镜像](https://ghproxy.com/) 或从 Release 附件手动下载。
> 详见 [集成指南](docs/integration_guide.md)。

## 文档

- [快速上手](docs/quickstart_cn.md)
- [构建与测试](docs/build_and_test_guide.md)
- [示例指南](docs/examples_guide.md)
- [API 参考](docs/api_reference.md)
- [架构设计](docs/architecture.md)
- [集成指南](docs/integration_guide.md)
- [性能报告](docs/performance_report.md)
- [性能分析实战指南](docs/perf-analysis-guide.md)
- [变更日志](CHANGELOG.md)
- [贡献指南](CONTRIBUTING.md)

## 参与贡献

欢迎贡献代码！基本流程：

1. Fork 本仓库
2. 创建特性分支 (`git checkout -b feat/my-feature`)
3. 确保通过 `clang-format` 格式检查和测试
4. 提交 Pull Request

详见 [CONTRIBUTING.md](CONTRIBUTING.md)。

## 联系方式

- 邮箱：hical0601@gmail.com
- GitHub Issues：[提交问题](https://github.com/Hical61/Hical/issues)

## 许可证

[MIT](LICENSE)
