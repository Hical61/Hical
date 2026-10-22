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

> 📖 Upgrade/migration guide → [CHANGELOG](./CHANGELOG.md)  
> 💬 Questions or ideas → [Discussion](https://github.com/Hical61/Hical/discussions)

**Hical** is a modern C++20/26 high-performance web framework built on Boost.Asio with a native HTTP/WebSocket stack (picohttpparser + self-developed WebSocket), leveraging C++26 reflection and PMR memory pooling for maximum throughput.

> **Status**: Stable — production-ready, follows [Semantic Versioning](https://semver.org/).

English | [简体中文](README_CN.md)

## Why Hical?

Every number below is from [HttpArena](https://http-arena.com/frameworks/hical/) — an independent benchmark that runs each framework on the same machine, in the same round, against the same load generator. No vendor hardware, no hand-picked scenarios.

### Throughput & concurrency — all profiles

| Category        | Profile               |  Conns |           Throughput | Avg latency |       p99 |     CPU |  Memory |
| :-------------- | :-------------------- | -----: | -------------------: | ----------: | --------: | ------: | ------: |
| **Connection**  | `baseline`            |  4,096 |      3,202,325 req/s |     1.28 ms |   1.80 ms | 6288.6% | 133 MiB |
|                 | `pipelined`           |  4,096 | **18,365,348 req/s** |     3.57 ms |   5.72 ms | 6626.4% | 148 MiB |
|                 | `limited-conn`        |  4,096 |      2,385,743 req/s |     1.70 ms |  25.00 ms | 6185.3% |  73 MiB |
| **Concurrency** | `async` (10 ms wait)  | 32,000 |      1,920,543 req/s |    16.52 ms |  19.00 ms | 6428.7% | 900 MiB |
| **Workload**    | `json-comp`           |  4,096 |        395,704 req/s |    10.18 ms | 235.90 ms | 6284.4% |  78 MiB |
|                 | `json-comp`           | 16,384 |        381,469 req/s |    39.04 ms | 997.90 ms | 6530.0% | 214 MiB |
|                 | `json-tls`            |  4,096 |        895,583 req/s |     4.56 ms |  63.82 ms | 6731.7% | 518 MiB |
|                 | `8gbit` (stream echo) |    512 |         49,350 req/s |    103.9 µs |  147.0 µs |  287.4% | 107 MiB |
| **WebSocket**   | `echo-ws`             |    512 |      3,460,015 msg/s |      147 µs |    296 µs | 6313.2% |  47 MiB |
|                 | `echo-ws`             |  4,096 |      3,503,912 msg/s |     1.17 ms |   1.75 ms | 6608.7% | 158 MiB |
|                 | `echo-ws`             | 16,384 |      3,062,490 msg/s |     5.31 ms |   6.48 ms | 6591.1% | 533 MiB |
|                 | `echo-ws-pipeline`    |    512 | **37,055,516 msg/s** |      220 µs |    384 µs | 6620.9% |  47 MiB |
|                 | `echo-ws-pipeline`    |  4,096 | **36,420,220 msg/s** |     1.80 ms |   2.19 ms | 6740.8% | 159 MiB |
|                 | `echo-ws-pipeline`    | 16,384 | **32,132,291 msg/s** |     8.12 ms |   9.39 ms | 6718.7% | 533 MiB |
|                 | `echo-ws-limited`     |    512 |      1,866,258 msg/s |      185 µs |    560 µs | 5741.8% |  81 MiB |
|                 | `echo-ws-limited`     |  4,096 |      2,192,315 msg/s |      540 µs |   1.01 ms | 6461.4% | 168 MiB |

### Latency & CPU efficiency — fixed-rate paced load

Unlike the peak-throughput profiles, these pin the offered rate, so every server that holds it delivers the same RPS and the ranking comes down to **CPU cost per request**, measured from cgroup v2 `cpu.stat`:

| Profile             |                        Pinned rate |  Held |    CPU / req |     CPU | Mean latency |      p99 |
| :------------------ | ---------------------------------: | ----: | -----------: | ------: | -----------: | -------: |
| `latency-1m`        |                    1,000,000 req/s | 99.8% | **22.55 µs** | 2460.8% |      94.5 µs | 175.0 µs |
| `latency-10k`       |                       10,000 req/s | 99.8% | **29.78 µs** |   28.5% |      80.9 µs | 112.0 µs |
| `latency-500k-8cpu` | 500,000 req/s, 4 cores / 8 threads | 86.1% | **18.52 µs** |  796.2% |       1.25 s |   4.98 s |

### Key takeaways

- **Pipelined HTTP/1.1: 18.37 M req/s.** Every request in one read batch shares a single write, instead of one syscall each.
- **Batched WebSocket: 36.42 M msg/s.** The same idea applied to frames.
- **Memory is the differentiator.** 4,096 concurrent connections cost **133 MiB**. The HTTP/1.1 composite leader needs 4.3 GiB on the same run — a gap of over 30× that comes from the three-tier PMR pool, the connection-level `ReadBufferPool`, and the rule that an idle connection holds no read buffer at all.
- **Cheap at saturation.** Holding a pinned 1 M req/s, one request costs 22.55 µs of CPU time summed across every thread — read straight out of the container's cgroup `cpu.stat`, not sampled.

### Reproduce it

The numbers above come from HttpArena's controlled environment — your own hardware will produce different ones. To run the same kind of comparison locally, including Drogon / Cinatra / Crow / Gin / Actix:

```bash
cd benchmark && docker compose up
```

- Live board: <https://http-arena.com/frameworks/hical/>
- Benchmark implementation: [docker/HttpArena](docker/HttpArena/)
- Methodology: [Performance Report](docs/performance_report.md)

## Core Features

- **Native network stack** — picohttpparser zero-copy HTTP parsing + self-developed WebSocket (RFC 6455), zero heap allocation
- **C++26 Reflection** — Automatic route registration + JSON serialization; seamless C++20 macro fallback
- **Coroutine async I/O** — `asio::awaitable<T>` + `co_await`, sync fast path with zero coroutine frame overhead
- **PMR three-tier pool** — Global synchronized / thread-local lock-free pool for connection-level allocations; request path uses ReadBufferPool + stack arrays + FixedBuffer for actual zero heap allocation
- **Full-featured middleware** — Onion model, CORS, Session, Logging, OpenAPI 3.0 doc generation
- **WebSocket** — permessage-deflate compression, Hub broadcast, subprotocol negotiation, heartbeat
- **Database middleware** — Coroutine connection pool + auto-transaction + slow query detection (Boost.MySQL)
- **Production-ready** — SSL/TLS, SO_REUSEPORT multi-acceptor, static file ETag/304, Docker deployment

> Full feature list in [Architecture](docs/architecture.md).

## Quick Start

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

> Full tutorial at [docs/quickstart.md](docs/quickstart.md), more examples in [examples/](examples/).

## Requirements

| Dependency                       | Notes                                           |
| -------------------------------- | ----------------------------------------------- |
| C++20/26                         | C++26 optional (reflection)                     |
| Boost >= 1.82                    | Asio, System, JSON; DB middleware needs >= 1.85 |
| OpenSSL                          | Required                                        |
| zlib                             | Required (WebSocket compression)                |
| CMake >= 3.28                    | Build system                                    |
| GCC 14+ / Clang 20+ / MSVC 2022+ | Compiler                                        |

## Build

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

Optional modules:

```bash
cmake -B build -DHICAL_WITH_DATABASE=ON ...    # Database middleware
cmake -B build -DHICAL_WITH_OPENAPI=OFF ...    # Disable OpenAPI
cmake -B build -DHICAL_ENABLE_REFLECTION=ON ...# C++26 Reflection
```

### Docker

```bash
# Production deployment
docker compose -f docker/prod/docker-compose.yml up -d
```

## Installation

### vcpkg (Recommended)

```bash
vcpkg install hical61-hical
```

```cmake
find_package(hical CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE hical::hical_core)
```

### Conan

Download the Conan source package from [GitHub Releases](https://github.com/Hical61/Hical/releases) and export to local cache:

```bash
# Download & extract (replace VERSION with actual version)
curl -LO https://github.com/Hical61/Hical/releases/download/vVERSION/hical-VERSION-conan-src.tar.gz
tar xzf hical-VERSION-conan-src.tar.gz

# Export to local Conan cache
cd hical
conan export . --version=VERSION
conan install . --build=missing
```

```cmake
find_package(hical REQUIRED)
target_link_libraries(my_app PRIVATE hical::hical_core)
```

> See [Integration Guide](docs/integration_guide.md) for details.

## Documentation

- [Quick Start](docs/quickstart.md)
- [Build & Test](docs/build_and_test_guide.md)
- [Examples Guide](docs/examples_guide.md)
- [API Reference](docs/api_reference.md)
- [Architecture](docs/architecture.md)
- [Integration Guide](docs/integration_guide.md)
- [Performance Report](docs/performance_report.md)
- [Performance Analysis Guide](docs/perf-analysis-guide.md)
- [Changelog](CHANGELOG.md)
- [Contributing](CONTRIBUTING.md)

## Contributing

Contributions are welcome! Basic workflow:

1. Fork this repository
2. Create a feature branch (`git checkout -b feat/my-feature`)
3. Ensure `clang-format` checks and tests pass
4. Submit a Pull Request

See [CONTRIBUTING.md](CONTRIBUTING.md) for details.

## Contact

- Email: hical0601@gmail.com
- GitHub Issues: [Report a problem](https://github.com/Hical61/Hical/issues)

## License

[MIT](LICENSE)
