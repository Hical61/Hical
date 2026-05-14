# Hical 编译与测试指南

**最后更新：** 2026-05-05
**支持平台：** Windows / Linux / macOS

---

## 1. 环境要求

| 组件        | 最低版本                         | 用途                        |
| ----------- | -------------------------------- | --------------------------- |
| C++ 编译器  | GCC 14+ / Clang 20+ / MSVC 2022+ | C++20 编译器（协程支持）    |
| CMake       | 3.20+                            | 构建系统                    |
| Ninja       | 1.10+                            | 构建工具（比 Make 更快）    |
| Boost       | 1.82+（DB 中间件 1.85+）         | Asio / JSON / MySQL         |
| OpenSSL     | 3.0+                             | SSL/TLS 支持                |
| zlib        | —                                | WebSocket permessage-deflate 压缩 |
| liburing    | —（Linux 可选）                  | Boost.Asio 异步文件 I/O     |
| Google Test | 1.10+                            | 单元测试框架                |

---

## 2. 环境安装

### 2.1 Windows（MSYS2）

**安装 MSYS2：** 从 https://www.msys2.org/ 下载并安装到 `C:\msys64`。

**安装编译工具链：** 打开 **MSYS2 MINGW64** 终端，执行：

```bash
pacman -Syu
pacman -S mingw-w64-x86_64-gcc
pacman -S mingw-w64-x86_64-cmake
pacman -S mingw-w64-x86_64-ninja
pacman -S mingw-w64-x86_64-boost
pacman -S mingw-w64-x86_64-openssl
pacman -S mingw-w64-x86_64-zlib
pacman -S mingw-w64-x86_64-gtest
```

**配置系统 PATH：** 将 `C:\msys64\mingw64\bin` 添加到 Windows 系统环境变量 PATH，然后**重新打开终端**生效。

**验证安装：**

```bash
g++ --version      # 应显示 GCC 15.x.x
cmake --version    # 应显示 cmake version 4.x.x
ninja --version    # 应显示 1.x.x
openssl version    # 应显示 OpenSSL 3.x.x
```

### 2.2 Ubuntu / Debian

```bash
sudo apt update
sudo apt install -y build-essential g++ cmake ninja-build \
                    libboost-all-dev libssl-dev libgtest-dev liburing-dev
```

> Ubuntu 24.04+ 开箱即用（GCC 14+、Boost 1.83+）。Ubuntu 22.04 的 Boost 1.74 不满足 1.82+ 要求，需手动升级 Boost 或使用 Ubuntu 24.04。

```bash
# Ubuntu 22.04 升级 GCC
sudo add-apt-repository ppa:ubuntu-toolchain-r/test
sudo apt update
sudo apt install g++-14
```

**验证安装：**

```bash
g++ --version                        # 需要 14+
cmake --version                      # 需要 3.20+
dpkg -s libboost-dev | grep Version  # 需要 1.82+（DB 中间件需 1.85+）
openssl version                      # 需要 3.0+
```

### 2.3 Fedora / RHEL / CentOS

```bash
# Fedora
sudo dnf install -y gcc-c++ cmake ninja-build \
                    boost-devel openssl-devel gtest-devel liburing-devel

# RHEL 8 / CentOS Stream 8（需启用 EPEL 和 PowerTools）
sudo dnf install -y epel-release
sudo dnf config-manager --set-enabled powertools
sudo dnf install -y gcc-c++ cmake ninja-build \
                    boost-devel openssl-devel gtest-devel liburing-devel
```

**验证安装：**

```bash
g++ --version      # 需要 14+
cmake --version    # 需要 3.20+
rpm -q boost-devel # 需要 1.82+（DB 中间件需 1.85+）
openssl version    # 需要 3.0+
```

### 2.4 Arch Linux

```bash
sudo pacman -S gcc cmake ninja boost openssl zlib gtest
```

> Arch 滚动更新，软件包版本始终满足要求。

### 2.5 macOS（Homebrew）

**安装 Homebrew（如未安装）：**

```bash
/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
```

**安装依赖：**

```bash
brew install cmake ninja boost openssl@3 googletest
```

> **注意：** macOS 系统自带的是 LibreSSL，不是 OpenSSL。Homebrew 安装的 OpenSSL 不在默认搜索路径，编译时需通过 CMake 参数指定（见第 3 节）。

**Apple Silicon（M1/M2/M3/M4）：**
- Homebrew 安装路径为 `/opt/homebrew`（Intel Mac 为 `/usr/local`）
- 使用 `brew --prefix` 会自动返回正确路径，无需手动区分

**验证安装：**

```bash
clang++ --version                   # 需要 Xcode 14+（Apple Clang 14+）
cmake --version                     # 需要 3.20+
brew list --versions boost          # 需要 1.82+（DB 中间件需 1.85+）
$(brew --prefix openssl@3)/bin/openssl version  # 需要 3.0+
```

---

## 3. 编译项目

### 3.1 CMake 配置

```bash
cd /path/to/hical

# 清理旧构建（首次或配置变更时执行）
rm -rf build
```

根据平台选择对应的配置命令：

```bash
# Windows (MSYS2)
cmake -B build -G "Ninja" -DCMAKE_PREFIX_PATH=C:/msys64/mingw64

# Linux (Ubuntu / Fedora / Arch)
cmake -B build -G "Ninja"

# Linux (Ubuntu 22.04 — 需指定升级后的编译器)
cmake -B build -G "Ninja" -DCMAKE_CXX_COMPILER=g++-14

# macOS (Homebrew)
cmake -B build -G "Ninja" \
    -DOPENSSL_ROOT_DIR=$(brew --prefix openssl@3) \
    -DCMAKE_PREFIX_PATH="$(brew --prefix boost);$(brew --prefix openssl@3)"
```

**可选编译开关：**

| 选项                      | 默认值 | 说明                                                       |
| ------------------------- | ------ | ---------------------------------------------------------- |
| `HICAL_WITH_DATABASE`     | `OFF`  | 启用数据库中间件（需 Boost.MySQL，即 Boost >= 1.85）       |
| `HICAL_WITH_OPENAPI`      | `ON`   | 启用 OpenAPI 文档自动生成模块（复用 Boost.JSON，无新依赖） |
| `HICAL_ENABLE_REFLECTION` | `OFF`  | 启用 C++26 原生反射（需兼容编译器）                        |
| `HICAL_BUILD_TESTS`       | `ON`   | 是否编译测试套件                                           |
| `HICAL_BUILD_EXAMPLES`    | `ON`   | 是否编译示例程序                                           |

```bash
# 示例：启用数据库 + 关闭 OpenAPI（体积敏感场景）
cmake -B build -G "Ninja" -DHICAL_WITH_DATABASE=ON -DHICAL_WITH_OPENAPI=OFF
```

**预期输出：**

```
-- Found Boost: ...BoostConfig.cmake (found suitable version "x.xx.x", ...)
-- Found OpenSSL: ... (found version "3.x.x")
-- Found GTest: ...GTestConfig.cmake (found version "x.xx.x")
-- Configuring done
-- Generating done
-- Build files have been written to: /path/to/hical/build
```

### 3.2 编译

```bash
cmake --build build
```

**预期输出：** 全部编译成功，0 错误 0 警告。

**构建产物说明：**

| 产物         | 说明                                                                                                               |
| ------------ | ------------------------------------------------------------------------------------------------------------------ |
| `hical_core` | 核心静态库；启用 `HICAL_WITH_DATABASE=ON` 时包含 DB 中间件代码；`HICAL_WITH_OPENAPI=ON`（默认）时包含 OpenAPI 代码 |
| `test_*`     | 各组件单元测试                                                                                                     |
| `examples/*` | 示例可执行文件（echo_server / http_server / benchmark 等）                                                         |

### 3.3 清理后重新编译

```bash
cmake --build build --clean-first
```

### 3.4 各平台注意事项

| 平台                  | 注意事项                                                                               |
| --------------------- | -------------------------------------------------------------------------------------- |
| Windows (MSYS2)       | 必须指定 `CMAKE_PREFIX_PATH=C:/msys64/mingw64`，否则找不到 Boost/OpenSSL               |
| Windows (MSVC)        | hical CMakeLists.txt 已自动添加 `/utf-8` 和 `_WIN32_WINNT=0x0A00`                      |
| Linux                 | 系统包管理器安装的库在标准路径，无需额外指定                                           |
| macOS                 | **必须**指定 `OPENSSL_ROOT_DIR`，否则 CMake 会找到系统自带的 LibreSSL 而非 OpenSSL 3.x |
| macOS (Apple Silicon) | `brew --prefix` 自动返回 `/opt/homebrew` 前缀，无需手动区分架构                        |

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

# C++20 Concepts 编译期约束测试
./build/tests/test_concepts.exe

# MetaJson + MetaRoutes 反射层测试
./build/tests/test_reflection.exe

# Cookie 解析与 Set-Cookie 测试
./build/tests/test_cookie.exe

# 静态文件服务测试
./build/tests/test_static_files.exe

# Multipart 解析测试
./build/tests/test_multipart.exe

# Session 生命周期 / 线程安全 / regenerate 测试
./build/tests/test_session.exe

# OpenAPI 自动生成测试（Schema/Registry/Document/Endpoint）
./build/tests/test_openapi.exe

# 查询参数解析测试
./build/tests/test_query_params.exe

# 表单参数解析测试
./build/tests/test_form_params.exe

# 重定向测试
./build/tests/test_redirect.exe

# CORS 中间件测试
./build/tests/test_cors.exe

# 路由组测试
./build/tests/test_route_group.exe

# 全局错误处理器测试
./build/tests/test_error_handler.exe

# 优雅关闭测试
./build/tests/test_graceful_shutdown.exe

# 日志核心测试（36 个用例）
./build/tests/test_log.exe

# NDEBUG 编译消除验证（3 个用例）
./build/tests/test_log_ndebug.exe

# FixedBuffer 栈缓冲测试（19 个用例）
./build/tests/test_fixed_buffer.exe

# LogFile 文件轮转测试（7 个用例）
./build/tests/test_log_file.exe

# AsyncFileSink 异步日志测试（7 个用例）
./build/tests/test_async_file_sink.exe

# TextFormatter + JsonFormatter 测试（12 个用例）
./build/tests/test_log_formatter.exe

# LogChannel + Registry 测试（12 个用例）
./build/tests/test_log_channel.exe

# LogMiddleware 测试（3 个用例）
./build/tests/test_log_middleware.exe

# LogAdmin 端点测试（4 个用例）
./build/tests/test_log_admin.exe

# 完整 HTTP 请求/响应周期集成测试
./build/tests/test_integration.exe
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

# Linux / macOS
openssl req -x509 -newkey rsa:2048 \
    -keyout test_server.key -out test_server.crt \
    -days 365 -nodes -subj "/CN=localhost"

# Windows (MSYS2) — 需要 MSYS_NO_PATHCONV=1 防止路径转换
MSYS_NO_PATHCONV=1 openssl req -x509 -newkey rsa:2048 \
    -keyout test_server.key -out test_server.crt \
    -days 365 -nodes -subj "/CN=localhost"
```

### 4.6 测试用例清单（38 + 5 个可选 DB 测试套件）

| 测试文件                                                | 用例数 | 覆盖范围                                                       |
| ------------------------------------------------------- | ------ | -------------------------------------------------------------- |
| test_basic                                              | 2      | Boost 版本、C++ 标准                                           |
| test_error                                              | 17     | 错误码映射、NetworkError 结构体                                |
| test_asio_event_loop                                    | 10     | run/stop/post/dispatch/定时器/pmr                              |
| test_asio_tcp_connection                                | 8      | 连接/收发/回调/上下文/PmrBuffer                                |
| test_memory_pool                                        | 21     | 单例/全局池/线程本地池/请求池/TrackedResource/PmrBuffer        |
| test_asio_timer                                         | 8      | 单次/周期/取消/精度/EventLoop 集成                             |
| test_ssl_connection                                     | 9      | SslContext/SSL 握手/加密通信/类型别名                          |
| test_coroutine                                          | 5      | sleep/sleepFor/coSpawn/多协程/返回值                           |
| test_http_types                                         | 23     | HttpMethod/HttpStatusCode/HttpRequest/HttpResponse/工厂方法    |
| test_router                                             | 14     | 路由注册/分发/404/协程/JSON/宏/路径参数/{param}                |
| test_router_perf                                        | 5      | 静态路由首条/末条/未命中/参数路由/1000 路由性能                |
| test_tcp_server                                         | 8      | EventLoopPool/TcpServer(accept/消息/IO 线程池)                 |
| test_middleware                                         | 5      | 空管道/单层/洋葱顺序/拦截/响应修改                             |
| test_http_server                                        | 7      | GET/POST/404/路径参数/中间件/JSON                              |
| test_websocket                                          | 3      | Echo/连接回调/未注册路径                                       |
| test_concepts                                           | —      | C++20 Concepts 编译期约束验证                                  |
| test_reflection                                         | 35     | MetaJson 装饰器/alias/required/ignore/unsigned/backward compat |
| test_cookie                                             | —      | Cookie 解析/Set-Cookie 头/CookieOptions                        |
| test_static_files                                       | —      | 静态文件服务/ETag 304/路径遍历防护/大文件拒绝                  |
| test_multipart                                          | —      | multipart/form-data 解析/256 part 上限                         |
| test_session                                            | —      | Session 生命周期/线程安全/regenerate/migrateFrom/并发          |
| test_openapi                                            | 35     | OpenAPI Schema 生成/registry CRUD/文档组装/端点注册/完整集成   |
| test_query_params                                       | —      | 查询参数解析（queryParam/queryParams/hasQueryParam）           |
| test_form_params                                        | —      | 表单参数解析（formParam/formParams/hasFormParam）              |
| test_redirect                                           | —      | 重定向响应（redirect 工厂方法/Location 头/CRLF 防护）          |
| test_cors                                               | —      | CORS 中间件（通配符/精确匹配/Preflight/凭证模式）              |
| test_route_group                                        | —      | 路由组（前缀拼接/组级中间件/嵌套子组）                         |
| test_error_handler                                      | —      | 全局错误处理器（异常捕获/自定义响应）                          |
| test_graceful_shutdown                                  | —      | 优雅关闭（stop()/进行中请求完成）                              |
| test_log                                                | 36     | 日志核心（format API、Sink API、多线程安全、Fatal abort）      |
| test_log_ndebug                                         | 3      | NDEBUG 编译消除（TRACE/TRACE_IF/TRACE_STREAM 消除）            |
| test_fixed_buffer                                       | 19     | FixedBuffer 栈缓冲（append、溢出 fallback、格式化）            |
| test_log_file                                           | 7      | LogFile 文件轮转（写入、大小轮转、文件数限制）                 |
| test_async_file_sink                                    | 7      | AsyncFileSink 异步日志（多线程、优雅关闭、轮转）               |
| test_log_formatter                                      | 12     | TextFormatter + JsonFormatter（格式、traceId、JSON）           |
| test_log_channel                                        | 12     | LogChannel + Registry（通道路由、HICAL_LOG_TO 宏）             |
| test_log_middleware                                     | 3      | LogMiddleware（trace-id 生成长度/hex/唯一性）                  |
| test_log_admin                                          | 4      | LogAdmin 端点（注册、级别往返、通道级别调整）                  |
| test_integration                                        | —      | 完整 HTTP 请求/响应周期集成测试                                |
| **以下为可选 DB 测试（需 `-DHICAL_WITH_DATABASE=ON`）** |        |                                                                |
| test_db_pool                                            | 12     | 连接池获取/释放、健康检查、空闲淘汰、统计                      |
| test_db_middleware                                      | 8      | DB 中间件连接注入、自动事务、洋葱模型集成                      |
| test_db_query_log                                       | 6      | 查询日志记录、慢查询检测、回调                                 |
| test_stmt_cache                                         | 9      | PreparedStatement LRU 缓存淘汰/提升/禁用                       |
| test_mysql_integration                                  | 7      | 真实 MySQL CRUD、事务、参数化查询（需数据库）                  |

---

## 5. 运行示例程序

### 5.1 pmr 内存池 PoC

> **注意：** Debug 模式下 pmr 虚函数无法内联，性能数据不准确。建议用 Release 模式运行性能对比。

**Debug 模式运行：**

```bash
./build/examples/pmr_poc        # Linux / macOS
./build/examples/pmr_poc.exe    # Windows
```

**Release 模式运行（推荐）：**

```bash
# Windows (MSYS2)
cmake -B build-release -G "Ninja" -DCMAKE_PREFIX_PATH=C:/msys64/mingw64 -DCMAKE_BUILD_TYPE=Release

# Linux
cmake -B build-release -G "Ninja" -DCMAKE_BUILD_TYPE=Release

# macOS (Homebrew)
cmake -B build-release -G "Ninja" -DCMAKE_BUILD_TYPE=Release \
    -DOPENSSL_ROOT_DIR=$(brew --prefix openssl@3) \
    -DCMAKE_PREFIX_PATH="$(brew --prefix boost);$(brew --prefix openssl@3)"

# 编译并运行
cmake --build build-release --target pmr_poc
./build-release/examples/pmr_poc        # Linux / macOS
./build-release/examples/pmr_poc.exe    # Windows
```

**Windows PowerShell 中文乱码修复：**

```powershell
chcp 65001
```

### 5.2 协程式 Echo Server

**终端 1 — 启动服务器：**

```bash
./build/examples/echo_server 8888        # Linux / macOS
./build/examples/echo_server.exe 8888    # Windows
```

**终端 2 — 压力测试：**

```bash
# 10 个并发连接，每连接 100 个请求
./build/examples/benchmark localhost 8888 10 100        # Linux / macOS
./build/examples/benchmark.exe localhost 8888 10 100    # Windows
```

### 5.3 HTTP Server

**终端 1 — 启动服务器：**

```bash
./build/examples/http_server 8080        # Linux / macOS
./build/examples/http_server.exe 8080    # Windows
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
./build-release/examples/http_benchmark localhost 8080 50 1000 /api/status GET         # Linux / macOS
./build-release/examples/http_benchmark.exe localhost 8080 50 1000 /api/status GET     # Windows

# POST 请求基准测试
./build-release/examples/http_benchmark localhost 8080 50 1000 /api/echo POST '{"hello":"world"}'       # Linux / macOS
./build-release/examples/http_benchmark.exe localhost 8080 50 1000 /api/echo POST '{"hello":"world"}'   # Windows
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
./build-release/examples/pmr_benchmark        # Linux / macOS
./build-release/examples/pmr_benchmark.exe    # Windows
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
{"framework":"hical","status":"running","version":"1.0.0"}

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
│   │   ├── TcpConnection.h     # TCP 连接接口（含 sendFile/lastActiveTime）
│   │   ├── Timer.h / Concepts.h # 定时器接口 / C++ Concepts
│   │   ├── PmrBuffer.h         # pmr 统一缓冲区（指数增长 + 自适应缩容）
│   │   ├── MemoryPool.h/.cpp   # pmr 内存池管理器
│   │   ├── Error.h/.cpp        # 错误码体系
│   │   ├── InetAddress.h/.cpp  # 网络地址封装
│   │   ├── SslContext.h/.cpp   # SSL/TLS 上下文配置
│   │   ├── Coroutine.h         # 协程工具（Awaitable/sleep/coSpawn）
│   │   ├── HttpTypes.h         # HTTP 方法/状态码枚举
│   │   ├── HttpRequest.h/.cpp  # HttpRequest 封装（string_view 返回 + jsonBody 缓存）
│   │   ├── HttpResponse.h/.cpp # HttpResponse 封装 + 工厂方法（CR/LF 防护）
│   │   ├── Cookie.h            # CookieOptions 结构体
│   │   ├── Router.h/.cpp       # 路由器（{param}/WS + WsOptions Origin 白名单）
│   │   ├── Middleware.h/.cpp   # 中间件系统（洋葱模型 + buildFor）
│   │   ├── HttpServer.h/.cpp   # HTTP 服务器（高层封装 + WS 中间件预构建 + fd 防护）
│   │   ├── WebSocket.h/.cpp    # WebSocket 会话
│   │   ├── StaticFiles.h       # 静态文件服务（异步 I/O + PathCache + ETag）
│   │   ├── Multipart.h/.cpp    # multipart/form-data 解析（dual API）
│   │   ├── Session.h/.cpp      # Session 会话管理（shared_mutex + regenerate）
│   │   ├── Cors.h              # CORS 中间件（makeCorsMiddleware + CorsOptions）
│   │   ├── RouteGroup.h/.cpp   # 路由组（前缀分组 + 组级中间件）
│   │   ├── OpenApiSchema.h     # JSON Schema 生成（jsonSchema<T>/collectSchemas）
│   │   ├── OpenApiRegistry.h/.cpp  # 路由元数据注册表（RouteApiInfo/HICAL_API）
│   │   ├── OpenApiDocument.h/.cpp  # OpenAPI 3.0 文档组装（惰性缓存/路径合并）
│   │   ├── OpenApiEndpoint.h   # 端点暴露（serveOpenApi，/openapi.json + /docs）
│   │   ├── IdleFd.h            # 空闲 fd 预留（EMFILE 防护）
│   │   ├── WriteNode.h         # 多态写队列节点（内存/文件）
│   │   ├── Reflection.h / MetaJson.h / MetaRoutes.h  # C++26 反射层
│   │   └── Version.h.in        # CMake 配置版本头
│   ├── asio/
│   │   ├── AsioEventLoop.h/.cpp      # Asio 事件循环
│   │   ├── GenericConnection.h/.cpp  # TCP/SSL 统一连接（WriteNode 写队列 + sendFile）
│   │   ├── SslConnection.h           # SSL 连接类型别名
│   │   ├── AsioTimer.h/.cpp          # Asio 定时器
│   │   ├── EventLoopPool.h/.cpp      # 多线程事件循环池
│   │   └── TcpServer.h/.cpp          # TCP 服务器（空闲超时 + IdleFd 防护）
│   └── db/                    # 数据库中间件（可选，HICAL_WITH_DATABASE=ON）
│       ├── DbConfig.h         # 数据库连接配置
│       ├── DbResult.h         # 查询结果封装
│       ├── DbConnection.h     # 数据库连接抽象接口
│       ├── DbConnectionPool.h/.cpp # 协程化连接池
│       ├── DbMiddleware.h     # HTTP 数据库中间件
│       ├── DbQueryLog.h/.cpp  # 查询日志中间件（装饰器模式）
│       ├── MysqlConnection.h/.cpp  # MySQL 后端（Boost.MySQL）
│       └── StmtCache.h/.cpp   # PreparedStatement LRU 缓存
├── tests/                     # 38 个测试套件 + 5 个可选 DB 测试
│   ├── test_db_pool.cpp               # DB 连接池测试
│   ├── test_db_middleware.cpp         # DB 中间件测试
│   ├── test_db_query_log.cpp          # 查询日志测试
│   ├── test_stmt_cache.cpp            # PreparedStatement 缓存测试
│   ├── test_mysql_integration.cpp     # 真实 MySQL 集成测试（需数据库）
│   ├── test_query_params.cpp          # 查询参数解析测试
│   ├── test_form_params.cpp           # 表单参数解析测试
│   ├── test_redirect.cpp              # 重定向测试
│   ├── test_cors.cpp                  # CORS 中间件测试
│   ├── test_route_group.cpp           # 路由组测试
│   ├── test_error_handler.cpp         # 全局错误处理器测试
│   └── test_graceful_shutdown.cpp     # 优雅关闭测试
├── examples/
│   ├── echo_server.cpp        # Echo Server（PoC）
│   ├── pmr_poc.cpp            # pmr PoC
│   ├── benchmark.cpp          # Echo Server 压力测试
│   ├── http_server.cpp        # HTTP Server（路由+中间件+WS）
│   ├── http_benchmark.cpp     # HTTP 基准测试（QPS/延迟）
│   ├── pmr_benchmark.cpp      # pmr 内存池基准测试
│   └── openapi_server.cpp     # OpenAPI 文档自动生成示例
└── docs/
    ├── project_structure.md   # 项目代码结构说明
    ├── build_and_test_guide.md # 本文档
    ├── api_reference.md        # 完整公共 API 说明
    ├── quickstart.md           # 快速上手指南
    ├── examples_guide.md       # 使用示例（8 个完整示例）
    ├── architecture.md         # 架构设计文档（PMR/协程/Concepts/反射）
    └── performance_report.md   # 性能测试报告与调优指南
```
