# hical

[![CI](https://github.com/Hical61/Hical/actions/workflows/ci.yml/badge.svg)](https://github.com/Hical61/Hical/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![C++ Standard](https://img.shields.io/badge/C%2B%2B-20%20%7C%2026-blue.svg)](https://isocpp.org/)
[![Platform](https://img.shields.io/badge/Platform-Linux%20%7C%20Windows%20%7C%20macOS-green.svg)]()
[![Boost](https://img.shields.io/badge/Boost-%E2%89%A51.82-orange.svg)](https://www.boost.org/)
[![GitHub release](https://img.shields.io/github/v/release/Hical61/Hical?include_prereleases&sort=semver)](https://github.com/Hical61/Hical/releases)
[![GitHub stars](https://img.shields.io/github/stars/Hical61/Hical?style=flat)](https://github.com/Hical61/Hical/stargazers)
[![PRs Welcome](https://img.shields.io/badge/PRs-welcome-brightgreen.svg)](CONTRIBUTING.md)

**Hical** is a modern C++20/26 high-performance web framework built on Boost.Asio with a native HTTP/WebSocket stack, leveraging C++26 reflection and PMR memory pooling for maximum throughput.

English | [简体中文](README_CN.md)

## Features

- **Native stack** — picohttpparser HTTP parsing + self-developed WebSocket (RFC 6455), performance on par with Drogon/Cinatra
- **C++26 Reflection** — Automatic route registration + JSON serialization; C++20 macro fallback
- **Coroutine async I/O** — `asio::awaitable<T>` + `co_await`, clean and efficient
- **PMR three-tier pool** — Global synchronized / thread-local lock-free / request-level monotonic buffer
- **High-performance routing** — Static route O(1) hash lookup, sync fast path with zero coroutine frame overhead
- **Onion middleware** — Async + sync dual-mode middleware, `SyncMiddleware` with zero coroutine frame
- **WebSocket** — permessage-deflate compression, Origin whitelist, fragment reassembly
- **SSL/TLS** — Template-based `GenericConnection`, compile-time branching
- **SO_REUSEPORT** — Multi-acceptor, accept and I/O on same thread
- **RouteGroup** — Prefix grouping + group-level middleware + nesting
- **CORS** — Built-in cross-origin middleware with automatic preflight
- **Cookie / Session** — RFC 6265 + session fixation prevention + atomic migration
- **Static files** — ETag/304 + async I/O + PathCache
- **Multipart** — RFC 7578 file upload with DoS protection
- **Logging** — 6-level, async double-buffered, named channels, dynamic level management
- **OpenAPI 3.0** — Auto-generate docs from macros, one-call Swagger UI setup
- **Database middleware** — Optional, coroutine connection pool + auto-transaction + slow query detection (Boost.MySQL)

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

Native HTTP/WebSocket stack with performance on par with Drogon, Cinatra, and other leading C++ frameworks. See [Performance Report](docs/performance_report.md).

```bash
# Docker one-click benchmark
docker compose -f docker-compose.bench.yml up
```

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
- [Contributing](CONTRIBUTING.md)

## License

[MIT](LICENSE)
