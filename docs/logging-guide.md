# C++ Web 服务日志最佳实践：Hical 日志系统完全指南

---

## 引子：生产环境 printf 调试？该升级了

不少 C++ 服务器项目在早期会这样写日志：

```cpp
printf("[INFO] user login: uid=%d\n", uid);
fprintf(stderr, "[ERROR] db connect failed\n");
```

这没什么问题——直到你的服务跑到生产环境，遇到以下场景：

1. **日志文件膨胀**：跑了三天，单个 app.log 已经 8GB，grep 一下需要等几分钟
2. **性能抖动**：每次写日志都 `fwrite` + `fflush`，高并发时 I/O 成为瓶颈
3. **信息不够**：出了问题只知道"某某接口报错"，不知道是哪个请求、哪个用户
4. **无法动态调级**：想临时开 DEBUG 排查问题，必须重启服务
5. **日志散落各处**：访问日志、审计日志、业务日志混在同一个文件里，难以分析

Hical 的日志系统正是为了解决这五个问题而设计的。本文从最简用法出发，逐步覆盖文件轮转、异步写盘、结构化日志、通道分流、HTTP 集成到运行时调级，每个场景都给出可直接复制的代码。

---

## 1. 快速上手：三种 API 对比

Hical 日志提供三种书写风格，适用不同场景：

### `std::format` 风格（首选）

```cpp
#include <hical/Log.h>

HICAL_LOG_INFO("server started on port={}", 8080);
HICAL_LOG_WARN("connection pool low, available={}", pool.available());
HICAL_LOG_ERROR("db query failed: sql={} err={}", sql, ec.message());
```

格式字符串在编译期校验（借助 `std::format_string<Args...>`），参数类型不匹配直接报错，不会等到运行时才崩溃。这是最常见的用法。

### 流式 API（复杂拼接场景）

```cpp
HICAL_LOG_DEBUG_STREAM << "packet dump: " << hexDump(buf) << " size=" << buf.size();
```

适合需要把多个值拼在一起、或者使用自定义 `operator<<` 的场景。内部使用栈上 `FixedBuffer<4096>`，超出后自动 fallback 到堆，避免频繁分配。

### 条件日志（高频路径节省开销）

```cpp
// 只有条件为真时才格式化字符串，比 if + LOG 略简洁
HICAL_LOG_DEBUG_IF(req.method() == HttpMethod::EPost, "POST body size={}", req.body().size());
```

三种 API 都遵循同一个原则：**级别不满足时零开销**。级别判断是一次 `atomic::load`，满足则继续格式化，不满足直接跳过，不产生任何字符串分配。

---

## 2. 级别与过滤：六级体系

Hical 定义了六个日志级别，对应不同使用场景：

| 宏后缀  | 枚举值   | 典型用途                                                    |
| ------- | -------- | ----------------------------------------------------------- |
| `TRACE` | `hTrace` | 极细粒度调试（循环内、协议字节级），**NDEBUG 下编译期消除** |
| `DEBUG` | `hDebug` | 开发期调试信息，不进生产                                    |
| `INFO`  | `hInfo`  | 正常运行信息（启动、请求摘要），**生产默认级别**            |
| `WARN`  | `hWarn`  | 值得关注但不影响服务的异常（重试、降级）                    |
| `ERROR` | `hError` | 需要处理的错误，服务仍在运行                                |
| `FATAL` | `hFatal` | 不可恢复错误，自动 flush 全部缓冲区然后 `abort()`           |

### 运行时级别设置

```cpp
#include <hical/Log.h>

// 开发环境：显示所有级别
Logger::instance().setLevel(LogLevel::hDebug);

// 生产环境：只看 INFO 及以上
Logger::instance().setLevel(LogLevel::hInfo);
```

### flush 阈值的意义

Hical 把"输出级别"和"flush 级别"分开控制：

```cpp
// >= WARN 的日志立即 flush，低级别日志由后台线程批量刷盘
Logger::instance().setFlushLevel(LogLevel::hWarn);
```

这个设计很实用：大量 INFO 日志可以缓冲后批量写，节省 I/O；但 WARN/ERROR 这种需要关注的信息要立即落盘，避免服务崩溃时丢失最后几条关键日志。

### TRACE 的 NDEBUG 消除

```cpp
// Debug 构建：正常输出
// Release 构建（定义 NDEBUG）：整个语句被替换为 ((void)0)，没有任何开销
HICAL_LOG_TRACE("loop iteration i={} val={}", i, val);
```

对于内层循环中的诊断日志，用 TRACE 而不是 DEBUG，Release 构建完全零开销。

---

## 3. 文件轮转：不让日志撑爆磁盘

默认的 `StderrSink` 只写 stderr，生产环境需要落文件。`FileSink` + `LogFile` 处理按大小自动轮转：

```cpp
#include <hical/LogSink.h>
#include <hical/LogFile.h>

auto fileSink = std::make_shared<FileSink>(LogFile::Options {
    .basePath    = "logs/app.log",  // 当前文件路径
    .maxFileSize = 100 * 1024 * 1024, // 100MB 触发轮转
    .maxFiles    = 10,              // 最多保留 10 个归档文件
});

Logger::instance().setSink(fileSink); // 替换默认的 StderrSink
```

轮转时当前文件会被 rename 为带时间戳序列的归档名（如 `app.20260501-142505.000001.log`），然后重新打开 `app.log` 继续写入。**当前文件名永远是 `app.log`**，便于 `tail -f` 跟踪。

超出 `maxFiles` 的最旧归档文件会被自动删除。

---

## 4. 异步写盘：高并发下消除 I/O 抖动

`FileSink` 是同步的——每条日志都会阻塞当前线程直到 `fwrite` 返回。在高并发场景下，这会导致请求处理线程被磁盘 I/O 卡住。

`AsyncFileSink` 用双缓冲 + 后台线程解决这个问题：

```cpp
#include <hical/AsyncFileSink.h>

auto asyncSink = std::make_shared<AsyncFileSink>(AsyncFileSink::Options {
    .file = {
        .basePath    = "logs/app.log",
        .maxFileSize = 200 * 1024 * 1024, // 200MB
        .maxFiles    = 20,
    },
    .bufferSize        = 4 * 1024 * 1024, // 前台缓冲区 4MB
    .backpressureLimit = 8 * 1024 * 1024, // 积压超 8MB 开始丢弃
    .flushInterval     = std::chrono::milliseconds {500}, // 最迟 500ms 刷一次盘
});

Logger::instance().setSink(asyncSink);
```

**双缓冲原理**（简明版）：前台线程写 `m_curBuf`（mutex 保护），后台线程每隔 `flushInterval` 或缓冲区满时执行 `swap`，把 `m_curBuf` 和 `m_flushBuf` 对调，然后把 `m_flushBuf` 批量写盘。两个缓冲区不同时被读写，锁竞争极小。

**背压保护**：当积压超过 `backpressureLimit` 时，新日志被丢弃（不阻塞调用方），并通过原子计数器记录丢弃数量：

```cpp
auto dropped = asyncSink->droppedCount();
if (dropped > 0)
{
    HICAL_LOG_WARN("async sink dropped {} log records due to backpressure", dropped);
}
```

> **选择建议**：开发环境用 `StderrSink`（输出即可见），生产环境用 `AsyncFileSink`（高吞吐），测试/集成环境用 `OStreamSink`（可注入 `stringstream` 验证输出）。

---

## 5. 结构化日志：给 ELK/Grafana 喂机器可读数据

纯文本日志人读起来还行，但 ELK / Loki / Splunk 更喜欢 JSON Lines——每行一个 JSON 对象，字段语义明确，不需要写正则解析。

切换到 JSON 格式只需换一个 Formatter：

```cpp
#include <hical/LogFormatter.h>

Logger::instance().setFormatter(std::make_shared<JsonFormatter>());
```

输出效果：

```json
{"timestamp":"2026-05-01T14:25:05.123Z","level":"INFO","thread":12345,"file":"HttpServer.cpp","line":87,"message":"server started on port=8080"}
```

### 附加结构化字段

有时候日志消息里的信息还不够，希望把 userId、requestId 这类关键字段单独提出来，方便聚合查询：

```cpp
#include <hical/Log.h>

// 需要 include LogRecord.h（Log.h 已间接 include）
HICAL_LOG_INFO_F(
    {{"userId", 10086}, {"action", "login"}, {"ip", "192.168.1.1"}},
    "user login success"
);
```

JSON 输出会把 `fields` 内的键值对合并进去：

```json
{"timestamp":"...","level":"INFO","message":"user login success","userId":10086,"action":"login","ip":"192.168.1.1"}
```

这样在 Kibana 里就能直接 `userId: 10086` 过滤，而不是从 message 字段里 parse。

---

## 6. 通道分流：访问日志、业务日志各走各路

实际项目里往往有多类日志，混在一起不好管：

- **访问日志**：记录每个 HTTP 请求（method、path、status、耗时），量大，需要 JSON 格式
- **业务日志**：游戏逻辑、用户行为等，量中等，人类可读格式即可
- **审计日志**：资金、道具操作，需要单独的文件，保留时间更长

Hical 用**命名通道**（`LogChannel`）实现分流，每个通道有独立的级别、Formatter 和 Sink：

```cpp
#include <hical/Log.h>
#include <hical/LogChannel.h>
#include <hical/LogFormatter.h>
#include <hical/AsyncFileSink.h>

// --- 访问日志通道：JSON 格式 + 异步文件 ---
auto accessSink = std::make_shared<AsyncFileSink>(AsyncFileSink::Options {
    .file = {.basePath = "logs/access.log", .maxFileSize = 500 * 1024 * 1024, .maxFiles = 30},
});

Logger::instance()
    .channels()
    .getOrCreate("access")
    ->setLevel(LogLevel::hInfo)
    .setFormatter(std::make_shared<JsonFormatter>())
    .addSink(accessSink);

// --- 审计日志通道：JSON 格式 + 同步文件（不能丢） ---
auto auditSink = std::make_shared<FileSink>(LogFile::Options {
    .basePath = "logs/audit.log",
    .maxFileSize = 1024 * 1024 * 1024, // 1GB，保留更多
    .maxFiles = 90,
});

Logger::instance()
    .channels()
    .getOrCreate("audit")
    ->setLevel(LogLevel::hInfo)
    .setFormatter(std::make_shared<JsonFormatter>())
    .addSink(auditSink);

// --- 默认 Logger：文本格式 + stderr（开发可见）+ 异步文件 ---
auto appSink = std::make_shared<AsyncFileSink>(AsyncFileSink::Options {
    .file = {.basePath = "logs/app.log"},
});

Logger::instance().setFormatter(std::make_shared<TextFormatter>());
Logger::instance().addSink(std::make_shared<StderrSink>());
Logger::instance().addSink(appSink);
```

向指定通道写日志：

```cpp
// 写审计通道
HICAL_LOG_TO("audit", Info, "item transfer: from={} to={} itemId={} count={}", fromUid, toUid, itemId, count);

// 带结构化字段写审计通道
HICAL_LOG_TO_F("audit", Warn,
    {{"fromUid", fromUid}, {"toUid", toUid}, {"itemId", itemId}, {"count", count}},
    "item transfer"
);
```

通道不存在时 `HICAL_LOG_TO` 静默忽略，不会崩溃。

---

## 7. HTTP 集成：一行代码搞定 trace-id + 访问日志

Hical 的 `makeLogMiddleware()` 是一个洋葱模型中间件，放在中间件链靠前位置，自动完成两件事：

1. **生成 trace-id**：为每个请求生成 32 字节十六进制随机 ID，注入 `req` 属性，并在响应头里回传给客户端
2. **写访问日志**：请求处理完毕后，把 method、path、status、耗时写到 `access` 通道

```cpp
#include <hical/HttpServer.h>
#include <hical/LogMiddleware.h>

HttpServer server;

server.use(makeLogMiddleware({
    .accessLogChannel = "access",  // 写到哪个通道
    .autoTraceId      = true,      // 自动生成并注入 trace-id
    .traceIdHeader    = "X-Trace-Id", // 响应头名称
}));

// 其他中间件 / 路由注册...
```

在业务处理器里提取 trace-id，传递给下游调用或写到日志里：

```cpp
server.get("/api/user/{id}", [](HttpRequest& req) -> Awaitable<HttpResponse>
{
    auto traceId = getTraceId(req); // 获取本请求的 trace-id

    HICAL_LOG_INFO_F(
        {{"traceId", traceId}, {"userId", req.param("id")}},
        "fetch user request"
    );

    // ... 业务逻辑
    co_return HttpResponse::ok().body("{}");
});
```

访问日志的 JSON 输出示例：

```json
{"timestamp":"2026-05-01T14:25:05.456Z","level":"INFO","message":"GET /api/user/42 200 12ms","traceId":"a3f2...","method":"GET","path":"/api/user/42","status":200,"latency_ms":12}
```

有了 trace-id，当用户报告"某某请求返回 500"时，你可以直接用 trace-id 从日志里捞出整条链路，不用靠时间猜。

---

## 8. 运行时调级：不重启服务临时开 DEBUG

`registerLogAdminEndpoints()` 注册两个 HTTP 端点，允许运行时动态调整日志级别：

```cpp
#include <hical/LogAdmin.h>

Router router;
// 生产环境务必传入认证回调，防止未授权访问
registerLogAdminEndpoints(router, "/admin", [](const HttpRequest& req) -> std::optional<HttpResponse>
{
    auto token = req.header("Authorization");
    if (token != "Bearer your-secret-token")
    {
        auto res = HttpResponse();
        res.setStatus(HttpStatusCode::hUnauthorized);
        res.setBody("Unauthorized", "text/plain");
        return res;
    }
    return std::nullopt; // 通过认证
});
```

**查询当前级别**：

```bash
curl -H "Authorization: Bearer your-secret-token" http://localhost:8080/admin/log-level
```

```json
{"default":"INFO","channels":{"access":"INFO","audit":"INFO"}}
```

**调整默认级别**（临时开 DEBUG 排查问题）：

```bash
curl -X PUT \
     -H "Authorization: Bearer your-secret-token" \
     -H "Content-Type: application/json" \
     -d '{"level":"DEBUG"}' \
     http://localhost:8080/admin/log-level
```

**只调整某个通道**（不影响其他通道）：

```bash
curl -X PUT \
     -H "Authorization: Bearer your-secret-token" \
     -H "Content-Type: application/json" \
     -d '{"channel":"access","level":"WARN"}' \
     http://localhost:8080/admin/log-level
```

排查完毕后再调回 INFO，全程不需要重启服务。

---

## 9. 完整初始化示例

把上面所有配置组合在一起，放在 `main()` 里：

```cpp
#include <hical/HttpServer.h>
#include <hical/Log.h>
#include <hical/LogAdmin.h>
#include <hical/LogFormatter.h>
#include <hical/LogMiddleware.h>
#include <hical/AsyncFileSink.h>
#include <hical/LogSink.h>

int main()
{
    // --- 1. 配置默认 Logger ---
    Logger::instance().setLevel(LogLevel::hInfo);
    Logger::instance().setFlushLevel(LogLevel::hWarn);
    Logger::instance().setFormatter(std::make_shared<TextFormatter>());
    Logger::instance().addSink(std::make_shared<StderrSink>());
    Logger::instance().addSink(std::make_shared<AsyncFileSink>(AsyncFileSink::Options {
        .file = {.basePath = "logs/app.log", .maxFileSize = 100 * 1024 * 1024, .maxFiles = 10},
        .bufferSize        = 4 * 1024 * 1024,
        .backpressureLimit = 8 * 1024 * 1024,
    }));

    // --- 2. 配置 access 通道 ---
    Logger::instance()
        .channels()
        .getOrCreate("access")
        ->setLevel(LogLevel::hInfo)
        .setFormatter(std::make_shared<JsonFormatter>())
        .addSink(std::make_shared<AsyncFileSink>(AsyncFileSink::Options {
            .file = {.basePath = "logs/access.log", .maxFileSize = 500 * 1024 * 1024, .maxFiles = 30},
        }));

    // --- 3. 构建 HTTP 服务器 ---
    HttpServer server;

    server.use(makeLogMiddleware({
        .accessLogChannel = "access",
        .autoTraceId      = true,
    }));

    // 注册业务路由...

    // --- 4. 注册运行时调级端点 ---
    registerLogAdminEndpoints(server.router(), "/admin");

    HICAL_LOG_INFO("server starting on port=8080");
    server.listen(8080);
    server.run();
    return 0;
}
```

---

## 10. 与 spdlog / glog 的差异

| 特性       | spdlog            | glog         | Hical Log                                        |
| ---------- | ----------------- | ------------ | ------------------------------------------------ |
| API 风格   | `fmt::format`     | `<<` 流式    | 两者都支持，`std::format`                        |
| 异步写盘   | 有（异步 logger） | 无           | `AsyncFileSink` 双缓冲                           |
| 文件轮转   | 有                | 有           | `LogFile`，大小轮转 + 数量限制                   |
| 结构化字段 | 无内置            | 无内置       | `HICAL_LOG_INFO_F` + JSON Lines                  |
| 通道路由   | sink 可分 logger  | 无           | `LogChannel` 命名通道，独立 level/formatter/sink |
| HTTP 集成  | 无                | 无           | `makeLogMiddleware()` 一键 trace-id + 访问日志   |
| 运行时调级 | 无 HTTP 端点      | 无 HTTP 端点 | `LogAdmin` REST 端点                             |
| 协程友好   | 线程局部安全      | 线程局部安全 | 设计上与 Boost.Asio 协程共存，无 TLS 阻塞        |

Hical 日志的差异化在于**和 HTTP 框架的深度集成**：trace-id 的生成、传递、写入访问日志、运行时调级，都是框架层面的一等公民，不需要自己拼装。如果你用的是纯 spdlog，这些都得自己搭。

对于已经在用 spdlog 的项目，也可以把 spdlog 注册为一个 `LogSink`（实现 `write()`/`flush()` 桥接），迁移成本极低。

---

## 总结

5 个常见需求的对应方案：

| 需求       | 方案                                               |
| ---------- | -------------------------------------------------- |
| 级别过滤   | `setLevel()` + 6 级枚举，TRACE 在 Release 零开销   |
| 文件轮转   | `FileSink` + `LogFile::Options`（大小 + 数量限制） |
| 异步不阻塞 | `AsyncFileSink`（双缓冲 + 背压保护）               |
| 结构化字段 | `JsonFormatter` + `HICAL_LOG_INFO_F`               |
| 分布式追踪 | `makeLogMiddleware()` 自动 trace-id + 通道路由     |

完整 API 参考：`src/core/Log.h`、`src/core/LogChannel.h`、`src/core/LogMiddleware.h`、`src/core/LogAdmin.h`。
