# 自研日志系统踩坑实录：从 printf 到生产级的 8 个教训

## 引言

Hical 没有用 spdlog、glog 或任何第三方日志库——整套日志系统完全自研，覆盖 20+ 个源文件：格式化、Sink 后端、文件轮转、异步双缓冲、通道分流、HTTP 集成、运行时调级。

自研日志的好处在[开发心得](14-development-insights.md)里聊过了。这篇只聊坑——从"能跑"到"能在生产环境跑"的过程中，踩过的 8 个真实问题。

---

## 目录

- [自研日志系统踩坑实录：从 printf 到生产级的 8 个教训](#自研日志系统踩坑实录从-printf-到生产级的-8-个教训)
  - [引言](#引言)
  - [目录](#目录)
  - [坑 1：异步写盘的背压——日志不应该成为延迟来源](#坑-1异步写盘的背压日志不应该成为延迟来源)
  - [坑 2：双缓冲析构时丢日志](#坑-2双缓冲析构时丢日志)
  - [坑 3：多线程 Sink 分发的锁竞争——COW 模式的引入](#坑-3多线程-sink-分发的锁竞争cow-模式的引入)
  - [坑 4：日志注入——一条恶意日志伪造十行告警](#坑-4日志注入一条恶意日志伪造十行告警)
  - [坑 5：LogAdmin 的审计致盲攻击](#坑-5logadmin-的审计致盲攻击)
  - [坑 6：TRACE 日志在 Release 构建中的隐性开销](#坑-6trace-日志在-release-构建中的隐性开销)
  - [坑 7：trace-id 生成的性能瓶颈——OpenSSL 全局锁](#坑-7trace-id-生成的性能瓶颈openssl-全局锁)
  - [坑 8：文件轮转的误删风险](#坑-8文件轮转的误删风险)
  - [总结：自研日志系统的检查清单](#总结自研日志系统的检查清单)

---

## 坑 1：异步写盘的背压——日志不应该成为延迟来源

**现象**：压测时发现 P99 延迟间歇性飙高。排查发现不是业务逻辑慢，而是日志写入阻塞了请求线程。

**根因**：早期版本的日志直接在调用线程同步 `fwrite`。高并发下磁盘 I/O 成为瓶颈，日志调用从微秒级退化为毫秒级。

引入异步 `AsyncFileSink` 后，新的问题出现——如果日志产生速度远超写盘速度（比如某个 bug 触发了大量 ERROR 日志），前端缓冲区会无限增长直到 OOM。

**解决方案**：背压保护——缓冲区超限时主动丢弃，而非阻塞或 OOM：

```cpp
// AsyncFileSink.cpp — write() 中的背压丢弃
void AsyncFileSink::write(std::string_view formattedLine)
{
    std::lock_guard<std::mutex> lock(m_bufMutex);

    // 背压保护：缓冲区过大时丢弃（防止内存爆炸）
    if (m_curBuf.size() > m_opts.backpressureLimit)
    {
        m_dropped.fetch_add(1, std::memory_order_relaxed);
        return;  // 丢弃这条日志，但不阻塞调用线程
    }

    m_curBuf.append(formattedLine.data(), formattedLine.size());

    // 缓冲区接近满时通知后台线程
    if (m_curBuf.size() >= m_opts.bufferSize)
    {
        m_cond.notify_one();
    }
}
```

关键细节：丢弃计数不是默默吞掉的——后台线程在每次刷盘时检查丢弃计数，并将统计写入日志文件：

```cpp
// AsyncFileSink.cpp — backgroundLoop 中注入丢弃统计
auto dropped = m_dropped.exchange(0, std::memory_order_relaxed);
if (dropped > 0)
{
    char dropMsg[128];
    auto n = snprintf(dropMsg, sizeof(dropMsg),
        "[WARN] AsyncFileSink: %llu log lines dropped due to backpressure\n",
        static_cast<unsigned long long>(dropped));
    m_logFile.append(dropMsg, static_cast<size_t>(n));
}
```

**经验**：日志系统的第一优先级是**不影响业务**。宁可丢日志也不能阻塞请求线程。但丢弃必须可观测——否则关键错误日志静默丢失，排障时就是噩梦。

默认配置：`bufferSize = 4MB`，`backpressureLimit = 8MB`，留出 2 倍缓冲空间。正常情况下后台线程在 buffer 满之前就 swap 走了，背压丢弃只在极端场景触发。

---

## 坑 2：双缓冲析构时丢日志

**现象**：服务正常关闭后，最后几条日志没有写入文件。

**根因**：`AsyncFileSink` 使用双缓冲模式——前端写 `m_curBuf`，后台线程 swap 到 `m_flushBuf` 再写盘。析构时如果只是让后台线程退出循环，`m_curBuf` 中积累的、尚未被 swap 的日志就丢了。

**第一版（有 bug）的析构**：

```cpp
// ❌ 后台线程收到 stop 信号直接退出
// m_curBuf 中的残余数据丢失
~AsyncFileSink()
{
    // jthread 析构自动 request_stop() + join()
}
```

**修复后的完整析构流程**：

```cpp
// AsyncFileSink.cpp — 析构前唤醒后台线程
AsyncFileSink::~AsyncFileSink()
{
    // 先唤醒后台线程，让它有机会处理当前 buf
    {
        std::lock_guard<std::mutex> lock(m_bufMutex);
        m_cond.notify_one();
    }
    // jthread 析构时自动 request_stop + join
    // 后台线程在 stop_requested 后会执行残余排空逻辑
}
```

后台线程的 break 后必须二次检查 `m_curBuf`：

```cpp
// backgroundLoop 主循环退出后的残余排空
// 停止前确保 m_curBuf 残余数据也被写入
{
    std::lock_guard<std::mutex> lock(m_bufMutex);
    if (!m_curBuf.empty())
    {
        m_curBuf.swap(m_flushBuf);
        m_curBuf.clear();
    }
}
if (!m_flushBuf.empty())
{
    m_logFile.append(m_flushBuf.data(), m_flushBuf.size());
    m_logFile.flush();
}

// 关闭前通知所有残留的 flush 等待者
{
    std::lock_guard<std::mutex> lock(m_bufMutex);
    for (auto& req : m_flushRequests)
    {
        req.set_value();
    }
    m_flushRequests.clear();
}
```

**经验**：异步日志的"最后一公里"问题——正常运行时不会发现，只在关闭时出现。测试必须覆盖"写几条日志然后立即析构"的场景。`jthread` 的 `stop_token` + `condition_variable_any` 组合让优雅关闭变得简洁，但残余数据排空逻辑仍需手动编写。

---

## 坑 3：多线程 Sink 分发的锁竞争——COW 模式的引入

**现象**：8 核压测时，日志吞吐量远低于预期。火焰图显示大量时间花在 `Logger::emit()` 的 mutex 竞争上。

**根因**：第一版 emit() 在整个格式化 + 分发过程中持有 mutex：

```cpp
// ❌ 第一版——锁持有时间太长
void Logger::emit(const LogRecord& record)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto formatted = m_formatter->format(record);  // 格式化在锁内
    for (auto& sink : m_sinks)
    {
        sink->write(formatted);  // I/O 在锁内
    }
}
```

格式化（字符串拼接 + 时间戳）+ I/O（fwrite/网络发送）都在临界区内，8 个线程抢一把锁，吞吐量退化为单线程。

**解决方案**：COW（Copy-on-Write）模式——锁内只拷贝 `shared_ptr`（O(1)），格式化和 I/O 全部在锁外执行：

```cpp
// Log.cpp — COW 快照分发
void Logger::emit(const LogRecord& record)
{
    // 锁内：仅拷贝两个 shared_ptr（2 次 atomic_inc）
    std::shared_ptr<LogFormatter> fmtSnap;
    std::shared_ptr<const std::vector<std::shared_ptr<LogSink>>> sinksSnap;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        fmtSnap = m_formatter;
        sinksSnap = m_sinks;  // m_sinks 是 shared_ptr<const vector<...>>
    }
    // 锁已释放

    // 锁外：格式化（纯函数，无共享状态）
    LogFormatter* formatter = fmtSnap ? fmtSnap.get() : defaultTextFormatter().get();
    auto formattedLine = formatter->format(record);

    // 锁外：分发到各 Sink（Sink 自行保证线程安全）
    for (const auto& sink : *sinksSnap)
    {
        if (record.level >= sink->sinkLevel())
        {
            sink->write(formattedLine);
        }
    }
    // ...
}
```

写路径（`addSink`）才需要创建新 vector：

```cpp
void Logger::addSink(std::shared_ptr<LogSink> sink)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    // 拷贝到新 vector，追加后替换 shared_ptr
    auto newSinks = std::make_shared<std::vector<std::shared_ptr<LogSink>>>(*m_sinks);
    newSinks->push_back(std::move(sink));
    m_sinks = std::move(newSinks);  // 原子替换
    // 正在 emit 的线程仍持有旧 sinksSnap，不受影响
}
```

**效果**：锁持有时间从"格式化 + N 次 I/O"降为"2 次 atomic_inc"。8 核压测吞吐量提升 ~6 倍。

**经验**：日志系统的读路径（emit）远多于写路径（addSink/setSink）——典型的读多写少场景。COW 模式让读路径几乎无锁，写路径虽然多了一次 vector 拷贝，但 addSink 一辈子只调用几次，可以忽略。

---

## 坑 4：日志注入——一条恶意日志伪造十行告警

**现象**：安全审计时发现，如果用户输入包含 `\n`，可以在日志文件中伪造额外的日志行。

**最小复现**：

```cpp
// 用户输入包含换行符
std::string userInput = "normal\n[ERROR] [admin] unauthorized access detected";
HICAL_LOG_INFO("user said: {}", userInput);

// 日志文件中出现两行：
// [2025-01-01 12:00:00.000] [INFO] [12345] user said: normal
// [ERROR] [admin] unauthorized access detected      ← 伪造的告警
```

更危险的是 ANSI 转义序列注入——如果日志输出到终端：

```cpp
// \033[31m 是红色文本、\033[2J 是清屏
std::string evil = "ok\033[31m CRITICAL SECURITY BREACH \033[0m";
```

**解决方案**：`sanitizeForText()` 转义所有控制字符：

```cpp
// LogFormatter.cpp — 日志注入防护
std::string sanitizeForText(std::string_view sv)
{
    std::string out;
    out.reserve(sv.size());
    for (unsigned char c : sv)
    {
        if (c == '\n')
        {
            out += "\\n";      // 换行 → 可见文本
        }
        else if (c == '\r')
        {
            out += "\\r";      // 回车 → 可见文本
        }
        else if (c == '\033')  // ESC — ANSI 转义序列入口
        {
            out += "\\e";      // 防止终端颜色/清屏注入
        }
        else if (c < 0x20 && c != '\t')  // 其余控制字符（保留 tab）
        {
            out += '?';        // 替换为安全字符
        }
        else
        {
            out += static_cast<char>(c);
        }
    }
    return out;
}
```

在 `TextFormatter::format()` 中，`traceId` 和 `message` 都经过清洗：

```cpp
// LogFormatter.cpp — 关键调用位置
result += sanitizeForText(record.traceId);   // trace-id 可能来自客户端请求头
result += sanitizeForText(record.message);   // message 可能包含用户输入
```

**为什么保留 `\t`？**

Tab 是合法的可见字符（展示为空白），常用于格式化表格日志。转义 tab 会破坏日志的可读性，且 tab 不构成安全威胁。

**为什么用 `unsigned char` 循环？**

`char` 在某些平台上是 signed，负值（如 0x80-0xFF 的 UTF-8 字节）会导致 `c < 0x20` 判断异常。强转 `unsigned char` 确保正确覆盖 0x00-0x1F 范围。

**经验**：日志看似"只写不读"，但日志分析系统（Splunk、ELK）会解析日志行——恶意换行可以伪造日志条目，误导自动化告警。spdlog 和 glog 都不做这个防御——自研时必须自己加。

---

## 坑 5：LogAdmin 的审计致盲攻击

**现象**：设计 LogAdmin（运行时日志级别调整端点）时，安全审查指出了一个攻击场景。

**攻击流程**：

```
攻击者：
1. PUT /admin/log-level {"level": "FATAL"}
   → 日志级别被改为 Fatal（只记录致命错误，其他日志全部静默）
2. 发起真正的攻击（SQL 注入、越权等）
   → 所有 WARN/ERROR 日志不再记录
3. PUT /admin/log-level {"level": "INFO"}
   → 恢复正常级别，痕迹消失
```

如果 PUT 端点不需要认证，任何能访问该端口的客户端都可以执行这个攻击——先"致盲"审计日志，再发起攻击，最后恢复原状，无痕。

**解决方案**：PUT 操作在无认证回调时**默认拒绝**（而非默认放行）：

```cpp
// LogAdmin.cpp — 安全默认值
router.put(prefix + "/log-level",
    [authCheck](const HttpRequest& req) -> HttpResponse
    {
        // 安全默认值：PUT 修改操作在无认证回调时默认拒绝
        // 防止"审计致盲"攻击
        if (!authCheck)
        {
            boost::json::object err;
            err["error"] = "admin endpoint requires authentication";
            auto res = HttpResponse::json(err);
            res.setStatus(HttpStatusCode::hForbidden);
            return res;
        }
        if (auto denied = authCheck(req))
        {
            return *denied;
        }
        // ... 修改逻辑
    });
```

对比 GET 操作——只读，安全性要求低：

```cpp
// GET 端点在无认证时默认放行（只读操作）
router.get(prefix + "/log-level",
    [authCheck](const HttpRequest& req) -> HttpResponse
    {
        if (authCheck)
        {
            if (auto denied = authCheck(req))
                return *denied;
        }
        // ... 查询逻辑（只读）
    });
```

同时，级别变更操作本身会用 WARN 级别记录到日志（在级别实际变更之前）：

```cpp
HICAL_LOG_WARN("LogAdmin: default level changed to {}", logLevelToString(*levelOpt));
```

这确保了即使攻击者把级别改为 Fatal，这条"级别已变更"的 WARN 日志也已经写入了（因为 WARN 在变更前仍然生效）。

**经验**：管理端点的安全默认值应该是"拒绝"而非"放行"。写操作和读操作的安全策略必须分开设计。

---

## 坑 6：TRACE 日志在 Release 构建中的隐性开销

**现象**：Release 构建中，日志级别设为 INFO，TRACE 日志理论上不该执行。但性能剖析发现 `std::format` 的参数求值仍然在发生。

**根因**：如果 TRACE 宏只是在运行时检查级别，参数表达式仍会被求值：

```cpp
// ❌ 运行时检查——参数仍然求值
#define HICAL_LOG_TRACE(fmt, ...) \
    do { \
        if (Logger::instance().level() <= LogLevel::hTrace) \
            Logger::instance().logFmt(LogLevel::hTrace, __FILE__, __LINE__, fmt, __VA_ARGS__); \
    } while(0)

// 调用
HICAL_LOG_TRACE("packet dump: {}", hexDump(hugeBuffer));
// 即使级别不满足，hexDump(hugeBuffer) 也会执行！
```

对于 DEBUG 级别这可以接受（开销只是一次 atomic load + 短路跳过）。但 TRACE 级别通常用在循环内、协议字节级调试——即使"只求值不格式化"，开销也无法接受。

**解决方案**：NDEBUG 下编译期完全消除 TRACE：

```cpp
// Log.h — Release 构建
#ifdef NDEBUG
    // 零开销：编译为 (void)0，参数表达式完全消除
    #define HICAL_LOG_TRACE(fmt, ...) ((void)0)
    #define HICAL_LOG_TRACE_IF(cond, fmt, ...) ((void)0)
    #define HICAL_LOG_TRACE_F(fields, fmt, ...) ((void)0)
#endif
```

流式版本稍微复杂——需要让编译器"看到"代码但永不执行，以保留 IDE 的语法检查和代码补全：

```cpp
// NDEBUG 下的流式 TRACE——if(true){}else 技巧
#define HICAL_LOG_TRACE_STREAM \
    if (true)                  \
    {                          \
    }                          \
    else                       \
        ::hical::LogMessageVoid() & ::hical::LogStream(LogLevel::hTrace, __FILE__, __LINE__)
```

原理：
- `if (true) {}` —— 永真条件，空分支体，编译器 DCE（Dead Code Elimination）消除
- `else LogStream(...)` —— 永不执行的代码，但语法合法——IDE 能解析、补全、类型检查
- 编译器最终生成零字节代码

对比 DEBUG 级别——保留运行时检查（可动态调整）：

```cpp
// Debug 仍用运行时级别判断（不编译期消除）
#define HICAL_LOG_DEBUG_STREAM                                           \
    if (::hical::Logger::instance().level() > ::hical::LogLevel::hDebug) \
    {                                                                    \
    }                                                                    \
    else                                                                 \
        ::hical::LogMessageVoid() & ::hical::LogStream(LogLevel::hDebug, __FILE__, __LINE__)
```

**经验**：TRACE 和 DEBUG 应该有不同的消除策略。TRACE 是编译期消除（Release 中完全不存在），DEBUG 是运行时消除（可以通过 LogAdmin 动态开启排查问题）。

---

## 坑 7：trace-id 生成的性能瓶颈——OpenSSL 全局锁

**现象**：高并发下 `makeLogMiddleware` 的 trace-id 生成成为瓶颈。火焰图指向 `RAND_bytes()`。

**根因**：第一版 trace-id 使用 OpenSSL 的 `RAND_bytes()` 生成密码学安全随机数。但 `RAND_bytes` 在某些 OpenSSL 版本上有全局锁——多线程同时调用时产生竞争。

而 trace-id 只需要**全局唯一**，不需要**密码学安全**——它不是 session ID，不用于签名或加密。

**解决方案**：`thread_local std::mt19937_64`——每线程独享 PRNG，零锁竞争：

```cpp
// LogMiddleware.cpp — 无锁 trace-id 生成
std::string generateTraceId()
{
    // thread_local PRNG：trace-id 只需全局唯一，不需要密码学安全
    // 避免 OpenSSL RAND_bytes 的跨平台全局锁竞争
    thread_local std::mt19937_64 sRng(std::random_device{}());

    uint64_t hi = sRng();
    uint64_t lo = sRng();

    static constexpr char kHex[] = "0123456789abcdef";
    std::string result(32, '\0');

    for (int i = 15; i >= 0; --i)
        result[15 - i] = kHex[(hi >> (i * 4)) & 0xf];
    for (int i = 15; i >= 0; --i)
        result[31 - i] = kHex[(lo >> (i * 4)) & 0xf];

    return result;  // 32 字符十六进制 = 128 位
}
```

同时，如果客户端请求头已经携带了 trace-id（跨服务链路追踪），优先采纳——但必须格式校验：

```cpp
// LogMiddleware.cpp — 客户端 trace-id 格式校验
bool isValidTraceId(std::string_view id)
{
    if (id.empty() || id.size() > 128) return false;
    for (unsigned char c : id)
    {
        bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')
               || (c >= 'A' && c <= 'F') || c == '-';
        if (!ok) return false;
    }
    return true;
}

// 中间件中的使用
auto clientTraceId = req.header(opts.traceIdHeader);
if (!clientTraceId.empty() && isValidTraceId(clientTraceId))
{
    traceId = std::string(clientTraceId);  // 采纳客户端的
}
else
{
    traceId = generateTraceId();  // 自行生成
}
```

**为什么要校验客户端的 trace-id？**

因为 trace-id 会被注入到日志的 `traceId` 字段——如果不校验，恶意客户端可以注入包含控制字符的值（参见坑 4 日志注入）。白名单只允许十六进制字符和 `-`。

**经验**：性能敏感路径上的随机数生成，要区分"需要密码学安全"和"只需要唯一"。Session ID 用 `RAND_bytes`（需要不可预测），trace-id 用 `mt19937_64`（只需要唯一）。

---

## 坑 8：文件轮转的误删风险

**现象**：测试环境中，日志轮转清理误删了同目录下的其他文件。

**根因**：第一版轮转清理只检查文件名前缀和后缀：

```cpp
// ❌ 宽松匹配——可能误删
if (name.starts_with("app.") && name.ends_with(".log"))
{
    rotatedFiles.push_back(path);
}
// 会匹配到：app.config.log、app.backup.log 等非轮转文件
```

**解决方案**：严格匹配轮转文件名格式——中间段必须恰好是 `YYMMDD-HHMMSS.NNNNNN`（20 字符）：

```cpp
// LogFile.cpp — 严格匹配清理
if (name.starts_with(m_stem + ".") && name.ends_with(m_ext))
{
    auto prefixLen = m_stem.size() + 1;  // "app."
    auto suffixLen = m_ext.size();        // ".log"
    if (name.size() > prefixLen + suffixLen)
    {
        auto middle = name.substr(prefixLen, name.size() - prefixLen - suffixLen);
        // 必须恰好 20 字符：YYMMDD-HHMMSS.NNNNNN
        // 且第 7 位是 '-'，第 14 位是 '.'
        if (middle.size() == 20 && middle[6] == '-' && middle[13] == '.')
        {
            rotatedFiles.push_back(entry.path().string());
        }
    }
}
```

轮转文件名的格式设计也很讲究：

```cpp
// LogFile.cpp — 时间戳 + 序列号命名
// 格式：app.YYMMDD-HHMMSS.NNNNNN.log
// 例如：app.250506-143022.000001.log
```

**设计决策**：

| 决策                 | 理由                                       |
| -------------------- | ------------------------------------------ |
| YY 而非 YYYY         | 文件名短，且日志文件寿命不会跨世纪         |
| 序列号 `NNNNNN`      | 同一秒内多次轮转时避免文件名冲突           |
| 时间戳编码在文件名中 | `ls -la` 排序 = 时间排序，无需解析文件内容 |
| 严格格式校验         | 防止误删同前缀的其他文件                   |

清理策略是按文件名字典序排序（= 时间序），删除最老的文件直到不超过 `maxFiles`：

```cpp
std::sort(rotatedFiles.begin(), rotatedFiles.end());
while (rotatedFiles.size() > m_opts.maxFiles)
{
    fs::remove(rotatedFiles.front(), ec);
    rotatedFiles.erase(rotatedFiles.begin());
}
```

**经验**：文件轮转的清理逻辑必须严格匹配自己生成的文件名格式——宽松匹配会误删。文件名要包含足够的信息让字典序 = 时间序，避免额外解析。

---

## 总结：自研日志系统的检查清单

| #   | 坑             | 检查项                                  | Hical 的方案                     |
| --- | -------------- | --------------------------------------- | -------------------------------- |
| 1   | 异步写盘背压   | 前端缓冲区是否有上限？超限后怎么处理？  | 背压丢弃 + 丢弃计数注入          |
| 2   | 析构时丢日志   | 双缓冲的残余数据是否被排空？            | 后台线程 break 后二次检查 curBuf |
| 3   | 多线程锁竞争   | emit() 锁持有时间多长？                 | COW 快照，锁内仅拷贝 shared_ptr  |
| 4   | 日志注入       | 用户输入中的 `\n`/`\r`/ESC 是否转义？   | `sanitizeForText()` 防御         |
| 5   | 审计致盲       | 管理端点是否有认证？写操作默认拒绝？    | PUT 无认证默认 403               |
| 6   | TRACE 隐性开销 | Release 构建中 TRACE 参数是否还在求值？ | `#ifdef NDEBUG` 编译期消除       |
| 7   | 随机数锁竞争   | trace-id 生成是否用了全局锁？           | `thread_local mt19937_64`        |
| 8   | 轮转误删       | 清理时文件名匹配是否足够严格？          | 严格 20 字符中间段校验           |

核心原则：**日志系统是基础设施，它的 bug 比业务 bug 更难发现——因为你靠日志来发现 bug，但日志系统的 bug 让你看不到 bug。**

---

> **踩坑实录五部曲**：[编译差异](15-pitfall-compiler-differences.md) | [Asio 协程](16-pitfall-asio-coroutine.md) | [Boost.MySQL](17-pitfall-boost-mysql.md) | [日志系统](#)（本文） | [PMR 内存池](19-pitfall-pmr-memory-pool.md)
