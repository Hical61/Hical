# Hical 编译与测试指南

**最后更新：** 2026-04-06
**验证环境：** Windows 10 Pro 10.0.19045

---

## 1. 环境要求

| 组件            | 最低版本 | 验证版本 | 用途                     |
| --------------- | -------- | -------- | ------------------------ |
| GCC (MinGW-w64) | 10+      | 15.2.0   | C++20 编译器（协程支持） |
| CMake           | 3.20+    | 3.28.3   | 构建系统                 |
| Ninja           | 1.10+    | 1.13.2   | 构建工具（比 Make 更快） |
| Boost           | 1.70+    | 1.90.0   | Asio / Beast / JSON      |
| OpenSSL         | 3.0+     | 3.6.1    | SSL/TLS 支持             |
| Google Test     | 1.10+    | 1.17.0   | 单元测试框架             |

---

## 2. 环境安装（MSYS2）

### 2.1 安装 MSYS2

从 https://www.msys2.org/ 下载并安装到 `C:\msys64`。

### 2.2 安装编译工具链

打开 **MSYS2 MINGW64** 终端，执行：

```bash
pacman -Syu
pacman -S mingw-w64-x86_64-gcc
pacman -S mingw-w64-x86_64-cmake
pacman -S mingw-w64-x86_64-ninja
pacman -S mingw-w64-x86_64-boost
pacman -S mingw-w64-x86_64-openssl
pacman -S mingw-w64-x86_64-gtest
```

### 2.3 配置系统 PATH

将以下路径添加到 Windows 系统环境变量 PATH 中：

```
C:\msys64\mingw64\bin
```

添加后**关闭并重新打开终端**即可生效，无需重启电脑。

### 2.4 验证安装

```bash
g++ --version      # 应显示 GCC 15.x.x
cmake --version    # 应显示 cmake version 4.x.x
ninja --version    # 应显示 1.x.x
openssl version    # 应显示 OpenSSL 3.x.x
```

---

## 3. 编译项目

### 3.1 CMake 配置

```bash
cd e:/MyGit/hical

# 清理旧构建（首次或配置变更时执行）
rm -rf build

# 配置构建，指定 MSYS2 路径
cmake -B build -G "Ninja" -DCMAKE_PREFIX_PATH=C:/msys64/mingw64
```

**预期输出：**

```
-- Found Boost: .../Boost-1.90.0/BoostConfig.cmake (found suitable version "1.90.0", ...)
-- Found OpenSSL: .../libcrypto.dll.a (found version "3.6.1")
-- Found GTest: .../GTestConfig.cmake (found version "1.17.0")
-- Configuring done
-- Generating done
-- Build files have been written to: E:/MyGit/hical/build
```

### 3.2 编译

```bash
cmake --build build
```

**预期输出：** 全部编译成功，0 错误 0 警告。

### 3.3 清理后重新编译

```bash
cmake --build build --clean-first
```

---

## 4. 运行测试

### 4.1 运行全部测试

```bash
ctest --test-dir build --output-on-failure -j4
```

### 4.2 运行单个测试模块

```bash
# 基础环境测试
./build/tests/test_basic.exe

# 错误码测试
./build/tests/test_error.exe

# EventLoop 测试
./build/tests/test_asio_event_loop.exe

# TcpConnection 测试（含 GenericConnection 兼容性）
./build/tests/test_asio_tcp_connection.exe

# MemoryPool + PmrBuffer 测试
./build/tests/test_memory_pool.exe

# Timer 测试
./build/tests/test_asio_timer.exe

# SSL 连接测试（需要先生成测试证书，见 4.4）
./build/tests/test_ssl_connection.exe

# 协程工具测试
./build/tests/test_coroutine.exe

# HTTP 类型测试（HttpRequest / HttpResponse）
./build/tests/test_http_types.exe

# 路由器测试（含路径参数）
./build/tests/test_router.exe

# TcpServer + EventLoopPool 测试
./build/tests/test_tcp_server.exe

# 中间件测试
./build/tests/test_middleware.exe

# HttpServer 集成测试
./build/tests/test_http_server.exe

# WebSocket 测试
./build/tests/test_websocket.exe
```

### 4.3 运行路由性能基准测试

```bash
./build-release/tests/test_router_perf.exe
```

**预期输出示例：**

```
[静态路由-首条命中] 100000 次分发, 总耗时: 263 ms, 每次: 2635 ns
[静态路由-末条命中] 100000 次分发, 总耗时: 375 ms, 每次: 3757 ns
[1000路由-中间命中] 100000 次分发, 总耗时: 281 ms, 每次: 2813 ns
```

> 哈希表优化后，100 路由和 1000 路由查找性能几乎一致。

### 4.4 运行指定测试用例

```bash
# 运行名称匹配 "Post" 的测试
./build/tests/test_asio_event_loop.exe --gtest_filter="*Post*"

# 运行所有 PmrBuffer 测试
./build/tests/test_memory_pool.exe --gtest_filter="PmrBufferTest.*"

# 运行 SSL 握手测试
./build/tests/test_ssl_connection.exe --gtest_filter="*NativeSsl*"
```

### 4.5 生成 SSL 测试证书

SSL 握手测试需要自签名证书。在 `build/` 目录下执行：

```bash
cd build
MSYS_NO_PATHCONV=1 openssl req -x509 -newkey rsa:2048 \
    -keyout test_server.key -out test_server.crt \
    -days 365 -nodes -subj "/CN=localhost"
```

> Windows MSYS2 环境需要 `MSYS_NO_PATHCONV=1` 前缀防止路径转换。

### 4.6 测试用例清单（145 个）

| 测试文件                 | 用例数 | 覆盖范围                                                    |
| ------------------------ | ------ | ----------------------------------------------------------- |
| test_basic               | 2      | Boost 版本、C++ 标准                                        |
| test_error               | 17     | 错误码映射、NetworkError 结构体                             |
| test_asio_event_loop     | 10     | run/stop/post/dispatch/定时器/pmr                           |
| test_asio_tcp_connection | 8      | 连接/收发/回调/上下文/PmrBuffer                             |
| test_memory_pool         | 21     | 单例/全局池/线程本地池/请求池/TrackedResource/PmrBuffer     |
| test_asio_timer          | 8      | 单次/周期/取消/精度/EventLoop 集成                          |
| test_ssl_connection      | 9      | SslContext/SSL 握手/加密通信/类型别名                       |
| test_coroutine           | 5      | sleep/sleepFor/coSpawn/多协程/返回值                        |
| test_http_types          | 23     | HttpMethod/HttpStatusCode/HttpRequest/HttpResponse/工厂方法 |
| test_router              | 14     | 路由注册/分发/404/协程/JSON/宏/路径参数/{param}             |
| test_router_perf         | 5      | 静态路由首条/末条/未命中/参数路由/1000 路由性能             |
| test_tcp_server          | 8      | EventLoopPool/TcpServer(accept/消息/IO 线程池)              |
| test_middleware          | 5      | 空管道/单层/洋葱顺序/拦截/响应修改                          |
| test_http_server         | 7      | GET/POST/404/路径参数/中间件/JSON                           |
| test_websocket           | 3      | Echo/连接回调/未注册路径                                    |

---

## 5. 运行示例程序

### 5.1 pmr 内存池 PoC

> **注意：** Debug 模式下 pmr 虚函数无法内联，性能数据不准确。建议用 Release 模式运行性能对比。

**Debug 模式运行：**

```bash
./build/examples/pmr_poc.exe
```

**Release 模式运行（推荐）：**

```bash
cmake -B build-release -G "Ninja" -DCMAKE_PREFIX_PATH=C:/msys64/mingw64 -DCMAKE_BUILD_TYPE=Release
cmake --build build-release --target pmr_poc
./build-release/examples/pmr_poc.exe
```

**PowerShell 中文乱码修复：**

```powershell
chcp 65001
```

### 5.2 协程式 Echo Server

**终端 1 — 启动服务器：**

```bash
./build/examples/echo_server.exe 8888
```

**终端 2 — 压力测试：**

```bash
# 10 个并发连接，每连接 100 个请求
./build/examples/benchmark.exe localhost 8888 10 100
```

### 5.3 HTTP Server

**终端 1 — 启动服务器：**

```bash
./build/examples/http_server.exe 8080
```

**终端 2 — 测试请求：**

```bash
# 首页
curl http://localhost:8080/

# 状态查询（JSON 响应）
curl http://localhost:8080/api/status

# Echo 回写
curl -X POST -d "Hello hical" http://localhost:8080/api/echo

# 带查询参数
curl http://localhost:8080/api/hello?name=world

# 路径参数
curl http://localhost:8080/users/42

# 未注册路由（返回 404）
curl http://localhost:8080/nonexistent
```

### 5.4 HTTP 基准测试

> 需要先启动 http_server（见 5.3），然后在另一个终端运行基准测试。

```bash
# 50 个并发连接，每连接 1000 个 GET 请求
./build-release/examples/http_benchmark.exe localhost 8080 50 1000 /api/status GET

# POST 请求基准测试
./build-release/examples/http_benchmark.exe localhost 8080 50 1000 /api/echo POST '{"hello":"world"}'
```

**预期输出：**

```
========== hical HTTP 基准测试 ==========
目标: localhost:8080/api/status
方法: GET
并发连接: 50
每连接请求: 1000
总请求数: 50000
=========================================
运行中...

========== 测试结果 ==========
  总请求数:    50000
  成功请求:    50000
  失败请求:    0
  QPS:         xxxxx req/s

  延迟分布:
    平均:  x.xx ms
    P50:   x.xx ms
    P99:   x.xx ms
==============================
```

### 5.5 pmr 内存池基准测试

```bash
./build-release/examples/pmr_benchmark.exe
```

**输出内容：**
- 单线程分配/释放性能对比（new/delete vs sync_pool vs unsync_pool vs monotonic vs hical threadLocal）
- 多线程并发分配性能 + MemoryPool 统计数据（全局池分配次数、峰值字节数等）
- PmrBuffer append 性能对比（std::string vs PmrBuffer 默认 vs PmrBuffer pool）

### 5.6 WebSocket 测试

**WebSocket 测试（需要 wscat 或类似工具）：**

```bash
# 安装 wscat: npm install -g wscat
wscat -c ws://localhost:8080/ws/echo
# 输入消息后会收到 "Echo: <你的消息>"
```

**预期输出：**

```
# GET /
Welcome to hical!

# GET /api/status
{"framework":"hical","status":"running","version":"0.2.0"}

# POST /api/echo
Hello hical

# GET /api/hello?name=world
Hello! query=name=world

# GET /users/42
{"name":"User 42","userId":"42"}

# GET /nonexistent
Not Found
```

---

## 6. 项目结构速查

```
hical/
├── CMakeLists.txt              # 根构建配置（Boost + OpenSSL + GTest）
├── src/
│   ├── CMakeLists.txt          # 核心库构建配置
│   ├── core/
│   │   ├── EventLoop.h         # 事件循环接口
│   │   ├── TcpConnection.h     # TCP 连接接口
│   │   ├── Timer.h / Concepts.h # 定时器接口 / C++ Concepts
│   │   ├── PmrBuffer.h         # pmr 统一缓冲区
│   │   ├── MemoryPool.h/.cpp   # pmr 内存池管理器
│   │   ├── Error.h/.cpp        # 错误码体系
│   │   ├── InetAddress.h/.cpp  # 网络地址封装
│   │   ├── SslContext.h/.cpp   # SSL/TLS 上下文配置
│   │   ├── Coroutine.h         # 协程工具（Awaitable/sleep/coSpawn）
│   │   ├── HttpTypes.h         # HTTP 方法/状态码枚举
│   │   ├── HttpRequest.h/.cpp  # HttpRequest 封装（含路径参数）
│   │   ├── HttpResponse.h/.cpp # HttpResponse 封装 + 工厂方法
│   │   ├── Router.h/.cpp       # 路由器（{param}/WS/HICAL_ROUTE）
│   │   ├── Middleware.h/.cpp   # 中间件系统（洋葱模型）
│   │   ├── HttpServer.h/.cpp   # HTTP 服务器（高层封装）
│   │   └── WebSocket.h/.cpp    # WebSocket 会话
│   └── asio/
│       ├── AsioEventLoop.h/.cpp      # Asio 事件循环
│       ├── GenericConnection.h/.cpp  # TCP/SSL 统一连接
│       ├── AsioTimer.h/.cpp          # Asio 定时器
│       ├── EventLoopPool.h/.cpp      # 多线程事件循环池
│       └── TcpServer.h/.cpp          # TCP 服务器
├── tests/                     # 15 个测试套件，145 个用例
├── examples/
│   ├── echo_server.cpp        # Echo Server（PoC）
│   ├── pmr_poc.cpp            # pmr PoC
│   ├── benchmark.cpp          # Echo Server 压力测试
│   ├── http_server.cpp        # HTTP Server（路由+中间件+WS）
│   ├── http_benchmark.cpp     # HTTP 基准测试（QPS/延迟）
│   └── pmr_benchmark.cpp      # pmr 内存池基准测试
└── docs/
    ├── project_structure.md   # 项目代码结构说明
    ├── build_and_test_guide.md # 本文档
    ├── api_reference.md        # 完整公共 API 说明
    ├── quickstart.md           # 快速上手指南
    ├── examples_guide.md       # 使用示例（8 个完整示例）
    ├── architecture.md         # 架构设计文档（PMR/协程/Concepts/反射）
    └── performance_report.md   # 性能测试报告与调优指南
```
