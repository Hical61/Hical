# Quick Start

> Get your first hical HTTP server running in 5 minutes.

English | [简体中文](quickstart_cn.md)

---

## 1. Prerequisites

| Component    | Minimum Version                                            |
| ------------ | ---------------------------------------------------------- |
| C++ Compiler | GCC 14+ / Clang 20+ / MSVC 2022+ (C++20 coroutine support) |
| CMake        | 3.20+                                                      |
| Boost        | 1.82+ (Asio / JSON); DB middleware requires ≥ 1.85         |
| OpenSSL      | 3.0+                                                       |

For detailed setup instructions, see [Build & Test Guide](build_and_test_guide.md).

<details>
<summary><b>Windows (MSYS2 MINGW64)</b></summary>

```bash
pacman -Syu
pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-cmake mingw-w64-x86_64-ninja \
          mingw-w64-x86_64-boost mingw-w64-x86_64-openssl mingw-w64-x86_64-gtest
```

Add `C:\msys64\mingw64\bin` to your system PATH.

</details>

<details>
<summary><b>Ubuntu / Debian</b></summary>

```bash
sudo apt update
sudo apt install -y build-essential g++ cmake ninja-build \
                    libboost-all-dev libssl-dev libgtest-dev
```

> Ubuntu 22.04+ works out of the box. Ubuntu 20.04 requires a GCC upgrade — see [Build & Test Guide](build_and_test_guide.md).

</details>

<details>
<summary><b>Fedora / RHEL</b></summary>

```bash
sudo dnf install -y gcc-c++ cmake ninja-build \
                    boost-devel openssl-devel gtest-devel
```

</details>

<details>
<summary><b>Arch Linux</b></summary>

```bash
sudo pacman -S gcc cmake ninja boost openssl gtest
```

</details>

<details>
<summary><b>macOS (Homebrew)</b></summary>

```bash
brew install cmake ninja boost openssl@3 googletest
```

> macOS ships LibreSSL instead of OpenSSL — you'll need to pass the OpenSSL path at build time (see below).

</details>

---

## 2. Installation

### Option A: vcpkg (Recommended)

```bash
# Install with overlay port
vcpkg install hical61-hical --overlay-ports=path/to/hical/ports

# With database middleware
vcpkg install hical61-hical[database] --overlay-ports=path/to/hical/ports
```

In your `CMakeLists.txt`:

```cmake
find_package(hical CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE hical::hical_core)
```

### Option B: Conan

```bash
# Download source package from GitHub Releases
curl -LO https://github.com/Hical61/Hical/releases/download/v2.6.7/hical-2.6.7-conan-src.tar.gz
tar xzf hical-2.6.7-conan-src.tar.gz && cd hical
conan export . --version=2.6.7
```

In your consumer project:

```cmake
find_package(hical REQUIRED)
target_link_libraries(my_app PRIVATE hical::hical_core)
```

### Option C: Source (as subdirectory)

```bash
git clone https://github.com/Hical61/Hical.git hical
```

In your `CMakeLists.txt`:

```cmake
add_subdirectory(hical)
target_link_libraries(my_app PRIVATE hical_core)
```

---

## 3. Hello World

Create two files:

**CMakeLists.txt:**

```cmake
cmake_minimum_required(VERSION 3.20)
project(my_server LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Choose ONE of:
# A) vcpkg / Conan — find installed package
find_package(hical CONFIG REQUIRED)
# B) Source subdirectory
# add_subdirectory(hical)

add_executable(my_server main.cpp)
target_link_libraries(my_server PRIVATE hical::hical_core)
```

**main.cpp:**

```cpp
#include "core/HttpServer.h"

using namespace hical;

int main()
{
    HttpServer server(8080);

    // Sync handler — returns HttpResponse directly
    server.router().get("/", [](const HttpRequest&) -> HttpResponse {
        return HttpResponse::ok("Hello, hical!");
    });

    server.start();
    return 0;
}
```

**Build & run:**

```bash
# Windows (MSYS2)
cmake -B build -G Ninja
cmake --build build
./build/my_server.exe

# Linux
cmake -B build -G Ninja
cmake --build build
./build/my_server

# macOS (Homebrew)
cmake -B build -G Ninja -DOPENSSL_ROOT_DIR=$(brew --prefix openssl@3)
cmake --build build
./build/my_server
```

**Test:**

```bash
curl http://localhost:8080/
# Output: Hello, hical!
```

---

## 4. Routes & Middleware

### Routes

```cpp
// JSON response
server.router().get("/api/status", [](const HttpRequest&) -> HttpResponse {
    return HttpResponse::json({{"status", "running"}, {"version", "1.0.0"}});
});

// Read request body
server.router().post("/api/echo", [](const HttpRequest& req) -> HttpResponse {
    return HttpResponse::ok(req.body());
});

// Path parameters
server.router().get("/users/{id}", [](const HttpRequest& req) -> HttpResponse {
    auto userId = req.param("id");
    return HttpResponse::json({{"userId", userId}});
});
```

### Middleware (Onion Model)

```cpp
// Logging middleware
server.use([](HttpRequest& req, MiddlewareNext next) -> Awaitable<HttpResponse> {
    std::cout << httpMethodToString(req.method()) << " " << req.path() << std::endl;
    auto res = co_await next(req);
    std::cout << "  -> " << static_cast<int>(res.statusCode()) << std::endl;
    co_return res;
});
```

---

## 5. What's Next

| Topic               | Guide                                        |
| ------------------- | -------------------------------------------- |
| WebSocket           | [Examples Guide](examples_guide.md)          |
| SSL/TLS             | [Integration Guide](integration_guide.md)    |
| Coroutine handlers  | [Coroutine Guide](coroutine-guide.md)        |
| Performance tuning  | [Performance Report](performance_report.md)  |
| OpenAPI generation  | [OpenAPI Guide](openapi-guide.md)            |
| Database middleware | [Integration Guide](integration_guide.md)    |
| Full examples       | [examples/](../examples/) — 8 runnable demos |
| API reference       | [API Reference](api_reference.md)            |
| Architecture        | [Architecture](architecture.md)              |

---

## 6. Troubleshooting

**CMake can't find Boost:**

```bash
# Specify Boost root explicitly
cmake -B build -DBOOST_ROOT=/path/to/boost ...
```

**macOS OpenSSL not found:**

```bash
cmake -B build -DOPENSSL_ROOT_DIR=$(brew --prefix openssl@3) ...
```

**Windows: "ws2_32 not found" or linker errors:**

Ensure you're building in MSYS2 MINGW64 shell (not MSYS or UCRT), or if using MSVC, pass `-DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake`.
