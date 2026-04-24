# Hical 快速上手指南

> 5 分钟搭建你的第一个 hical HTTP 服务器

---

## 1. 环境准备

详细的环境搭建步骤请参考 [编译与测试指南](build_and_test_guide.md)。

**最低要求：**

| 组件       | 版本                                                  |
| ---------- | ----------------------------------------------------- |
| C++ 编译器 | GCC 14+ / Clang 20+ / MSVC 2022+（需支持 C++20 协程） |
| CMake      | 3.20+                                                 |
| Boost      | 1.78+（Asio / Beast / JSON）                          |
| OpenSSL    | 3.0+                                                  |

**快速安装：**

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

> macOS 自带 LibreSSL 而非 OpenSSL，编译时需指定路径，详见下方编译命令。

</details>

---

## 2. 项目结构

```
your_project/
├── CMakeLists.txt
└── main.cpp
```

**CMakeLists.txt：**

```cmake
cmake_minimum_required(VERSION 3.20)
project(my_server LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Boost 1.78 REQUIRED COMPONENTS system json)
find_package(OpenSSL REQUIRED)

# 假设 hical 安装在 HICAL_DIR 或作为子目录引入
add_subdirectory(path/to/hical)

add_executable(my_server main.cpp)
target_link_libraries(my_server PRIVATE hical_core)
```

---

## 3. 第一个 HTTP 服务器

**main.cpp：**

```cpp
#include "core/HttpServer.h"

using namespace hical;

int main()
{
    // 创建服务器，监听 8080 端口
    HttpServer server(8080);

    // 注册 GET / 路由
    server.router().get("/", [](const HttpRequest&) -> HttpResponse {
        return HttpResponse::ok("Hello, hical!");
    });

    // 启动服务器（阻塞）
    server.start();
    return 0;
}
```

**编译运行：**

```bash
# Windows (MSYS2)
cmake -B build -G "Ninja" -DCMAKE_PREFIX_PATH=C:/msys64/mingw64
cmake --build build
./build/my_server.exe

# Linux (Ubuntu / Fedora / Arch)
cmake -B build -G "Ninja"
cmake --build build
./build/my_server

# macOS (Homebrew)
cmake -B build -G "Ninja" -DOPENSSL_ROOT_DIR=$(brew --prefix openssl@3)
cmake --build build
./build/my_server
```

**测试：**

```bash
curl http://localhost:8080/
# 输出: Hello, hical!
```

---

## 4. 添加路由

hical 支持 GET / POST / PUT / DELETE 等 HTTP 方法，以及路径参数和查询参数。

```cpp
// GET 路由 + JSON 响应
server.router().get("/api/status", [](const HttpRequest&) -> HttpResponse {
    return HttpResponse::json({
        {"status", "running"},
        {"version", "1.0.0"}
    });
});

// POST 路由 + 读取请求体
server.router().post("/api/echo", [](const HttpRequest& req) -> HttpResponse {
    return HttpResponse::ok(req.body());
});

// 路径参数：{id} 会被自动提取
server.router().get("/users/{id}", [](const HttpRequest& req) -> HttpResponse {
    auto userId = req.param("id");
    return HttpResponse::json({{"userId", userId}});
});

// 查询参数
server.router().get("/search", [](const HttpRequest& req) -> HttpResponse {
    auto query = req.query();  // "?keyword=hello" -> "keyword=hello"
    return HttpResponse::ok("搜索: " + query);
});
```

**也可以使用 HICAL_ROUTE 宏：**

```cpp
HICAL_ROUTE(server.router(), Get, "/api/hello", myHandler);
```

---

## 5. 中间件

中间件采用洋葱模型，按注册顺序执行。每个中间件可以在调用 `next` 前后添加逻辑。

```cpp
// 日志中间件
server.use([](const HttpRequest& req, MiddlewareNext next)
               -> Awaitable<HttpResponse> {
    std::cout << httpMethodToString(req.method()) << " " << req.path() << std::endl;
    auto res = co_await next(req);  // 调用下一层
    std::cout << "  -> " << static_cast<int>(res.statusCode()) << std::endl;
    co_return res;
});

// 认证中间件（拦截未授权请求）
server.use([](const HttpRequest& req, MiddlewareNext next)
               -> Awaitable<HttpResponse> {
    if (req.header("Authorization").empty())
    {
        co_return HttpResponse::badRequest("需要认证");
    }
    co_return co_await next(req);
});
```

---

## 6. WebSocket

```cpp
#include "core/WebSocket.h"

// 注册 WebSocket 路由
server.router().ws("/ws/chat",
    // 消息回调
    [](const std::string& msg, WebSocketSession& ws) -> Awaitable<void> {
        co_await ws.send("收到: " + msg);
    },
    // 连接建立回调（可选）
    [](WebSocketSession& ws) -> Awaitable<void> {
        co_await ws.send("欢迎连接!");
    }
);
```

**测试 WebSocket：**

```bash
# 需要 wscat: npm install -g wscat
wscat -c ws://localhost:8080/ws/chat
```

---

## 7. SSL/TLS

```cpp
// 启用 SSL
server.enableSsl("server.crt", "server.key");
server.start();
```

**生成自签名证书（测试用）：**

```bash
openssl req -x509 -newkey rsa:2048 -keyout server.key -out server.crt \
    -days 365 -nodes -subj "/CN=localhost"
```

---

## 8. 协程风格处理器

路由处理器可以使用协程，支持异步操作：

```cpp
server.router().get("/async", [](const HttpRequest&) -> Awaitable<HttpResponse> {
    // 协程内可以使用 co_await
    co_await hical::sleep(0.1);  // 模拟异步操作
    co_return HttpResponse::ok("异步完成");
});
```

---

## 9. 性能调优建议

### 多线程

```cpp
// 使用 4 个 IO 线程
HttpServer server(8080, 4);
```

### pmr 内存池

hical 内置三层 pmr 内存池，默认启用：

- **全局同步池** — 跨线程共享
- **线程本地池** — thread_local 无锁访问
- **请求级单调池** — HTTP 请求结束后整体释放

可通过 `PoolConfig` 调整参数：

```cpp
PoolConfig config;
config.requestPoolInitialSize = 8192;  // 请求池初始大小
config.threadLocalMaxBlocksPerChunk = 128;
MemoryPool::instance().configure(config);
```

### 监控内存使用

```cpp
auto stats = MemoryPool::instance().getStats();
std::cout << "当前分配: " << stats.currentBytesAllocated << " bytes\n";
std::cout << "峰值: " << stats.peakBytesAllocated << " bytes\n";
```

---

## 10. 完整示例

参考 [examples/http_server.cpp](../examples/http_server.cpp) 查看包含路由、中间件、路径参数、WebSocket 的完整示例。

**运行示例：**

```bash
cd /path/to/hical

# Windows (MSYS2)
cmake -B build -G "Ninja" -DCMAKE_PREFIX_PATH=C:/msys64/mingw64

# Linux
cmake -B build -G "Ninja"

# macOS (Homebrew)
cmake -B build -G "Ninja" -DOPENSSL_ROOT_DIR=$(brew --prefix openssl@3)

# 编译并运行
cmake --build build --target http_server
./build/examples/http_server       # Linux / macOS
./build/examples/http_server.exe   # Windows
```

---

## 下一步

- [使用示例](examples_guide.md) — 8 个从简到繁的完整示例
- [API 文档](api_reference.md) — 所有公共类和方法的详细说明
- [架构设计](architecture.md) — 框架分层架构和设计决策
- [性能报告](performance_report.md) — 基准测试数据
