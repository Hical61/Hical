# hical

[![CI](https://github.com/Hical61/Hical/actions/workflows/ci.yml/badge.svg)](https://github.com/Hical61/Hical/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![C++ Standard](https://img.shields.io/badge/C%2B%2B-20%20%7C%2026-blue.svg)](https://isocpp.org/)
[![Platform](https://img.shields.io/badge/Platform-Linux%20%7C%20Windows%20%7C%20macOS-green.svg)]()
[![Boost](https://img.shields.io/badge/Boost-%E2%89%A51.82-orange.svg)](https://www.boost.org/)
[![GitHub release](https://img.shields.io/github/v/release/Hical61/Hical?include_prereleases&sort=semver)](https://github.com/Hical61/Hical/releases)
[![GitHub stars](https://img.shields.io/github/stars/Hical61/Hical?style=flat)](https://github.com/Hical61/Hical/stargazers)
[![PRs Welcome](https://img.shields.io/badge/PRs-welcome-brightgreen.svg)](CONTRIBUTING.md)

> 📖 Upgrade/migration guide → [CHANGELOG](./CHANGELOG.md)  
> 💬 Questions or ideas → [Discussion](https://github.com/Hical61/Hical/discussions)

**Hical** is a modern C++20/26 high-performance web framework built on Boost.Asio with a native HTTP/WebSocket stack (picohttpparser + self-developed WebSocket), leveraging C++26 reflection and PMR memory pooling for maximum throughput.

> **Status**: Stable — production-ready, follows [Semantic Versioning](https://semver.org/).

English | [简体中文](README_CN.md)

## Why Hical?

| Dimension | Hical | Drogon | Cinatra | Crow / oatpp |
|-----------|-------|--------|---------|--------------|
| C++ Standard | C++20/26 | C++17 | C++20 | C++11/14 |
| Reflection / Auto-routing | Native C++26 reflection + C++20 macro dual-track | Macro registration | Macro registration | Manual registration |
| Memory Model | ReadBufferPool + stack buffers → zero heap alloc per request | Traditional allocator | Traditional allocator | Traditional allocator |
| HTTP Parsing | picohttpparser zero-copy (stack-based 64 headers) | Custom parser | picohttpparser | Custom / http-parser |
| WebSocket | Self-developed + permessage-deflate + Hub broadcast | Built-in | Built-in | Third-party |
| Coroutine Model | `asio::awaitable<T>` native coroutines | Custom coroutines | `asio::awaitable` | None / thread pool |

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

## Performance

Native HTTP/WebSocket stack with zero heap allocation on critical paths, sync fast path route dispatch ~40-130 ns/req.

```bash
# Docker one-click comparative benchmark (Hical vs Drogon / Cinatra / Crow / Gin / Actix etc.)
cd benchmark && docker compose up
```

> Actual QPS varies by hardware — run the benchmark on your target environment for real numbers. See [Performance Report](docs/performance_report.md).

## Requirements

| Dependency                       | Notes                                           |
| -------------------------------- | ----------------------------------------------- |
| C++20/26                         | C++26 optional (reflection)                     |
| Boost >= 1.82                    | Asio, System, JSON; DB middleware needs >= 1.85 |
| OpenSSL                          | Required                                        |
| zlib                             | Required (WebSocket compression)                |
| CMake >= 3.20                    | Build system                                    |
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
