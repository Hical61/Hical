# hical

[![CI](https://github.com/Hical61/Hical/actions/workflows/ci.yml/badge.svg)](https://github.com/Hical61/Hical/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![C++ Standard](https://img.shields.io/badge/C%2B%2B-20%20%7C%2026-blue.svg)](https://isocpp.org/)
[![Platform](https://img.shields.io/badge/Platform-Linux%20%7C%20Windows%20%7C%20macOS-green.svg)]()
[![Boost](https://img.shields.io/badge/Boost-%E2%89%A51.82-orange.svg)](https://www.boost.org/)
[![GitHub release](https://img.shields.io/github/v/release/Hical61/Hical?include_prereleases&sort=semver)](https://github.com/Hical61/Hical/releases)
[![GitHub stars](https://img.shields.io/github/stars/Hical61/Hical?style=flat)](https://github.com/Hical61/Hical/stargazers)
[![PRs Welcome](https://img.shields.io/badge/PRs-welcome-brightgreen.svg)](CONTRIBUTING.md)
[![HttpArena H/1.1](https://img.shields.io/endpoint?url=https://http-arena.com/badge/hical/h1.json)](https://http-arena.com/frameworks/hical/)
[![HttpArena WebSocket](https://img.shields.io/endpoint?url=https://http-arena.com/badge/hical/ws.json)](https://http-arena.com/frameworks/hical/)

> 📖 升级迁移指南请查阅 [CHANGELOG](./CHANGELOG.md)  
> 💬 问题或建议请开 [Discussion](https://github.com/Hical61/Hical/discussions)

**Hical** 是一个基于 Boost.Asio 的现代 C++20/26 高性能 Web 框架，采用原生 HTTP/WebSocket 网络栈（picohttpparser + 自研 WebSocket），利用 C++26 反射和 PMR 内存池实现极致性能。

> **项目状态**：Stable — 生产可用，遵循 [语义化版本](https://semver.org/lang/zh-CN/)。

[English](README.md) | 简体中文

## 为什么选择 Hical？

下面所有数字都来自 [HttpArena](https://http-arena.com/frameworks/hical/)——一个把各家框架放在同一台机器、同一轮次、同一个压测工具下跑的独立榜单。不用自己的硬件，也不挑场景。

### 吞吐与并发 —— 全部 profile

| 分类          | Profile             |   连接 |                 吞吐 | 平均延迟 |       p99 |     CPU |    内存 |
| :------------ | :------------------ | -----: | -------------------: | -------: | --------: | ------: | ------: |
| **连接**      | `baseline`          |  4,096 |      3,202,325 req/s |  1.28 ms |   1.80 ms | 6288.6% | 133 MiB |
|               | `pipelined`         |  4,096 | **18,365,348 req/s** |  3.57 ms |   5.72 ms | 6626.4% | 148 MiB |
|               | `limited-conn`      |  4,096 |      2,385,743 req/s |  1.70 ms |  25.00 ms | 6185.3% |  73 MiB |
| **并发**      | `async`（等 10 ms） | 32,000 |      1,920,543 req/s | 16.52 ms |  19.00 ms | 6428.7% | 900 MiB |
| **负载**      | `json-comp`         |  4,096 |        395,704 req/s | 10.18 ms | 235.90 ms | 6284.4% |  78 MiB |
|               | `json-comp`         | 16,384 |        381,469 req/s | 39.04 ms | 997.90 ms | 6530.0% | 214 MiB |
|               | `json-tls`          |  4,096 |        895,583 req/s |  4.56 ms |  63.82 ms | 6731.7% | 518 MiB |
|               | `8gbit`（流式回显） |    512 |         49,350 req/s | 103.9 µs |  147.0 µs |  287.4% | 107 MiB |
| **WebSocket** | `echo-ws`           |    512 |      3,460,015 msg/s |   147 µs |    296 µs | 6313.2% |  47 MiB |
|               | `echo-ws`           |  4,096 |      3,503,912 msg/s |  1.17 ms |   1.75 ms | 6608.7% | 158 MiB |
|               | `echo-ws`           | 16,384 |      3,062,490 msg/s |  5.31 ms |   6.48 ms | 6591.1% | 533 MiB |
|               | `echo-ws-pipeline`  |    512 | **37,055,516 msg/s** |   220 µs |    384 µs | 6620.9% |  47 MiB |
|               | `echo-ws-pipeline`  |  4,096 | **36,420,220 msg/s** |  1.80 ms |   2.19 ms | 6740.8% | 159 MiB |
|               | `echo-ws-pipeline`  | 16,384 | **32,132,291 msg/s** |  8.12 ms |   9.39 ms | 6718.7% | 533 MiB |
|               | `echo-ws-limited`   |    512 |      1,866,258 msg/s |   185 µs |    560 µs | 5741.8% |  81 MiB |
|               | `echo-ws-limited`   |  4,096 |      2,192,315 msg/s |   540 µs |   1.01 ms | 6461.4% | 168 MiB |

### 延迟与 CPU 效率 —— 固定速率

和上面的峰值吞吐不同，这几个 profile 把**供给速率钉死**，能扛住的机器交付的 RPS 都一样，所以排名完全取决于**每请求 CPU 成本**（从 cgroup v2 的 `cpu.stat` 精确读出，不是采样）：

| Profile             |                 钉住的速率 | 实际交付 |   CPU / 请求 |     CPU | 平均延迟 |      p99 |
| :------------------ | -------------------------: | -------: | -----------: | ------: | -------: | -------: |
| `latency-1m`        |            1,000,000 req/s |    99.8% | **22.55 µs** | 2460.8% |  94.5 µs | 175.0 µs |
| `latency-10k`       |               10,000 req/s |    99.8% | **29.78 µs** |   28.5% |  80.9 µs | 112.0 µs |
| `latency-500k-8cpu` | 500,000 req/s，4 核 8 线程 |    86.1% | **18.52 µs** |  796.2% |   1.25 s |   4.98 s |

### 几个结论

- **流水线 HTTP/1.1：1837 万 req/s。** 一次读出来的多个请求共用一次写，而不是一个请求一趟系统调用。
- **批量 WebSocket：3642 万 msg/s。** 同样的思路用在帧上。
- **内存是差异点。** 4096 条并发连接只占 **133 MiB**；同一轮里 HTTP/1.1 复合分第一的那个框架要 4.3 GiB，差 30 倍以上。来源是三件事：PMR 三层内存池、连接级 `ReadBufferPool`，以及「空闲连接不持有任何读缓冲」。
- **满载时也便宜。** 扛住钉死的 100 万 req/s 时，一个请求在所有线程上合计只花 22.55 µs CPU 时间——直接从容器 cgroup 的 `cpu.stat` 读出来，不是采样。

### 自己复现

上面的数字来自 HttpArena 的受控环境，你自己的硬件跑出来会不一样。想在本地跑同类型的对比（含 Drogon / Cinatra / Crow / Gin / Actix）：

```bash
cd benchmark && docker compose up
```

- 实时榜单：<https://http-arena.com/frameworks/hical/>
- 压测实现：[docker/HttpArena](docker/HttpArena/)
- 方法论：[性能报告](docs/performance_report.md)

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

## 依赖

| 依赖项                           | 说明                                    |
| -------------------------------- | --------------------------------------- |
| C++20/26                         | C++26 可选（反射）                      |
| Boost >= 1.82                    | Asio, System, JSON；DB 中间件需 >= 1.85 |
| OpenSSL                          | 必需                                    |
| zlib                             | 必需（WebSocket 压缩）                  |
| CMake >= 3.28                    | 构建系统                                |
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
