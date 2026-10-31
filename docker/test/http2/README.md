# 原生 HTTP/2 互操作夹具

这里的探针使用生产公开 API，不依赖 `core/http2/` 内部头。
目录名沿用既有测试组织方式，运行无需 Docker、WSL 或容器服务。
两个探针可用于本机互操作检查；跨平台结果仍待最终验收。
下面的本机结果不能代替其他平台和 Sanitizer 的执行结果。

## 构建入口

`tests/CMakeLists.txt` 已登记两个普通可执行目标：`http2_interop_server` 和 `http2_interop_client`。
后者仅在 `HICAL_WITH_HTTP_CLIENT=ON` 时存在。复用已配置的 H2/客户端构建目录：

```powershell
cmake --build build-http2 --config Release --target http2_interop_server http2_interop_client
```

继承 `hical_core` 的 include、C++20、依赖和公开 feature 宏。
H2 消费构建须显式打开 `HICAL_WITH_HTTP2` 与 `HICAL_WITH_HTTP_CLIENT`，使用 nghttp2 >=1.70,<2 及经过项目验证的 Boost >=1.86。
不要把目标名注册成虚假的 GoogleTest 测试；真实外部工具场景由 Python 驱动集中执行。
ASan/UBSan、TSan、Valgrind 和 CI 矩阵需要分别执行并保存结果。

`server_probe.cpp` 的 H2 调用、尾字段及能力标识受 `HICAL_HAS_HTTP2` 隔离。
Hello、JSON、echo、文件、gzip、慢处理和就绪机制尽量使用 v2.7.2 已有接口，方便用同一夹具比较基线和候选版本。
同一探针已实际链接并运行 v2.7.2 基线；场景仍须逐项校验，不能由构建成功推断所有响应都正确。

## 行为与时限

服务端固定监听 `127.0.0.1:0`，由 OS 分配实际端口。
`--gzip-body-bytes` 默认为 `0`，gzip 路由使用完整夹具；非零值仅限制该路由的原始正文，
不改变文件、范围或 POST 数据。就绪文件同时发布 gzip 正文的实际字节数与哈希。
性能主矩阵显式使用 32KiB 整体 gzip；256KiB 大正文另测候选配置，保留基线的旧 CL/TE 报文失败。
同步中间件在 H1 大正文路径使用 chunked 封装，H2 发送原始 gzip 字节，不能统一称为流式压缩测量。
只有 `HttpServer::start()` 的真实 owner io_context 执行回调后才原子发布 `ready.json`，内含端口、模式、TLS/H2 能力和文件哈希。
启动前路由均已注册，GET/POST `/` 返回非空 200，满足 h2spec 服务器前提。
存在以下路由：

| 路由 | 行为 |
| --- | --- |
| `/hello` | 固定 Hello 完整文本 |
| `/json-dom`、`/json-dto`、`/json-writer` | 相同 message/answer，分别走三类生产 JSON API |
| `POST /echo` | 完整二进制请求体回写 |
| `/file.bin` | 生产 `setFileBody` 异步发送夹具 |
| `/range.bin` | 固定 offset=717、length=87001 的 206 文件响应；不冒充通用 Range 解析器 |
| `/trailer` | H2 独立 `x-interop-checksum` 尾字段 |
| `/gzip` | 同一大夹具，使用生产 gzip 中间件 |
| `/slow` | 有限协程等待；默认 50ms，参数限定 1～1000ms |

服务端接收 `--mode h1|h2`、`--tls on|off`、`--cert`、`--key`、`--ready`、`--stop-file`、`--file` 和有限寿命参数。
stop-file 和寿命 timer 走正常 `stop()`；独立原生线程 watchdog 在 io_context 停滞时以 124 退出。
客户端提供 `shared`（单个 HttpClient）、`pool-lease`（同一公开租借对象）和 `pool`（高层池接口）三种入口，支持 GET/POST、并发、H1/H2/协商。
每条响应都校验协议、状态和完整 body，并记录 SHA256、大小、错误、头和尾字段。
客户端同时有 owner io timer 和独立线程 watchdog；全部异步交换结束后才关闭资源。
Python 对每个子进程另设真实时限，所有路径 finally 回收；启动失败、零测试、跳过、超时和停机失败均保留为失败。

## 驱动命令

显式传入真实工具、探针和证书路径，使用全新输出目录。以下为本地 MinGW 构建的完整命令，
其中审查参数明确接受后文列出的七项旧套件差异；删除这一参数便恢复默认严格失败策略：

```powershell
python scripts/test_http2_interop.py `
  --server-driver build-http2/tests/http2_interop_server.exe `
  --client-driver build-http2/tests/http2_interop_client.exe `
  --nghttp build-http2-deps/cygwin-tools/bin/nghttp.exe `
  --nghttpd build-http2-deps/cygwin-tools/bin/nghttpd.exe `
  --h2load build-http2-deps/cygwin-tools/bin/h2load.exe `
  --h2spec build-http2-deps/h2spec-tools/bin/h2spec.exe `
  --cert build-http2/tests/test_server.crt --key build-http2/tests/test_server.key `
  --ca build-http2/tests/test_server.crt --external-insecure `
  --h2spec-max-header-length 3000 `
  --h2spec-rfc9113-review docker/test/http2/h2spec-rfc9113.json `
  --out build-http2-deps/interop-reviewed-001
```

Linux 使用各工具实际路径和证书路径，参数相同。工具默认为已准备的 nghttp2 1.69.0 与固定 h2spec 2.6.0；
驱动仅依赖 Python 3.11+ 标准库；无需额外 pip 安装。
其他已审核 nghttp 工具版本需显式设置 `--expected-nghttp-version`，每轮仍保存真实版本和二进制哈希。
外部工具 1.69 与实现库 >=1.70 严格区分。
`--external-insecure` 仅用于本地自签名测试证书的外部工具；生产 HttpClient 和 Python nghttpd 就绪握手仍加载指定 CA 并检查主机身份。
证书须包含 127.0.0.1 SAN；缺失证书直接失败，不 SKIP，不在系统证书存储安装 CA。
MSVC 多配置构建将两个探针路径改为实际 `tests/Release/` 目录，证书仍按生成时的真实路径填写。

本机独立 Cygwin 工具目录已创建 `etc/nsswitch.conf`（仅 `passwd: files`、`group: files`）和 `tmp/`，
避免工具启动时等待域 LDAP，并提供工具自己的临时目录。没有改系统 NSS、全局 PATH 或系统临时目录设置。
驱动在报告中保存该局部 NSS 文件的路径和哈希；迁移工具目录时应重新核对其实际依赖和目录布局。

`--section server|client|h1|h2spec|h2load` 可重复选择组件；省略时执行全部。
`--prepare-only` 只记录文件身份，不运行任何工具、探针或网络；未构建 driver 可以省略并明确记为 unavailable。
`--build-source-identity` 填写实际构建使用的源码版本或快照标识，观察到的工作树哈希不会被冒充为候选提交 SHA。
本互操作驱动用于 ON 场景及候选的 H1 回退；OFF 矩阵不运行外部 H2 工具。
v2.7.2/OFF 性能基线可直接使用相同 server probe 的 `--mode h1`，以及 client probe 的 `--mode h1`。

## 证据解释

所有命令、cwd、起止时间、超时、退出码和 stdout/stderr 原始二进制日志及哈希保存在输出目录。
nghttp `-O` 在官方实现中尚未实现，不能拿它假称已保存 body：驱动用一次 `-v -n` 多 URI 请求的帧日志/HAR 确认一条 TLS 连接多个真实 stream 及每条 DATA 长度；
另以无 `-v` 的独立请求 stdout 逐字节验证所有完整 body。gzip 额外检查 Content-Encoding，nghttp 的 stdout 是自动解压后的完整 body。
这两类证据在报告中分别记录，不把独立下载说成同连接的逐流 body 哈希。
Hical client→nghttpd 通过真实 verbose 日志的 session ID、stream ID、随机 run-id 头核对同一连接上的多条完整响应。
nghttpd 没有公开端口 0 实际值的接口，因此仅该外部进程使用“临时 socket 取端口再启动”；若发生端口交接竞争，按真实启动失败处理。
h2load 只执行 16 请求/1 连接/8 流的冒烟，不用于性能结论。

h2spec 固定为 `2.6.0` / 提交 `70ac2294010887f48b18e2d64f5cccd48421fad1`，版本输出必须精确匹配。
[官方 README](https://github.com/summerwind/h2spec/blob/v2.6.0/README.md) 定义 RFC 7540、RFC 7541 和 generic 组，
而 [RFC 9113 附录 B](https://www.rfc-editor.org/rfc/rfc9113.html#appendix-B) 包含优先级、Upgrade、TLS1.3、字段校验等变化。
每个 JUnit 用例保留原始 suite/package/classname/name、完整失败载荷、原始状态和跳过内容。
旧套件通过也不构成完整 RFC 9113 合规结论；其他验收项仍需逐项确认。

## h2spec 输入与精确审查

默认完整套件使用 `--max-header-length 3000`，使 `http2/6.10/1` 的两段合法头块保持在已通告的 8192 字节预算内。
但这一长度会让 `http2/4.2/3` 没有生成其描述所要求的超过 16KiB 的单个 HEADERS 帧。
驱动因此另以 4000 字节仅复验 `http2/4.2/3`。若显式将完整套件长度设为 4000 或更大，
则反向仅以 3000 字节复验 `http2/6.10/1`。不提高生产端头预算，不把 CONTINUATION 失败纳入差异名单。

完整旧套件有 146 个不同案例，额外精确复验 1 次，因此 `executed_case_count` 是 147、
`effective_case_count` 是 146。`raw_runs` 完整保留两轮原始结果，包括完整套件中尺寸输入不合适的失败；
有效集合只替换这一精确身份的案例，而且复验必须实际通过。
输入不匹配案例的已知原始失败载荷也按精确内容核对，出现其他失败不得被替换掩盖。
`--h2spec-strict` 会额外执行旧套件的一项 SHOULD 检查，此时对应计数为 148 / 147，新增失败同样严格处理。

默认不采用任何 RFC 差异审查，七项旧预期失败仍使驱动退出 1。
只有显式传入 `--h2spec-rfc9113-review`，才读取 [固定审查文件](h2spec-rfc9113.json)：

| 精确用例 | 独立审查结论及边界 |
| --- | --- |
| `http2/5.1/8`、`/9`、`/11`、`/12` | RFC 9113 §5.1 允许对所有 closed 流最小处理；仍须同步 HPACK、计算连接流控，且不能重新派发业务。 |
| `http2/5.1.1/2` | 固定输入先开 5，3 已隐式 closed。仅覆盖该输入，不接受流 0、偶数流或实际非法新建流；§5.1.1 的 unexpected ID 错误要求仍适用。 |
| `http2/5.3.1/2` | §5.3.2 弃用旧自依赖语义；§6.3 的线格式和非零流 ID 校验仍适用，PRIORITY 不得占用新流 ID。 |
| `http2/6.9.1/3` | §5.4.1 允许将窗口溢出的流错误升级为连接错误；仍须检出溢出并终止连接，不接受忽略错误。 |

匹配同时要求固定版本/提交、精确 package/classname/name，以及完整 failure/error 类型、属性和载荷。
审查文件的七项签名本身也固定，不能增加案例或修改失败内容来放宽结果；每轮记录文件 SHA256、理由、边界和 RFC 链接。
`http2/6.3/1` 的 PRIORITY 流 0 缺陷必须修复并通过，不能加入名单。
任意新失败、载荷变化、skipped、缺失/重复案例、JUnit 缺失、整轮超时、异常退出或退出码与用例结果不一致，均仍失败。
七项中旧用例内部的 `Timeout` 是被精确审查的原始载荷；它不等于允许整个 h2spec 进程超时。

`h2spec-cases.json` 分别记录：

- `raw_ok`：所有实际执行的原始案例均通过；尺寸输入失败或七项旧差异都使它为 false。
- `accepted`：两种输入、完整执行和精确审查满足本轮策略，可能同时存在原始失败。
- `accepted_with_reviewed_differences`：本轮确实采用了已审差异。此时 stage 为 `accepted_with_reviewed_differences`，
  根报告为 `component_scenarios_accepted_with_reviewed_differences`，明确标注“有已审差异”。

原始 case 的 `status` 不改为 passed，原进程退出码、日志和审查解释全部保留。
