# 快速上手

> 5 分钟搭建你的第一个 hical HTTP 服务器。

[English](quickstart.md) | 简体中文

---

## 1. 环境准备

| 组件       | 最低版本                                              |
| ---------- | ----------------------------------------------------- |
| C++ 编译器 | GCC 14+ / Clang 20+ / MSVC 2022+（需支持 C++20 协程） |
| CMake      | 3.20+                                                 |
| Boost      | 1.82+（Asio / JSON）；DB 中间件 >= 1.85               |
| OpenSSL    | 3.0+                                                  |

详细环境搭建步骤请参考 [编译与测试指南](build_and_test_guide.md)。

<details>
<summary><b>Windows（MSYS2 MINGW64）</b></summary>

```bash
pacman -Syu
pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-cmake mingw-w64-x86_64-ninja \
          mingw-w64-x86_64-boost mingw-w64-x86_64-openssl mingw-w64-x86_64-gtest
```

将 `C:\msys64\mingw64\bin` 添加到系统 PATH。

</details>

<details>
<summary><b>Ubuntu / Debian</b></summary>

```bash
sudo apt update
sudo apt install -y build-essential g++ cmake ninja-build \
                    libboost-all-dev libssl-dev libgtest-dev
```

> Ubuntu 22.04+ 开箱即用。Ubuntu 20.04 需升级 GCC，详见 [编译与测试指南](build_and_test_guide.md)。

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
<summary><b>macOS（Homebrew）</b></summary>

```bash
brew install cmake ninja boost openssl@3 googletest
```

> macOS 自带 LibreSSL 而非 OpenSSL，编译时需指定路径（见下方编译命令）。

</details>

---

## 2. 获取 hical

### 方式 A：vcpkg（推荐）

```bash
# 通过 overlay port 安装
vcpkg install hical61-hical --overlay-ports=path/to/hical/ports

# 启用数据库中间件
vcpkg install hical61-hical[database] --overlay-ports=path/to/hical/ports
```

在 `CMakeLists.txt` 中：

```cmake
find_package(hical CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE hical::hical_core)
```

### 方式 B：Conan

```bash
# 从 GitHub Releases 下载源码包
curl -LO https://github.com/Hical61/Hical/releases/download/v2.6.7/hical-2.6.7-conan-src.tar.gz
tar xzf hical-2.6.7-conan-src.tar.gz && cd hical
conan export . --version=2.6.7
```

在消费者项目中：

```cmake
find_package(hical REQUIRED)
target_link_libraries(my_app PRIVATE hical::hical_core)
```

### 方式 C：源码（作为子目录）

```bash
git clone https://github.com/Hical61/Hical.git hical
```

在 `CMakeLists.txt` 中：

```cmake
add_subdirectory(hical)
target_link_libraries(my_app PRIVATE hical_core)
```

---

## 3. Hello World

创建两个文件：

**CMakeLists.txt：**

```cmake
cmake_minimum_required(VERSION 3.20)
project(my_server LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 二选一：
# A) vcpkg / Conan — 查找已安装的包
find_package(hical CONFIG REQUIRED)
# B) 源码子目录
# add_subdirectory(hical)

add_executable(my_server main.cpp)
target_link_libraries(my_server PRIVATE hical::hical_core)
```

**main.cpp：**

```cpp
#include "core/HttpServer.h"

using namespace hical;

int main()
{
    HttpServer server(8080);

    // 同步处理器 — 直接返回 HttpResponse
    server.router().get("/", [](const HttpRequest&) -> HttpResponse {
        return HttpResponse::ok("Hello, hical!");
    });

    server.start();
    return 0;
}
```

**编译运行：**

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

**测试：**

```bash
curl http://localhost:8080/
# 输出: Hello, hical!
```

---

## 4. 路由与中间件

### 路由

```cpp
// JSON 响应
server.router().get("/api/status", [](const HttpRequest&) -> HttpResponse {
    return HttpResponse::json({{"status", "running"}, {"version", "1.0.0"}});
});

// 读取请求体
server.router().post("/api/echo", [](const HttpRequest& req) -> HttpResponse {
    return HttpResponse::ok(req.body());
});

// 路径参数
server.router().get("/users/{id}", [](const HttpRequest& req) -> HttpResponse {
    auto userId = req.param("id");
    return HttpResponse::json({{"userId", userId}});
});
```

### 中间件（洋葱模型）

```cpp
// 日志中间件
server.use([](HttpRequest& req, MiddlewareNext next) -> Awaitable<HttpResponse> {
    std::cout << httpMethodToString(req.method()) << " " << req.path() << std::endl;
    auto res = co_await next(req);
    std::cout << "  -> " << static_cast<int>(res.statusCode()) << std::endl;
    co_return res;
});
```

---

## 5. 下一步

| 主题         | 文档                                          |
| ------------ | --------------------------------------------- |
| WebSocket    | [使用示例](examples_guide.md)                 |
| SSL/TLS      | [集成指南](integration_guide.md)              |
| 协程处理器   | [协程指南](coroutine-guide.md)                |
| 性能调优     | [性能报告](performance_report.md)             |
| OpenAPI 生成 | [OpenAPI 指南](openapi-guide.md)              |
| 数据库中间件 | [集成指南](integration_guide.md)              |
| 完整示例     | [examples/](../examples/) — 8 个可运行的 demo |
| API 文档     | [API 参考](api_reference.md)                  |
| 架构设计     | [架构文档](architecture.md)                   |

---

## 6. 常见问题

**CMake 找不到 Boost：**

```bash
# 显式指定 Boost 路径
cmake -B build -DBOOST_ROOT=/path/to/boost ...
```

**macOS 找不到 OpenSSL：**

```bash
cmake -B build -DOPENSSL_ROOT_DIR=$(brew --prefix openssl@3) ...
```

**Windows 链接报错 "ws2_32 not found"：**

确保在 MSYS2 MINGW64 终端中编译（而非 MSYS 或 UCRT），或者如果使用 MSVC，需传入 `-DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake`。
