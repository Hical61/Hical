# HTTP/2 CI 入口与固定依赖

这些文件只提供待执行的 CI 入口，本机未触发远端 workflow，也未运行 Linux Sanitizer。
没有 Docker/WSL 依赖；本机 Windows 原生互操作由 `test_http2_interop.py` 驱动。

| 入口 | 真实配置与检查 |
| --- | --- |
| `ci.yml` 原有 build/profiling | 保留 GCC/Clang/CMake最低版本/MSYS2/MSVC OFF 矩阵，HTTP2 与 HTTP_CLIENT 显式 OFF；不准备 H2 依赖 |
| `ci.yml` 的 `http2` | Linux GCC、Linux Clang ASan/UBSan、Windows MSYS2、Windows MSVC，双端显式 ON；完整回归排除既有性能测试 |
| `ci.yml` 的 `http2-interop` | 手动 dispatch 勾选 `run_http2_interop`；Linux 原生双向 CLI、h2spec 逐项留证 |
| `sanitizer.yml` 的三个手动 job | ASan/UBSan、TSan、Valgrind Memcheck，双端 ON；定向生命周期/客户端/池/排空/文件/取消及生产 AsyncHttpClient 场景 |

ON job 均先执行 `ctest -N` 和 JSON discovery，检查真实的生产客户端及 H2 suite 存在，再以 `--no-tests=error` 运行。
证书显式生成到该 build 的 `tests/`，并设置 `HICAL_REQUIRE_SSL_CERTS=1`，缺证书不能 SKIP。
Linux 始终设置 `HICAL_DISABLE_IO_URING=ON`，实际执行 `Http2FileTlsTest`，覆盖文件 fallback。
每条外层 CTest、依赖构建命令和 job 都有有限时限；artifact 保存命令日志、CTest/JUnit/MemoryChecker 原始结果及依赖身份。
Windows H2 job 不使用原 OFF MSYS2 的失败重试逻辑。

`ci_http2_deps.py` 默认仅写 `plan_only` 清单；只有 `--build` 会下载或构建。
所有新库装入指定 CI 工作目录的 `prefix/`，不装入系统前缀，也不借用 Ubuntu apt Boost 1.83。
两个 workflow 均先运行不下载依赖的解包回归，再允许 H2 构建工作开始；Linux 用例还会实际执行解出的内部构建脚本。
普通成员保留归档权限中的低九位，不恢复 setuid/setgid 等特殊位。源码缓存同时校验归档 SHA256 和解包规则版本；
旧规则产生的缓存不能复用，遇到明确提示时为 `--root` 指定新的工作目录，脚本不会删除或覆盖旧树。
Boost 使用官方完整发行源，以 B2 只构建 JSON 静态库和安装头/配置；用户配置和 site-config 与机器现状隔离。
nghttp2 使用真实的 `BUILD_STATIC_LIBS=ON` / `BUILD_SHARED_LIBS=OFF`，普通 ON/Sanitizer 构建只启用 C 库。
MSVC 的 OpenSSL/zlib/GTest 使用工作区 vcpkg install root，并显式关闭根 manifest 的自动安装；Boost 始终来自固定独立前缀。
Linux/MSYS2 的其他基础库沿用该 job 的系统包，不会影响运行于其他 runner 的 OFF job。

固定来源：

- Boost 1.90.0：`https://archives.boost.io/release/1.90.0/source/boost_1_90_0.tar.bz2`，SHA256 `49551aff3b22cbc5c5a9ed3dbc92f0e23ea50a0f7325b0d198b705e8ee3fc305`；来自[官方 JSON](https://archives.boost.io/release/1.90.0/source/boost_1_90_0.tar.bz2.json)。
- nghttp2 1.70.0：`https://github.com/nghttp2/nghttp2/releases/download/v1.70.0/nghttp2-1.70.0.tar.xz`，SHA256 `e05cb1388eaca3830aded4ccf20044b6e1ac1a61411dcca11b0437c4285c8bc2`；已在本地复核官方归档。
- h2spec 2.6.0：独立 checkout [官方提交](https://github.com/summerwind/h2spec/commit/70ac2294010887f48b18e2d64f5cccd48421fad1)，使用固定 Go 1.24.6，从固定 `go.mod/go.sum` 构建并执行 `go mod verify`。

较慢的互操作 job 另从已验证的 nghttp2 1.70.0 源构建独立 nghttp/nghttpd/h2load CLI，而实现库也是 1.70.0；
二者分别作为库依赖和外部进程留证。本机已准备的 Cygwin 1.69.0 工具是另一组外部工具，不冒充实现依赖。
h2spec 测试基于 RFC7540/7541，不能直接声明 RFC9113 合规。默认保留严格失败；互操作 job 显式使用经过独立审查的七项精确差异文件，同时保留原始 failed 状态、退出码和规范理由。缺失、重复、skip、整轮超时、新失败或失败载荷变化均不接受，不作通配豁免。头预算与超大帧两项使用精确输入复验，原始执行147条，对应146个有效案例。

本机无下载/构建的静态计划示例：

```powershell
python scripts/ci_http2_deps.py --root build-http2-deps/ci-plan --compiler clang --cc clang-20 --cxx clang++-20 --configuration Debug --sanitizer address,undefined
```

静态语法和结构检查只能检查配方，实际构建、互操作及 Sanitizer 仍需在对应平台运行。
