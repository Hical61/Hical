# HTTP/2 使用指南

> 本文对应当前候选实现。Windows 本机功能、包消费、互操作、安装示例和性能矩阵已完成下述验证；Linux Sanitizer 和 CI 尚未运行，仍待最终验收。示例和配置以当前源码为准，不把单个平台的结果当作跨平台证明。

Hical 的 HTTP/2 通过 TLS ALPN 协商，继续使用现有路由、`HttpRequest`、`HttpResponse` 和中间件。默认构建和默认客户端仍使用 HTTP/1.1，应用需要显式开启 HTTP/2。

## 构建与安装

`HICAL_WITH_HTTP2` 和 `HICAL_WITH_HTTP_CLIENT` 都默认关闭，两者可独立组合：

| HTTP2 | HTTP_CLIENT | 编译内容 |
| --- | --- | --- |
| OFF | OFF | HTTP/1 服务端，不查找 nghttp2 |
| ON | OFF | HTTP/1 与 TLS HTTP/2 服务端 |
| OFF | ON | HTTP/1 服务端与协程 HTTP/1 客户端 |
| ON | ON | 服务端与客户端均支持 TLS HTTP/2 |

需要 CMake 3.28+、C++20 编译器、OpenSSL、zlib 和 Boost。服务端 Boost 最低 1.82；启用生产 HTTP 客户端时最低 1.86，并在配置阶段检查所需的取消 API。启用 HTTP/2 还需要静态 nghttp2 C 库，版本范围为 `>=1.70.0,<2`，不需要安装其命令行工具。HTTP/2 使用 OpenSSL 1.1.1+；nghttp2 处理协议和 HPACK，网络 I/O 仍由 Boost.Asio 驱动。

以下从仓库根目录配置，依赖前缀按实际安装位置调整：

```sh
cmake -S . -B build-h2 -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DHICAL_WITH_HTTP2=ON -DHICAL_WITH_HTTP_CLIENT=ON \
  -DHICAL_BUILD_EXAMPLES=ON -DHICAL_BUILD_TESTS=OFF \
  -DNghttp2_ROOT=/opt/nghttp2 -DNghttp2_USE_STATIC_LIBS=ON
cmake --build build-h2 --target http2_server http2_client
cmake --install build-h2 --prefix ./install-h2
```

Windows MinGW 环境应统一使用同一套 CMake、编译器和依赖，不能混用 MSVC 与 MinGW 库。例如 PowerShell：

```powershell
$env:PATH = "C:/msys64/mingw64/bin;" + $env:PATH
cmake -S . -B build-h2 -G Ninja -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_C_COMPILER=C:/msys64/mingw64/bin/gcc.exe `
  -DCMAKE_CXX_COMPILER=C:/msys64/mingw64/bin/g++.exe `
  -DCMAKE_PREFIX_PATH=C:/msys64/mingw64 `
  -DNghttp2_ROOT=F:/dependencies/nghttp2-static -DNghttp2_USE_STATIC_LIBS=ON `
  -DHICAL_WITH_HTTP2=ON -DHICAL_WITH_HTTP_CLIENT=ON `
  -DHICAL_BUILD_EXAMPLES=ON -DHICAL_BUILD_TESTS=OFF
cmake --build build-h2 --target http2_server http2_client
```

Hical 以静态库分发。外部项目通过 `find_package(hical CONFIG REQUIRED)` 和 `hical::hical_core` 使用导出的依赖、编译定义与线程 ABI 设置。MinGW 的 `BOOST_ASIO_HAS_PTHREADS`、Windows nghttp2 静态链接相关定义均随目标传递，消费者不要另写一套相冲突的宏。只有启用 `HICAL_WITH_HTTP_CLIENT` 才会安装 `asio/HttpClient.h` 和 `asio/HttpClientPool.h`。

把两个示例源码复制到一个外部项目后，可使用：

```cmake
cmake_minimum_required(VERSION 3.28)
project(hical_http2_examples LANGUAGES CXX)
find_package(hical CONFIG REQUIRED)
add_executable(http2_server http2_server.cpp)
add_executable(http2_client http2_client.cpp)
target_compile_features(http2_server PRIVATE cxx_std_20)
target_compile_features(http2_client PRIVATE cxx_std_20)
target_link_libraries(http2_server PRIVATE hical::hical_core)
target_link_libraries(http2_client PRIVATE hical::hical_core)
```

配置该项目时用 `-DCMAKE_PREFIX_PATH=<Hical安装前缀>`，并提供依赖的查找路径。两个源码只包含安装后的公开头，不需要引用内部 HTTP/2 头文件。

## 运行双端示例

先生成仅用于本地演示的证书。脚本包含 `localhost` 和 `127.0.0.1` 的 SAN；实际服务应换成受信任 CA 签发的证书，不提交私钥或临时证书。

```sh
python scripts/gen_test_certs.py --out-dir build-h2/example-certs
```

在第一个终端运行服务器。默认只监听 `127.0.0.1:8443`，30 秒后开始排空，排空上限为 2 秒：

```sh
./build-h2/examples/http2_server \
  build-h2/example-certs/test_server.crt \
  build-h2/example-certs/test_server.key 8443 30
```

在另一个终端启动客户端，CA 参数信任刚才生成的自签证书。`shared` 复用同一个 `HttpClient`，`pool` 使用连接池高层入口；两种方式都会并发提交一个 GET 和一个 POST，并检查完整响应：

```sh
./build-h2/examples/http2_client 127.0.0.1 8443 \
  build-h2/example-certs/test_server.crt h2 shared
./build-h2/examples/http2_client 127.0.0.1 8443 \
  build-h2/example-certs/test_server.crt auto pool
./build-h2/examples/http2_client 127.0.0.1 8443 \
  build-h2/example-certs/test_server.crt h1 shared
```

Windows 使用相同参数，程序名带 `.exe`；多配置生成器还需要使用对应的 `Release` 子目录。客户端连接超时为 2 秒，请求总时限为 5 秒，并有 8 秒的示例总运行保护。正常完成后显式 `close()` 或 `shutdown()`，让 `io_context::run()` 处理收尾后自然返回。

本机示例和验收服务端监听 `127.0.0.1`，外部 `nghttpd` 也显式绑定回环地址，无需为本机测试开放外网入站。客户端连接 `127.0.0.1` 并不能证明服务端只监听回环，调整夹具时应同时核对服务端的监听配置。

## 原生验收与性能采样

原生工具的准备、完整命令与证据解释见 [互操作夹具说明](../docker/test/http2/README.md)，执行入口为 [test_http2_interop.py](../scripts/test_http2_interop.py)，不要求 Docker Desktop。当前 Windows 本机双向互操作以 `accepted_with_reviewed_differences` 接受：h2spec 有效集合为 146 项，其中 139 项通过、7 项为逐项审查的 RFC 9113 差异。完整 h2spec 原始进程仍退出 1，原始失败状态和日志均保留；这不等于全绿或完整 RFC 9113 合规。精确签名与边界见[七项审查记录](../docker/test/http2/h2spec-rfc9113.json)。安装后示例的 `h1/auto/h2 × shared/pool` 六种组合，以及面向独立 `nghttp/nghttpd` 的真实交换均已通过。

[bench_http2.py](../scripts/bench_http2.py) 比较四种变体：v2.7.2 基线 `cab5e33`、候选 H2 OFF、候选 H2 ON 但按 H1 运行、候选实际 H2。对比时固定同一工具链、构建配置及适用静态依赖的版本和来源；关闭 H2 的变体不额外链接 nghttp2。通过 `--identity-manifest` 关联各自的源码快照、构建日志和实际二进制 SHA256；脚本核对二进制哈希并保留声明原文，不能用当前 Git HEAD 代替未提交源码的身份，也不能仅凭哈希不同就认定对比对象正确。

各轮正反交替运行四变体，客户端的共享实例与高层池也交替采样；保留原始日志、请求延迟、进程采样及工具身份，再按同场景取中位数。请求区间吞吐与压测进程整段吞吐分别记录。RSS 是整个进程的采样工作集，包含固定成本和缓存；除以配置连接数或连接×流槽数只是描述性商，不是边际每连接/流内存。CPU 是采样区间的进程用户态加内核态时间，以单核为百分比基准，不是整机占用。

`mixed-slow` 输入列表为 9 个 `/hello` 加 1 个 `/slow`，当前 P50/P99 是两类请求的合并统计，不能当作快请求的单独尾延迟或流公平性证明。服务端探针未暴露队列峰值，报告保留为空并附说明；不能用在途请求数代替。客户端高层池使用实际 `peakQueuedRequests`，共享实例没有同口径的池队列指标。 客户端延迟包含准入等待、首次建连和完整响应比较；池队列的生命周期峰值包含初始建连积压，不能当作稳态队列长度。本轮 Windows 矩阵使用 1×1、1×8、4×8 三种配置，交替两轮、每样本 512 请求；原始结果随本地验收记录保存。短回环样本不作生产性能收益保证，互操作冒烟也不能替代性能测量。

## 服务端接入

启动前调用 `enableSsl(cert, key)` 和 `enableHttp2(options)`，两者先后顺序不限。缺少 TLS 时启动会明确失败；只创建 `Http2Options` 不会启用 HTTP/2。启用后的服务端优先协商 `h2`，也接受 HTTP/1.1 回退，原有 H1 路由无需复制一份。

`request.protocol()` 可查询实际协议。初始头和尾头分开放在 `headers` 与 `trailers` 中，同名字段也不会合并。响应可用 `setTrailer()` 设置 H2 尾字段；HTTP/1 分块尾字段仍通过原 `ChunkedBody::end()` 接口处理。HTTP/2 不发送 HTTP/1 状态行、`Transfer-Encoding: chunked` 或分块长度文本。

中间件的阶段语义保持一致：普通前置中间件在完整 body 读取之前运行；需要 body 的逻辑使用相应 body 阶段或 handler。现有 `ChunkedBody` 是已收集内容的响应描述，不是无限流式生产接口。`FileBody` 保留 path、offset、length 及原有 Range/ETag 处理，文件内容按窗口和待发预算分批读取，不先缓存整个文件。

`gracefulStop()` 停止接纳新连接和新流，并向 H2 连接发送 GOAWAY，让已接纳的请求在原排空期限内完成。重复调用无副作用；`setShutdownTimeout()` 配置排空上限，`stop()` 用于强制关闭。服务器对象必须活到阻塞的 `start()` 返回，业务任务也必须有自己的有限等待和清理逻辑。

## 客户端协议与 TLS 身份

`HttpClientOptions::protocolMode` 有三种策略：

| 策略 | 行为 |
| --- | --- |
| `hHttp1Only` | 默认值，维持 HTTP/1.1 |
| `hNegotiateHttp2` | TLS ALPN 提议 h2 和 HTTP/1.1；对端只选 H1 或没有 ALPN 时回退 H1 |
| `hHttp2Only` | 必须协商到 h2，否则返回协议协商错误 |

H2 模块关闭时后两种策略明确返回协议不可用。HTTP/2 客户端需要非空 `tlsContext`；TLS 验证失败不会自动重连明文或绕过证书检查。

`verifyPeer=true` 同时验证证书链和构造客户端时的目标 `host`。DNS 名按证书中的 DNS SAN 验证，IP 按 IP SAN 验证。`sniHost` 仅覆盖 SNI 路由名称，不会改变证书身份验证目标。多个客户端共享 TLS context 时，各连接的 ALPN 和身份检查仍独立配置。先配置 context，再发起请求，使用期间不要并发修改它。

返回结果先检查 `result.error`，再检查 HTTP `status`。404/500 属于正常收到的 HTTP 响应，网络错误字段可以为空；`result.ok()` 同时要求无传输错误和 2xx。`result.protocol` 记录实际协商协议；H2 没有原因短语，因此 `statusText` 为空。`body` 在完整响应接收后返回，受配置的聚合上限约束；`trailers` 保存独立尾字段。

## 生命周期、期限与取消

`request()`、`get()`、`post()` 在返回 awaitable 前同步持有请求参数与内部状态。保存 awaitable 后，原 target/body/header 对象可以销毁；这不代表保存任意 handler 中的 `HttpRequest` 视图也安全。服务端请求中的视图仍受该请求生命周期约束，请求级 PMR 分配也不能逃逸。

请求总期限从普通客户端或池入口调用时开始，覆盖建连等待、排队、发送和完整响应；延后启动 awaitable 不会重新计时。`connectTimeout` 覆盖 DNS、TCP、TLS 建连阶段；`requestTimeout` 是整次请求的总限时，非正值关闭对应客户端时限。HTTP/2 的 `queueTimeout` 等阶段时限仍要求为正。

同一个 `HttpClient` 的首批请求共享一次建连；协商为 H1 后串行完成往返，协商为 H2 后在本地和对端并发配额内多路复用。超时或使用 Asio cancellation slot 取消一个 H2 请求，只结束该请求；还在等槽位、没有创建 stream 的请求直接退出队列。连接级故障会影响该连接上的未完成请求。`close()` 是关闭整条客户端连接的操作，不是取消单流的入口；随后新请求可以重新建连。

服务端收到 RST_STREAM、超时或断连后会停止该流输出并释放可立即回收的协议资源，但不会强制销毁正在运行的 handler 协程。数据库中间件和用户代码继续按原作用域清理：正常完成可能提交，异常路径按原事务规则回滚。RST_STREAM 本身不等于业务回滚，也不能代替幂等键、防重入或业务取消协议。仍在运行的 handler 会继续占用应用任务配额。

客户端 facade 析构会取消其旧请求并投递清理；已保存的 awaitable 可以安全启动并得到取消结果。`io_context` 必须覆盖所有未完成操作、读写泵和析构投递的清理。示例先结束业务请求并关闭资源，再在 facade 析构后继续运行一次事件循环，避免清理任务留在已经停止的 loop 中。

## 连接池与重试边界

连接池每个 `io_context` 一份，普通请求、`acquire()`、`release()`、统计查询都在所属 loop 上调用；不要从多个线程同时操作同一池。`shutdown()` 和析构会将清理投递回所属 loop，因此关池后也要让 loop 继续处理取消完成。

池身份包括目标主机、端口、TLS context 身份、有效 SNI、验证方式、协议策略和生效的 H2 配置，不跨 authority 合并。高层 `request/get/post` 根据 H2 流容量共享连接，受 `maxConnectionsPerOrigin` 与 `maxQueuedRequestsPerOrigin` 约束。对端 SETTINGS 把并发上限设为零或降低时，新请求等待容量，不超发；协商退回 H1 后按独占往返调度。

高层池在已有连接上等待流槽位时，使用原请求总期限和 `queueTimeout`，不会因为 `connectTimeout` 较短就提前结束容量等待。如果排队期间原连接断开，池内也没有其他已连接传输，则为每个等待者启动它自己的建连子期限（`connectTimeout` 为正时）。共享一次握手不代表共享调用方的时限；重复容量通知和重新入队都不延长已经启动的子期限。连接按时就绪后解除建连子期限，后续仍使用原请求总期限和队列截止点。

旧 `acquire/release` 仍是独占租借，与高层共享调度分开。借出的连接要在全部请求结束后归还，池的 `shutdown()` 不代替外部租借者关闭业务连接。`stats()` 需在所属 loop 读取，统计高层协商/H2 调度，包括回退的 H1；它不包含旧租借表或 `hHttp1Only` 桶。

`requestDisposition` 只表达传输证据：`hNotSent` 为尚未发送，`hNotProcessed` 为协议能证明未处理，`hPossiblyProcessed` 为可能已处理。`status == 0` 不能证明业务没有执行；普通断连也不能推断 POST 安全重放。REFUSED_STREAM 与 GOAWAY 的 last-stream-id 可提供更强的协议证据。

只有池的高层 `request/get/post` 入口会按以下条件自动重试，最多增加一次尝试，总共不超过两次。普通 `HttpClient` 和旧 `acquire/release` 租借不会自动获得池高层的重试调度。可重试的方法仅限 GET、HEAD、PUT、DELETE、OPTIONS；POST、PATCH 默认不自动重放，即使 REFUSED_STREAM 或 GOAWAY 已明确证明未处理，也不例外。

| 路径与证据 | 自动重试条件 |
| --- | --- |
| 协商/H2 高层入口，`hNotSent` | 尚未发送，且错误属于下述瞬态传输白名单；包括 ALPN 尚未完成、结果协议仍为默认 H1 的建连失败 |
| H2，`hNotProcessed` | 真实 REFUSED_STREAM，或 GOAWAY last-stream-id 证明本流未处理，并带有相应协议错误元数据 |
| H2，`hPossiblyProcessed` | 不自动重放，普通 EOF、断连或 reset 不能自行升级成“未处理” |
| H1 | 仅第一次使用的是复用连接、尚未取得响应状态（`status == 0`），且遇到瞬态白名单错误时，丢弃陈旧连接再尝试一次 |

瞬态白名单为 `hEof`、`hConnectionReset`、`hConnectionRefused`、`hConnectionAborted`、`hNetworkUnreachable`、`hHostUnreachable`、`hBrokenPipe`。原生取消、超时、TLS 证书/身份错误、配置错误及资源预算错误不重试；`status == 0` 单独也不构成重试条件。

第二次尝试使用入口持有的请求体副本，继续受原请求绝对期限约束。H2 首次实际进入队列时才开始计算 `queueTimeout`，之后重试保留同一队列截止时间，不重新领取完整时限。重试前也会再次检查取消和停机。当前没有公开的重试配置字段；如需业务重试，应由应用结合幂等性和处理证据自行决定。

这是相对 v2.7.2 的兼容性变化：旧 H1 池分支可能在陈旧连接失败后自动重放 POST；当前默认不再重放 POST/PATCH。依赖旧行为的调用方应显式处理失败，不能把普通断连当作业务未执行。版本迁移说明见 [Unreleased](../CHANGELOG.md#unreleased)。

## 配额与协议范围

`Http2Options` 默认提供有限预算，`validate()` 会拒绝零容量、非法窗口或相互矛盾的配置。常用边界如下：

| 配置 | 约束 |
| --- | --- |
| `maxConcurrentStreams` | 单连接活跃协议流上限；客户端还取对端 SETTINGS 的较小值 |
| `maxApplicationTasks` / `maxServerApplicationTasks` | 每连接及整个服务器尚未收尾的应用任务上限，含已 reset 的 handler |
| `maxHeaderBytes` / `maxHeaderFields` | 每个初始头块、尾头块分别限制；每个原始字段按 `name + value + 32` 字节计费，含伪头，字段数最多 64 |
| `maxBodyBytes` / `maxConnectionReceiveBytes` | 服务端单流请求体及连接接收预算；还与已有服务端 body/header 上限取较小值 |
| `maxConnectionSendBytes` | 所有待发响应及文件预取共同占用的连接预算 |
| `maxOutputBatchBytes` / `maxFilePrefetchBytes` | 单批输出及单流文件预取大小，均不能超过连接待发预算 |
| `maxResponseBytes` / `maxClientConnectionReceiveBytes` | 客户端单流和整连接响应聚合上限 |
| `negotiationTimeout` / `headerTimeout` / `bodyTimeout` / `writeTimeout` / `queueTimeout` | 独立阶段的有限期限，PING 或小量控制帧不会不断刷新请求体期限 |

这些配额约束框架持有的任务和缓冲，不表示整个进程 RSS 等于预算总和；TLS、协议库和业务对象仍占内存。调大窗口或并发前应测量自己的 body 大小、慢流比例和任务收尾时间。

目前范围是 TLS HTTP/2 请求与完整响应，不支持 h2c、HTTP/1 Upgrade 到 H2、CONNECT/扩展 CONNECT、HTTP/2 WebSocket、TLS SSE、HTTP/2 服务端推送或 gRPC。启用 HTTP/2 不会让既有 WS/SSE 入口自动变成 H2 流；这些能力需要各自的协议和生命周期设计。

相关接口见 [HttpServer](../src/core/HttpServer.h)、[Http2Options](../src/core/Http2Options.h)、[HttpClient](../src/asio/HttpClient.h) 和 [HttpClientPool](../src/asio/HttpClientPool.h)，完整示例见 [服务端](../examples/http2_server.cpp) 与 [客户端](../examples/http2_client.cpp)。
