# 从零构建现代C++ Web服务器（四）：实战案例与性能调优

> **系列导航**：[第一篇：设计理念](01-design-philosophy.md) | [第二篇：协程与内存池](02-coroutine-and-memory.md) | [第三篇：路由、中间件与SSL](03-router-middleware-ssl.md) | [第四篇：实战与性能](04-practice-and-performance.md) | [第五篇：Cookie、Session与文件服务](#)（本文）

## 前置知识

- 阅读过本系列前四篇（特别是第三篇的中间件洋葱模型）
- 了解 HTTP Cookie 与 Session 基本概念
- 了解 multipart/form-data 编码格式基本原理

---

## 目录

- [1. Web 应用的"最后一公里"](#1-web-应用的最后一公里)
- [2. Cookie：无状态协议的状态记忆](#2-cookie无状态协议的状态记忆)
- [3. Session：从 Cookie 到有状态会话](#3-session从-cookie-到有状态会话)
- [4. 静态文件服务：安全地托管资源](#4-静态文件服务安全地托管资源)
- [5. Multipart 文件上传：解析 RFC 7578](#5-multipart-文件上传解析-rfc-7578)
- [6. 综合实战：带登录的文件管理服务](#6-综合实战带登录的文件管理服务)
- [7. 全系列总结](#7-全系列总结)

---

## 1. Web 应用的"最后一公里"

经过前四篇的铺垫，hical 已经具备了完整的 HTTP 服务器骨架——协程驱动的异步 I/O、PMR 内存池、双策略路由、洋葱模型中间件、SSL/WebSocket 支持，以及反射宏系统。

但如果你真正尝试用它搭建一个 Web 应用，很快就会发现少了几样东西：用户登录后刷新页面状态丢失、无法提供前端静态资源、用户没法上传头像文件。这些功能看似"基础"，却是 Web 应用从"能跑通"到"能用"的**最后一公里**。

hical v1.0.0 补齐了这四块拼图：

| 模块            | 解决的问题                        | 集成方式                            | 核心文件                                        |
| --------------- | --------------------------------- | ----------------------------------- | ----------------------------------------------- |
| **Cookie**      | HTTP 无状态协议下的客户端状态存储 | `req.cookie()` / `res.setCookie()`  | `Cookie.h` `HttpRequest.cpp` `HttpResponse.cpp` |
| **Session**     | 服务端有状态会话管理              | `makeSessionMiddleware()` 中间件    | `Session.h` `Session.cpp`                       |
| **StaticFiles** | 安全地托管前端/资源文件           | `serveStatic()` 工厂函数            | `StaticFiles.h`                                 |
| **Multipart**   | 文件上传（RFC 7578）              | `MultipartParser::parse()` 静态方法 | `Multipart.h` `Multipart.cpp`                   |

它们在 hical 整体架构中的位置：

```
                         ┌─────────────────────────────────┐
                         │          用户请求 (HTTP)          │
                         └────────────────┬────────────────┘
                                          │
                         ┌────────────────▼────────────────┐
                         │        TcpServer (Accept)        │
                         └────────────────┬────────────────┘
                                          │
                         ┌────────────────▼────────────────┐
                         │   MiddlewarePipeline (洋葱模型)   │
                         │  ┌───────────────────────────┐  │
                         │  │  ★ Session 中间件 (本篇)   │  │
                         │  │  · CORS 中间件             │  │
                         │  │  · 日志中间件               │  │
                         │  └───────────────────────────┘  │
                         └────────────────┬────────────────┘
                                          │
                         ┌────────────────▼────────────────┐
                         │       Router (路由分发)          │
                         │  ┌──────────┬─────────────────┐ │
                         │  │ API 路由  │ ★ 静态文件路由   │ │
                         │  └──────────┴─────────────────┘ │
                         └────────────────┬────────────────┘
                                          │
                    ┌─────────────────────┼──────────────────────┐
                    │                     │                      │
         ┌──────────▼──────────┐ ┌────────▼─────────┐ ┌─────────▼────────┐
         │  ★ Cookie 解析/写入 │ │ ★ Multipart 解析 │ │  JSON / 普通响应  │
         │  req.cookie()       │ │ MultipartParser  │ │  req.jsonBody()  │
         │  res.setCookie()    │ │ ::parse()        │ │                  │
         └─────────────────────┘ └──────────────────┘ └──────────────────┘
```

> ★ 标记的即为本篇讲解的四个模块。

接下来我们逐个深入。

---

## 2. Cookie：无状态协议的状态记忆

### 2.1 为什么需要框架级 Cookie 支持？

HTTP 是无状态协议——每次请求独立，服务器不记得"你是谁"。Cookie 机制通过在响应中设置 `Set-Cookie` 头、在后续请求中携带 `Cookie` 头，让客户端帮服务器"记住"状态。

手动解析 Cookie 头并不复杂，但每个路由处理器都自己写一遍既冗余又容易出错：

```cpp
// ❌ 手动解析：每个 handler 都要重复这段逻辑
auto cookieHeader = req.header("Cookie");
// 手动 split "name1=value1; name2=value2; ..."
// 还要处理空格、分号、RFC 编码...
```

hical 提供了框架级 Cookie 支持，一行搞定：

```cpp
// ✅ hical 方式
auto token = req.cookie("auth_token");   // 读取
res.setCookie("theme", "dark", opts);    // 设置
```

### 2.2 惰性解析：用时才解析，省了就赚了

并非每个请求都需要读取 Cookie——API 接口可能只看 Authorization 头。如果在每个请求进入时就解析 Cookie 头，对不需要 Cookie 的路由而言是浪费。

hical 采用**惰性解析（lazy parsing）**策略：只有第一次调用 `req.cookie()` 或 `req.cookies()` 时才真正解析，之后缓存结果。

关键数据结构：

```cpp
// HttpRequest.h 中的成员
mutable std::optional<std::unordered_map<std::string, std::string>> cookies_;
```

为什么用 `mutable optional`？

| 设计选择        | 原因                                                             |
| --------------- | ---------------------------------------------------------------- |
| `mutable`       | `cookie()` 是 `const` 方法（不修改请求语义），但需要修改内部缓存 |
| `optional`      | 区分"未解析"（`nullopt`）和"已解析但为空"（空 map）              |
| `unordered_map` | O(1) 查找，Cookie 数量通常很少（<20），哈希开销可忽略            |

解析流程：

```
  req.cookie("name")
         │
         ▼
  cookies_ 有值？ ──是──► 直接查找返回
         │
         否
         │
         ▼
  parseCookies()
         │
         ▼
  读取 "Cookie" 头
         │
         ▼
  按 ';' 分割键值对
         │
         ▼
  try_emplace（first-wins）
         │
         ▼
  缓存到 cookies_
         │
         ▼
  查找返回
```

核心实现（`HttpRequest.cpp:177-226`）：

```cpp
void HttpRequest::parseCookies() const
{
    cookies_.emplace();
    auto cookieHeader = header("Cookie");
    if (cookieHeader.empty())
    {
        return;
    }

    // 解析 "name1=value1; name2=value2; ..." 格式
    std::string_view sv(cookieHeader);
    while (!sv.empty())
    {
        // 跳过前导空格
        while (!sv.empty() && sv.front() == ' ')
        {
            sv.remove_prefix(1);
        }

        // 查找分隔符 ';'
        auto semi = sv.find(';');
        std::string_view pair = (semi != std::string_view::npos)
                                    ? sv.substr(0, semi) : sv;
        sv = (semi != std::string_view::npos)
                 ? sv.substr(semi + 1) : std::string_view {};

        // 分割 name=value
        auto eq = pair.find('=');
        if (eq == std::string_view::npos)
        {
            continue;
        }
        std::string_view name = pair.substr(0, eq);
        std::string_view value = pair.substr(eq + 1);

        // 去除 name 首尾空格
        while (!name.empty() && name.front() == ' ')
        {
            name.remove_prefix(1);
        }
        while (!name.empty() && name.back() == ' ')
        {
            name.remove_suffix(1);
        }

        if (!name.empty())
        {
            // RFC 6265：同名 Cookie 以先出现的值为准（first-wins）
            (*cookies_).try_emplace(std::string(name), std::string(value));
        }
    }
}
```

**`try_emplace` 的 first-wins 语义**是一个重要细节。RFC 6265 规定：当客户端发送多个同名 Cookie 时，服务器应以第一个出现的为准。`try_emplace` 在键已存在时不会覆盖，恰好满足这个要求——比 `insert` 更高效（不需要构造临时 pair），比 `operator[]` 更安全（不会意外覆盖）。

### 2.3 Set-Cookie 构建：RFC 6265 安全编码

设置 Cookie 比解析更复杂——不仅要拼接字符串，还要处理安全属性和字符编码。

hical 的 `CookieOptions` 结构体涵盖了所有 RFC 6265 规定的属性：

```cpp
struct CookieOptions
{
    std::string path = "/";    // Cookie 作用路径，默认 "/"
    std::string domain;        // Cookie 作用域名，空表示当前域
    int maxAge = -1;           // 有效期（秒），-1 表示会话 Cookie
    bool httpOnly = false;     // 禁止 JavaScript 访问（防 XSS）
    bool secure = false;       // 仅通过 HTTPS 传输
    std::string sameSite;      // SameSite 策略："Strict"/"Lax"/"None"
};
```

各属性的安全含义：

| 属性       | 默认值       | 安全作用                                  |
| ---------- | ------------ | ----------------------------------------- |
| `path`     | `"/"`        | 限制 Cookie 发送路径                      |
| `domain`   | 空（当前域） | 防止跨子域泄露                            |
| `maxAge`   | `-1`（会话） | 控制持久化时长                            |
| `httpOnly` | `false`      | **防 XSS**：JS 无法读取 `document.cookie` |
| `secure`   | `false`      | **防中间人**：仅 HTTPS 传输               |
| `sameSite` | 空           | **防 CSRF**：`Strict`/`Lax` 限制跨站发送  |

Cookie 值必须经过 RFC 6265 编码。规范规定的合法字符范围：

```
cookie-value = *cookie-octet
cookie-octet = %x21 / %x23-2B / %x2D-3A / %x3C-5B / %x5D-7E
               │       │          │          │          │
               !       #到+       -到:       <到[       ]到~
```

不在此范围内的字符需要百分号编码。`HttpResponse.cpp:77-95` 中的 `encodeCookieValue` 实现了这个逻辑：

```cpp
auto encodeCookieValue = [](const std::string& raw) -> std::string
{
    std::ostringstream encoded;
    encoded << std::hex << std::uppercase;
    for (unsigned char c : raw)
    {
        bool safe = (c == 0x21)
                    || (c >= 0x23 && c <= 0x2B)
                    || (c >= 0x2D && c <= 0x3A)
                    || (c >= 0x3C && c <= 0x5B)
                    || (c >= 0x5D && c <= 0x7E);
        if (safe)
        {
            encoded << static_cast<char>(c);
        }
        else
        {
            encoded << '%' << std::setw(2) << std::setfill('0')
                    << static_cast<int>(c);
        }
    }
    return encoded.str();
};
```

### 2.4 CRLF 注入防护

**HTTP Response Splitting（HTTP 响应拆分）** 是一种经典攻击手段：如果攻击者能在 Cookie 的 name 或 value 中注入 `\r\n`（CRLF），就能伪造 HTTP 头部甚至注入完整的 HTTP 响应。

攻击原理：

```
正常 Set-Cookie:
  Set-Cookie: name=value; Path=/

注入攻击（value 包含 \r\n）:
  Set-Cookie: name=evil\r\n
  Set-Cookie: admin=true\r\n     ← 伪造的头部
  \r\n                           ← 头部结束
  <script>alert('xss')</script>  ← 注入的响应体
```

hical 在 `setCookie` 入口处直接拦截（`HttpResponse.cpp:64-73`）：

```cpp
void HttpResponse::setCookie(const std::string& name,
                             const std::string& value,
                             const CookieOptions& options)
{
    // HTTP Response Splitting 防护：name/value 不允许包含 CR/LF
    auto containsCRLF = [](const std::string& s) -> bool
    {
        return s.find('\r') != std::string::npos
               || s.find('\n') != std::string::npos;
    };
    if (containsCRLF(name) || containsCRLF(value))
    {
        // 拒绝含控制字符的 Cookie，静默忽略
        return;
    }
    // ... 后续正常构建 Set-Cookie
}
```

**设计决策**：检测到 CRLF 时静默忽略而非抛异常。理由是：这通常意味着外部输入被恶意篡改，抛异常可能让攻击者利用错误信息探测服务器。生产环境建议加上告警日志。

### 2.5 多 Cookie：Beast `insert()` vs `set()` 的关键区别

一个 HTTP 响应可能需要设置多个 Cookie。HTTP 规范要求每个 Cookie 用独立的 `Set-Cookie` 头，不能合并到一行。

Boost.Beast 的 `message::set()` 方法会**覆盖**同名头部，而 `insert()` 会**追加**：

```cpp
// ❌ set() 会覆盖，第二个 Cookie 丢失
res_.set(http::field::set_cookie, "name1=value1");
res_.set(http::field::set_cookie, "name2=value2");
// 结果：只有 Set-Cookie: name2=value2

// ✅ insert() 追加，两个 Cookie 都保留
res_.insert(http::field::set_cookie, "name1=value1");
res_.insert(http::field::set_cookie, "name2=value2");
// 结果：
//   Set-Cookie: name1=value1
//   Set-Cookie: name2=value2
```

hical 在 `HttpResponse.cpp:126` 使用 `insert`：

```cpp
// Beast 不支持同名字段多值直接 set，使用 insert 追加多个 Set-Cookie
res_.insert(boost::beast::http::field::set_cookie, oss.str());
```

这是一个容易踩的坑——使用 `set()` 设置多个 Cookie 时只有最后一个生效，且不会报错。

---

## 3. Session：从 Cookie 到有状态会话

### 3.1 Cookie 不够用了？

Cookie 能存储少量客户端状态，但有明显的局限性：

| 对比维度 | Cookie               | Session                |
| -------- | -------------------- | ---------------------- |
| 存储位置 | 客户端浏览器         | 服务器内存/数据库      |
| 容量上限 | 4KB（RFC 6265 推荐） | 理论上无限             |
| 安全性   | 客户端可篡改/窃取    | 客户端只持有 ID        |
| 数据类型 | 仅字符串             | 任意类型（`std::any`） |
| 传输开销 | 每次请求携带全部数据 | 仅传输 Session ID      |
| 适用场景 | 偏好设置、跟踪标识   | 用户登录态、购物车     |

Session 的本质是：**客户端存一把钥匙（Session ID），服务端存一个保险箱（Session 数据）**。

### 3.2 Session 类：std::any 实现的万能保险箱

hical 的 Session 类核心设计非常简洁——一个线程安全的 key-value 存储：

```cpp
class Session
{
public:
    explicit Session(std::string id) : id_(std::move(id)) {}

    const std::string& id() const { return id_; }

    void set(const std::string& key, std::any value);

    template <typename T>
    std::optional<T> get(const std::string& key) const;

    bool has(const std::string& key) const;
    void remove(const std::string& key);
    void clear();
    void touch();

private:
    std::string id_;                                       // 构造后不可变
    mutable std::mutex mutex_;                             // 保护下面所有字段
    std::unordered_map<std::string, std::any> data_;       // 数据存储
    bool dirty_ = false;                                   // 是否被修改
    std::chrono::steady_clock::time_point lastAccess_      // 上次访问时间
        = std::chrono::steady_clock::now();
};
```

关键设计决策：

| 设计选择              | 原因                                                                |
| --------------------- | ------------------------------------------------------------------- |
| `std::any` 作为值类型 | 无需为每种数据类型定义序列化，直接存任意 C++ 对象                   |
| `std::mutex` 保护     | Keep-Alive 多路复用下，同一 Session 可能被多个 IO 线程并发访问      |
| `id_` 不加锁          | 构造后不可变，天然线程安全                                          |
| `dirty_` 标志         | 只有被写过数据的 Session 才需要刷新 Cookie，减少不必要的 Set-Cookie |
| `lastAccess_` 时间戳  | 用于过期判断和懒 GC                                                 |

`get<T>()` 方法通过模板实现类型安全的取值：

```cpp
template <typename T>
std::optional<T> get(const std::string& key) const
{
    std::lock_guard<std::mutex> lk(mutex_);
    auto it = data_.find(key);
    if (it == data_.end())
    {
        return std::nullopt;
    }
    try
    {
        return std::any_cast<T>(it->second);
    }
    catch (const std::bad_any_cast&)
    {
        return std::nullopt;  // 类型不匹配也返回 nullopt，不抛异常
    }
}
```

类型不匹配时返回 `nullopt` 而非抛异常——这是防御性设计。Session 数据可能因代码版本迭代而改变类型，不应因此导致整个请求崩溃。

### 3.3 SessionManager：ID 生成与懒 GC

`SessionManager` 管理所有 Session 的生命周期——创建、查找、销毁和垃圾回收。

#### Session ID 生成：128 位随机数

Session ID 的安全性至关重要——如果攻击者能猜到或暴力破解 ID，就能劫持用户会话。

hical 使用 128 位随机数作为 Session ID（`Session.cpp:99-112`）：

```cpp
std::string SessionManager::generateId()
{
    // 使用 thread_local 随机引擎，避免加锁
    thread_local std::mt19937_64 rng(std::random_device {}());
    std::uniform_int_distribution<uint64_t> dist;

    // 生成两个 64 位随机数拼成 128 位 ID
    uint64_t hi = dist(rng);
    uint64_t lo = dist(rng);

    std::ostringstream oss;
    oss << std::hex << std::setfill('0')
        << std::setw(16) << hi << std::setw(16) << lo;
    return oss.str();
}
```

设计要点：

| 细节                      | 说明                                                |
| ------------------------- | --------------------------------------------------- |
| `thread_local` 引擎       | 每个线程独立的随机引擎，无需加锁，高并发下无竞争    |
| `std::random_device` 播种 | 使用操作系统熵源初始化，确保不可预测                |
| 128 位长度                | 2^128 种可能，暴力破解不可行                        |
| 碰撞保护                  | `while (store_.count(id))` 循环检查（极低概率触发） |

生成的 ID 形如：`a1b2c3d4e5f60718091a2b3c4d5e6f70`——32 个十六进制字符。

#### 懒 GC：搭便车的垃圾回收

Session 会过期，但 hical 不启动单独的定时器线程做清理。而是采用**懒 GC（Lazy Garbage Collection）**策略：在 `create()` 时顺便检查是否需要清理。

```
  create() 被调用
        │
        ▼
  距上次 GC ≥ gcInterval？ ──否──► 直接创建新 Session
        │
        是
        │
        ▼
  遍历 store_：
  检查每个 Session 的 lastAccess_
        │
        ▼
  elapsed ≥ maxAge？ ──是──► erase
        │
        否
        │
        ▼
  更新 lastGc_ 时间戳
        │
        ▼
  生成新 Session ID
        │
        ▼
  存入 store_，返回
```

核心实现（`Session.cpp:29-66`）：

```cpp
std::shared_ptr<Session> SessionManager::create()
{
    std::lock_guard<std::mutex> lock(mutex_);

    // 懒 GC：每隔 gcInterval 秒在 create() 时顺带清理过期 Session
    if (opts_.gcInterval > 0)
    {
        auto now = std::chrono::steady_clock::now();
        auto sinceGcMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - lastGc_).count();
        if (sinceGcMs >= static_cast<long long>(opts_.gcInterval) * 1000LL)
        {
            lastGc_ = now;
            for (auto it = store_.begin(); it != store_.end();)
            {
                auto elapsedMs =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        now - it->second->lastAccess()).count();
                if (opts_.maxAge > 0
                    && elapsedMs >= static_cast<long long>(opts_.maxAge) * 1000LL)
                {
                    it = store_.erase(it);
                }
                else
                {
                    ++it;
                }
            }
        }
    }

    auto id = generateId();
    while (store_.count(id))  // 极低概率碰撞保护
    {
        id = generateId();
    }
    auto session = std::make_shared<Session>(id);
    store_[id] = session;
    return session;
}
```

**为什么不用定时器？** 懒 GC 的优势在于简单和零额外开销——不需要额外线程、不需要协程定时器。代价是过期 Session 不会被立即清理，但 `find()` 方法也会检查过期（`Session.cpp:8-27`），所以过期 Session 不会被"复活"。

`SessionOptions` 控制 GC 行为：

| 选项         | 默认值            | 说明                  |
| ------------ | ----------------- | --------------------- |
| `cookieName` | `"HICAL_SESSION"` | Cookie 名称           |
| `maxAge`     | `3600`（1小时）   | Session 有效期（秒）  |
| `httpOnly`   | `true`            | 防 XSS                |
| `secure`     | `false`           | 生产环境应设为 `true` |
| `sameSite`   | `"Lax"`           | 防 CSRF               |
| `path`       | `"/"`             | Cookie 作用路径       |
| `gcInterval` | `300`（5分钟）    | 懒 GC 触发间隔        |

### 3.4 Session 中间件：洋葱模型的完美应用

Session 管理天然适合中间件模式——在请求进入前加载 Session，在响应返回后写回 Cookie。这正是第三篇中介绍的**洋葱模型**的完美应用场景。

`makeSessionMiddleware` 工厂函数（`Session.h:270-311`）：

```cpp
inline MiddlewareHandler makeSessionMiddleware(
    std::shared_ptr<SessionManager> manager)
{
    return [manager](HttpRequest& req, MiddlewareNext next)
        -> Awaitable<HttpResponse>
    {
        const auto& opts = manager->options();

        // 1. 从 Cookie 中读取 Session ID
        auto sessionId = req.cookie(opts.cookieName);
        std::shared_ptr<Session> session;

        if (!sessionId.empty())
        {
            session = manager->find(sessionId);
        }
        if (!session)
        {
            // 新建 Session
            session = manager->create();
        }
        session->touch();

        // 2. 注入到请求 attribute
        req.setAttribute(SessionManager::hSessionKey, session);

        // 3. 执行后续中间件/路由
        auto res = co_await next(req);

        // 4. 如果 Session 被写过数据（dirty），刷新 Cookie
        if (session->isDirty() || sessionId != session->id())
        {
            CookieOptions cookieOpts;
            cookieOpts.maxAge = opts.maxAge;
            cookieOpts.httpOnly = opts.httpOnly;
            cookieOpts.secure = opts.secure;
            cookieOpts.sameSite = opts.sameSite;
            cookieOpts.path = opts.path;
            res.setCookie(opts.cookieName, session->id(), cookieOpts);
        }

        co_return res;
    };
}
```

中间件的洋葱模型流转：

```
请求进入
    │
    ▼
┌─────────────────────────────────────────────┐
│ Session 中间件 (before)                      │
│   ① 读 Cookie → find Session                │
│   ② 未找到 → create 新 Session              │
│   ③ touch() 刷新时间                        │
│   ④ setAttribute 注入请求                    │
│   │                                         │
│   ▼                                         │
│ ┌─────────────────────────────────────────┐ │
│ │ 其他中间件 / 路由 Handler               │ │
│ │   session->set("user", "alice")         │ │
│ │   → 标记 dirty_ = true                 │ │
│ └─────────────────────────────────────────┘ │
│   │                                         │
│   ▼                                         │
│ Session 中间件 (after)                       │
│   ⑤ 检查 isDirty() 或 ID 变化              │
│   ⑥ 写 Set-Cookie 到响应                   │
└─────────────────────────────────────────────┘
    │
    ▼
响应返回
```

注意步骤 ⑤ 的条件：`isDirty() || sessionId != session->id()`。两种情况需要写 Cookie：
- **Session 被修改**（dirty）：用户登录/写入了数据
- **Session ID 变化**：旧 Session 已过期，创建了新 Session，需要下发新 ID

### 3.5 登录/登出完整示例

结合以上所有机制，实现用户登录和登出：

```cpp
#include <hical/HttpServer.h>
#include <hical/Session.h>

int main()
{
    auto sessionMgr = std::make_shared<hical::SessionManager>();

    hical::HttpServer<hical::AsioBackend> server;
    server.use(hical::makeSessionMiddleware(sessionMgr));

    // 登录
    server.router().post("/login",
        [](const hical::HttpRequest& req) -> hical::HttpResponse
    {
        auto json = req.jsonBody();
        auto username = json.at("username").as_string();

        // 获取 Session（由中间件注入）
        auto sessionOpt = req.getAttribute(hical::SessionManager::hSessionKey);
        if (!sessionOpt) return hical::HttpResponse::serverError();

        auto session = std::any_cast<std::shared_ptr<hical::Session>>(*sessionOpt);
        session->set("user", std::string(username));  // 标记 dirty

        return hical::HttpResponse::json({{"status", "ok"}});
    });

    // 获取当前用户
    server.router().get("/me",
        [](const hical::HttpRequest& req) -> hical::HttpResponse
    {
        auto sessionOpt = req.getAttribute(hical::SessionManager::hSessionKey);
        if (!sessionOpt) return hical::HttpResponse::badRequest("No session");

        auto session = std::any_cast<std::shared_ptr<hical::Session>>(*sessionOpt);
        auto user = session->get<std::string>("user");
        if (!user) return hical::HttpResponse::badRequest("Not logged in");

        return hical::HttpResponse::json({{"user", *user}});
    });

    // 登出
    server.router().post("/logout",
        [&sessionMgr](const hical::HttpRequest& req) -> hical::HttpResponse
    {
        auto sessionOpt = req.getAttribute(hical::SessionManager::hSessionKey);
        if (sessionOpt)
        {
            auto session = std::any_cast<std::shared_ptr<hical::Session>>(*sessionOpt);
            sessionMgr->destroy(session->id());
        }

        // 清除客户端 Cookie（设置 maxAge=0）
        hical::HttpResponse res = hical::HttpResponse::json({{"status", "logged out"}});
        res.setCookie("HICAL_SESSION", "", {.maxAge = 0});
        return res;
    });

    server.listen(8080);
    server.run();
}
```

---

## 4. 静态文件服务：安全地托管资源

### 4.1 为什么需要框架内置静态文件服务？

大多数生产环境会用 Nginx/CDN 托管静态文件，但框架内置静态文件服务在以下场景仍然必要：

| 场景           | Nginx 反代         | 框架内置                         |
| -------------- | ------------------ | -------------------------------- |
| **开发调试**   | 需要额外配置 Nginx | 零配置，`serveStatic()` 一行搞定 |
| **单体部署**   | 多一个运维组件     | 单进程搞定                       |
| **嵌入式/IoT** | 资源有限难以安装   | 框架自带                         |
| **管理后台**   | 过度设计           | 恰到好处                         |
| **生产高并发** | ✅ 推荐             | ⚠️ 非最优（无 sendfile）          |

hical 的 `serveStatic()` 工厂函数返回一个 `SyncRouteHandler`，可直接注册到路由：

```cpp
// 将 /static/... 映射到 ./public 目录
server.router().get("/static/{path}", hical::serveStatic("./public", "/static/"));
```

### 4.2 serveStatic 工厂：初始化与闭包捕获

`serveStatic()` 采用**工厂模式**——在初始化阶段做一次性工作（规范化根目录），然后返回一个闭包处理每个请求。

初始化阶段（`StaticFiles.h:134-150`）：

```cpp
inline std::function<HttpResponse(const HttpRequest&)> serveStatic(
    const std::string& rootDir,
    const std::string& urlPrefix,
    std::uintmax_t maxFileSize = 64ULL * 1024 * 1024)
{
    namespace fs = std::filesystem;

    // 提前规范化根目录路径（只做一次）
    std::error_code ec;
    fs::path root = fs::canonical(rootDir, ec);
    if (ec)
    {
        // 根目录不存在时，每次请求都返回 404
        return [rootDir](const HttpRequest&) -> HttpResponse
        {
            return HttpResponse::notFound();
        };
    }

    return [root, urlPrefix, maxFileSize](const HttpRequest& req) -> HttpResponse
    {
        // ... 请求处理逻辑
    };
}
```

为什么提前 `canonical`？`std::filesystem::canonical()` 会解析符号链接并消除 `.` 和 `..`，得到一个绝对路径。这个操作只需要做一次——根目录在服务器运行期间不会变。

### 4.3 路径遍历防护：不只是字符串前缀

**路径遍历攻击（Path Traversal）** 是静态文件服务最常见的安全漏洞。攻击者通过 `../` 序列跳出根目录：

```
请求: GET /static/../../etc/passwd
期望: ./public/../../etc/passwd → /etc/passwd  ← 泄露系统文件！
```

很多框架用字符串前缀检查来防护，但这种方式有边界问题：

| 方法           | 代码                       | 问题                                       |
| -------------- | -------------------------- | ------------------------------------------ |
| 字符串前缀     | `target.starts_with(root)` | `/pub` 是 `/public` 的前缀，但不在目录内！ |
| **逐段迭代器** | 逐个路径分量比较           | ✅ 准确，不受部分匹配影响                   |

hical 采用逐段迭代器方案（`StaticFiles.h:83-97`）：

```cpp
inline bool isSafePath(const std::filesystem::path& root,
                       const std::filesystem::path& target)
{
    // 逐段迭代器比较：root 的每个路径分量必须是 target 的前缀
    // 比字符串前缀比对更可靠，不受 /pub vs /public 等 edge case 影响
    auto rootIt = root.begin();
    auto targetIt = target.begin();
    for (; rootIt != root.end(); ++rootIt, ++targetIt)
    {
        if (targetIt == target.end() || *rootIt != *targetIt)
        {
            return false;
        }
    }
    return true;
}
```

攻击防御流程：

```
请求: GET /static/../../etc/passwd
         │
         ▼
拼接路径: root / "../../etc/passwd"
         │
         ▼
canonical(): 解析为 /etc/passwd
         │
         ▼
isSafePath(root="/srv/public", target="/etc/passwd")
         │
         ▼
逐段比较:
  root[0]="/"     == target[0]="/"      ✓
  root[1]="srv"   != target[1]="etc"    ✗ → return false
         │
         ▼
返回 403 Forbidden
```

注意 hical 使用的是**双重防护**：`canonical()` 解析符号链接 + `isSafePath()` 验证路径归属。即使攻击者构造了绕过 `canonical` 的路径（理论上不可能），`isSafePath` 仍然能拦截。

### 4.4 MIME 类型推断与目录处理

hical 内置了 26 种常见 MIME 类型映射，覆盖 Web 开发中最常用的文件格式：

```cpp
static const std::unordered_map<std::string, std::string> table = {
    {".html", "text/html; charset=utf-8"},
    {".css",  "text/css; charset=utf-8"},
    {".js",   "application/javascript; charset=utf-8"},
    {".json", "application/json; charset=utf-8"},
    {".png",  "image/png"},
    {".jpg",  "image/jpeg"},
    {".svg",  "image/svg+xml"},
    {".woff2","font/woff2"},
    // ... 共 26 种
};
// 未知扩展名 → "application/octet-stream"
```

当请求路径指向目录时，hical 自动尝试 `index.html`：

```cpp
if (fs::is_directory(target, ec2))
{
    target /= "index.html";
    // 追加后重新 canonical 解析
    // 防止 index.html 是指向 root 外的符号链接
    target = fs::canonical(target, ec2);
    if (ec2 || !detail::isSafePath(root, target))
    {
        return HttpResponse::notFound();
    }
}
```

关键细节：追加 `index.html` 后**重新执行 canonical + isSafePath**。原因是 `index.html` 本身可能是一个符号链接，指向根目录之外的文件。如果不重新验证，攻击者可以在目录中创建一个名为 `index.html` 的符号链接来逃逸。

### 4.5 ETag 缓存验证

ETag（Entity Tag）是 HTTP 缓存验证机制：服务器为每个文件生成一个唯一标识符，客户端在后续请求中通过 `If-None-Match` 头发送缓存的 ETag。如果文件未变，服务器返回 304 Not Modified（无 body），节省带宽。

hical 的 ETag 生成策略（`StaticFiles.h:105-109`）：

```cpp
inline std::string makeEtag(std::uintmax_t fileSize,
                            std::filesystem::file_time_type lastWrite)
{
    auto ns = lastWrite.time_since_epoch().count();
    return "\"" + std::to_string(fileSize) + "-" + std::to_string(ns) + "\"";
}
```

用 **文件大小 + 最后修改时间** 组合作为 ETag。这不是加密哈希（不像 MD5/SHA），但对于静态文件服务足够——文件修改必然改变这两个值中的至少一个。

缓存交互流程：

```
首次请求:
  客户端 → GET /static/app.js
  服务器 → 200 OK + ETag: "12345-1712345678"
                    Body: (文件内容)

二次请求:
  客户端 → GET /static/app.js
            If-None-Match: "12345-1712345678"
  服务器 → ETag 匹配？
           ├─ 是 → 304 Not Modified（无 body，节省带宽）
           └─ 否 → 200 OK + 新 ETag + 新内容
```

### 4.6 大文件限制与 Race Condition 处理

**大文件限制**：默认 64MB 上限（`maxFileSize` 参数），防止请求超大文件导致 `bad_alloc` 崩溃：

```cpp
if (fileSize > maxFileSize)
{
    HttpResponse res;
    res.setStatus(HttpStatusCode::hPayloadTooLarge);
    res.setBody("413 File Too Large");
    return res;
}
```

**stat/read Race Condition**：`file_size()` 和 `read()` 之间文件可能被外部修改（截短或删除）。hical 的处理策略：

```cpp
std::string content(fileSize, '\0');
ifs.read(content.data(), static_cast<std::streamsize>(fileSize));
auto bytesRead = ifs.gcount();
// 文件在 stat 和 read 之间被截短时，
// 截断到实际读取长度，拒绝返回零填充内容
if (bytesRead <= 0)
{
    return HttpResponse::serverError();
}
if (static_cast<std::uintmax_t>(bytesRead) < fileSize)
{
    content.resize(static_cast<std::size_t>(bytesRead));
}
```

错误码一览：

| HTTP 状态码               | 触发条件                    |
| ------------------------- | --------------------------- |
| 200 OK                    | 正常返回文件                |
| 304 Not Modified          | ETag 匹配（缓存有效）       |
| 403 Forbidden             | 路径遍历攻击被拦截          |
| 404 Not Found             | 文件不存在 / canonical 失败 |
| 413 Payload Too Large     | 文件超过 maxFileSize        |
| 500 Internal Server Error | file_size 失败 / read 失败  |

---

## 5. Multipart 文件上传：解析 RFC 7578

### 5.1 为什么不能用 JSON 上传文件？

JSON 只支持文本数据——二进制文件必须 Base64 编码，体积膨胀 33%。HTTP 提供了三种主要的请求体编码方式：

| 编码方式      | Content-Type               | 适用场景            | 二进制支持     |
| ------------- | -------------------------- | ------------------- | -------------- |
| JSON          | `application/json`         | API 数据交换        | ❌（需 Base64） |
| 原始字节      | `application/octet-stream` | 单文件上传          | ✅ 但无元数据   |
| **Multipart** | `multipart/form-data`      | **表单+多文件上传** | ✅ 原生支持     |

`multipart/form-data`（RFC 7578）是 HTML 表单文件上传的标准格式，支持在一次请求中发送多个字段（文本+文件）。

### 5.2 协议结构

一个 multipart 请求体的结构：

```
Content-Type: multipart/form-data; boundary=----WebKitFormBoundary7MA4YWxk
                                            ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
                                            boundary：各 Part 的分隔符

请求体:
------WebKitFormBoundary7MA4YWxk\r\n          ← delimiter（"--" + boundary）
Content-Disposition: form-data; name="username"\r\n
\r\n                                          ← 空行分隔头部和数据
alice                                         ← Part 数据（文本字段）
------WebKitFormBoundary7MA4YWxk\r\n          ← 下一个 Part
Content-Disposition: form-data; name="avatar"; filename="photo.jpg"\r\n
Content-Type: image/jpeg\r\n
\r\n
<二进制文件数据>                                ← Part 数据（文件）
------WebKitFormBoundary7MA4YWxk--\r\n        ← end delimiter（"--" + boundary + "--"）
```

### 5.3 MultipartPart 与三个静态 API

hical 将每个 Part 解析为 `MultipartPart` 结构体（`Multipart.h:19-35`）：

```cpp
struct MultipartPart
{
    std::unordered_map<std::string, std::string> headers;  // Part 头部
    std::string name;          // form 字段名
    std::string filename;      // 上传文件名（无则为空）
    std::string contentType;   // Part 的 Content-Type
    std::string data;          // Part 数据

    bool isFile() const { return !filename.empty(); }
};
```

`MultipartParser` 提供三个静态方法，覆盖不同使用场景：

```cpp
// 完整解析：获取所有 Part
auto parts = MultipartParser::parse(req);

// 快捷方法：直接获取指定文件
auto avatar = MultipartParser::getFile(req, "avatar");

// 快捷方法：直接获取指定文本字段
auto username = MultipartParser::getField(req, "username");
```

使用示例：

```cpp
server.router().post("/upload",
    [](const hical::HttpRequest& req) -> hical::HttpResponse
{
    auto file = hical::MultipartParser::getFile(req, "avatar");
    if (!file)
    {
        return hical::HttpResponse::badRequest("No file uploaded");
    }

    // file->filename  → "photo.jpg"
    // file->data      → 文件二进制内容
    // file->contentType → "image/jpeg"

    // 保存到磁盘
    std::ofstream ofs("./uploads/" + file->filename, std::ios::binary);
    ofs.write(file->data.data(), file->data.size());

    return hical::HttpResponse::json({
        {"filename", file->filename},
        {"size", file->data.size()}
    });
});
```

### 5.4 四步解析流程

`MultipartParser::parse()` 的解析流程（`Multipart.cpp:160-262`）可以分为四步：

```
步骤 1: 提取 boundary
  Content-Type: multipart/form-data; boundary=----WebKit...
                                              ↓
  extractBoundary() → "----WebKit..."

步骤 2: 定位第一个 delimiter
  body 中搜索 "--" + boundary
  跳过 delimiter 后的 CRLF

步骤 3: 循环解析每个 Part
  ┌────────────────────────────────────┐
  │ 搜索下一个 delimiter               │
  │         ↓                          │
  │ partData = 当前位置到下一 delimiter │
  │         ↓                          │
  │ 找 "\r\n\r\n" 分割头部和数据       │
  │         ↓                          │
  │ parsePartHeaders() 解析头部        │
  │         ↓                          │
  │ parts.push_back(part)              │
  │         ↓                          │
  │ 检查 Part 数量上限（256）          │
  │         ↓                          │
  │ 下一 delimiter 后是 "--"？          │
  │   ├─ 是 → 结束                     │
  │   └─ 否 → 继续循环                 │
  └────────────────────────────────────┘

步骤 4: 返回 parts 列表
```

`extractBoundary` 实现（`Multipart.cpp:54-85`）：

```cpp
std::string MultipartParser::extractBoundary(const std::string& contentType)
{
    auto pos = contentType.find("boundary=");
    if (pos == std::string::npos)
    {
        return "";
    }
    pos += 9;  // 跳过 "boundary="
    std::string_view rest(contentType.c_str() + pos, contentType.size() - pos);

    rest = trim(rest);
    rest = unquote(rest);  // 处理引号包裹的 boundary

    auto semi = rest.find(';');
    if (semi != std::string_view::npos)
    {
        rest = rest.substr(0, semi);
    }

    auto boundary = std::string(trim(rest));

    // RFC 2046: boundary 最长 70 字符
    if (boundary.size() > 70)
    {
        return "";
    }

    return boundary;
}
```

### 5.5 安全防护：多层纵深防御

文件上传是 Web 应用中最危险的操作之一。hical 在多个层次提供防护：

| 防护层     | 机制              | 说明                                                  |
| ---------- | ----------------- | ----------------------------------------------------- |
| **连接层** | `maxBodySize`     | TcpServer 限制请求体总大小（默认 10MB），超出直接断开 |
| **协议层** | boundary 长度检查 | RFC 2046 要求 ≤70 字符，拒绝超长 boundary             |
| **解析层** | **256 Part 上限** | 防止在 maxBodySize 内构造大量小 Part 消耗 CPU         |
| **应用层** | 用户自行校验      | 检查文件名、大小、类型，防止上传恶意文件              |

256 Part 上限的实现（`Multipart.cpp:236-240`）：

```cpp
// Part 数量上限：防止在 maxBodySize 内构造大量小 Part 消耗 CPU/内存
static constexpr std::size_t hMaxMultipartParts = 256;
if (parts.size() >= hMaxMultipartParts)
{
    return std::nullopt;
}
```

**为什么 256 够用？** 常见的文件上传表单通常只有几个到几十个字段。256 对正常使用绰绰有余，但能有效阻止 DoS 攻击——攻击者可以构造一个合法大小的请求体，但内含上千个微小 Part，让解析器消耗大量 CPU 和内存。

---

## 6. 综合实战：带登录的文件管理服务

现在让我们把四个模块组装起来，构建一个完整的小型文件管理服务：

```cpp
#include <hical/HttpServer.h>
#include <hical/Session.h>
#include <hical/StaticFiles.h>
#include <hical/Multipart.h>

int main()
{
    // ========== 配置 ==========
    hical::SessionOptions sessionOpts;
    sessionOpts.maxAge = 7200;  // 2 小时
    auto sessionMgr = std::make_shared<hical::SessionManager>(sessionOpts);

    hical::HttpServer<hical::AsioBackend> server;

    // ========== 中间件 ==========
    server.use(hical::makeSessionMiddleware(sessionMgr));

    // ========== 登录 ==========
    server.router().post("/api/login",
        [](const hical::HttpRequest& req) -> hical::HttpResponse
    {
        auto json = req.jsonBody();
        auto user = std::string(json.at("username").as_string());
        auto pass = std::string(json.at("password").as_string());

        // 简化示例：实际应查数据库 + bcrypt 校验
        if (user != "admin" || pass != "123456")
        {
            return hical::HttpResponse::badRequest("Invalid credentials");
        }

        auto sessionOpt = req.getAttribute(hical::SessionManager::hSessionKey);
        auto session = std::any_cast<std::shared_ptr<hical::Session>>(*sessionOpt);
        session->set("user", user);

        return hical::HttpResponse::json({{"status", "ok"}, {"user", user}});
    });

    // ========== 文件列表 ==========
    server.router().get("/api/files",
        [](const hical::HttpRequest& req) -> hical::HttpResponse
    {
        // 检查登录
        auto sessionOpt = req.getAttribute(hical::SessionManager::hSessionKey);
        auto session = std::any_cast<std::shared_ptr<hical::Session>>(*sessionOpt);
        if (!session->has("user"))
        {
            return hical::HttpResponse::badRequest("Please login first");
        }

        // 列出 uploads 目录
        namespace fs = std::filesystem;
        boost::json::array files;
        for (const auto& entry : fs::directory_iterator("./uploads"))
        {
            if (entry.is_regular_file())
            {
                files.push_back({
                    {"name", entry.path().filename().string()},
                    {"size", entry.file_size()}
                });
            }
        }
        return hical::HttpResponse::json({{"files", files}});
    });

    // ========== 文件上传 ==========
    server.router().post("/api/upload",
        [](const hical::HttpRequest& req) -> hical::HttpResponse
    {
        // 检查登录
        auto sessionOpt = req.getAttribute(hical::SessionManager::hSessionKey);
        auto session = std::any_cast<std::shared_ptr<hical::Session>>(*sessionOpt);
        if (!session->has("user"))
        {
            return hical::HttpResponse::badRequest("Please login first");
        }

        auto file = hical::MultipartParser::getFile(req, "file");
        if (!file)
        {
            return hical::HttpResponse::badRequest("No file in request");
        }

        // 保存文件
        std::ofstream ofs("./uploads/" + file->filename, std::ios::binary);
        ofs.write(file->data.data(), file->data.size());

        return hical::HttpResponse::json({
            {"filename", file->filename},
            {"size", file->data.size()}
        });
    });

    // ========== 静态文件（前端页面） ==========
    server.router().get("/static/{path}",
        hical::serveStatic("./public", "/static/"));

    // ========== 登出 ==========
    server.router().post("/api/logout",
        [&sessionMgr](const hical::HttpRequest& req) -> hical::HttpResponse
    {
        auto sessionOpt = req.getAttribute(hical::SessionManager::hSessionKey);
        if (sessionOpt)
        {
            auto session = std::any_cast<std::shared_ptr<hical::Session>>(*sessionOpt);
            sessionMgr->destroy(session->id());
        }

        hical::HttpResponse res = hical::HttpResponse::json({{"status", "logged out"}});
        res.setCookie("HICAL_SESSION", "", {.maxAge = 0});
        return res;
    });

    server.listen(8080);
    server.run();
}
```

完整的请求流转：

```
用户访问 /static/index.html
    │
    ▼
Session 中间件 → 创建新 Session → Cookie: HICAL_SESSION=abc123
    │
    ▼
静态文件路由 → 读取 ./public/index.html → 200 OK
    │
    ▼
用户提交登录表单 POST /api/login
    │
    ▼
Session 中间件 → Cookie 中找到 abc123 → 加载 Session
    │
    ▼
路由 Handler → session.set("user", "admin") → dirty!
    │
    ▼
Session 中间件 (after) → 刷新 Set-Cookie
    │
    ▼
用户上传文件 POST /api/upload (multipart/form-data)
    │
    ▼
Session 中间件 → 验证登录态
    │
    ▼
路由 Handler → MultipartParser::getFile("file") → 保存到磁盘
    │
    ▼
200 OK {"filename": "report.pdf", "size": 102400}
```

---

## 7. 全系列总结

经过五篇文章，我们从设计理念一路走到 Web 应用的四大基础模块，完整地剖析了 hical 框架的每一层。

### 知识图谱

```
从零构建现代 C++ Web 服务器
│
├── 第一篇：设计理念与架构总览
│   ├── 为什么用 C++ 写 Web 框架
│   ├── 两层架构：core（抽象）+ asio（实现）
│   ├── C++20 Concepts 后端抽象
│   └── 线程模型：1 Thread : 1 io_context
│
├── 第二篇：协程异步与 PMR 内存池
│   ├── 从回调地狱到 co_await
│   ├── Awaitable<T> 协程基石
│   ├── PMR 三层内存架构
│   └── PmrBuffer 零拷贝缓冲区
│
├── 第三篇：路由、中间件与 SSL
│   ├── 双策略路由（哈希 O(1) + 参数线性）
│   ├── 洋葱模型中间件管道
│   ├── 模板化 SSL（编译期零开销）
│   └── WebSocket 集成
│
├── 第四篇：实战案例与性能调优
│   ├── RESTful API 完整案例
│   ├── WebSocket 实时通信案例
│   ├── C++26 反射宏系统
│   ├── 性能调优实战
│   └── 安全加固清单
│
└── 第五篇：Cookie、Session、静态文件与文件上传（本文）
    ├── Cookie 惰性解析与 RFC 6265 编码
    ├── Session 中间件与懒 GC
    ├── 静态文件 ETag 缓存与路径遍历防护
    └── Multipart 文件上传与 DoS 防护
```

### 核心设计决策完整表

| #   | 决策           | 选择                          | 核心理由                       |
| --- | -------------- | ----------------------------- | ------------------------------ |
| 1   | 协程模型       | `asio::awaitable<T>`          | 与 Boost.Asio 生态无缝集成     |
| 2   | HTTP 解析      | Boost.Beast                   | 成熟、标准、零额外依赖         |
| 3   | 内存管理       | C++17 PMR 三层池              | 全局→线程→请求，逐层减少竞争   |
| 4   | SSL 实现       | 模板化 `if constexpr`         | 不用 SSL 时零开销              |
| 5   | 后端抽象       | C++20 Concepts                | 编译期约束，零运行时开销       |
| 6   | 路由查找       | 哈希表 + 线性匹配             | 静态 O(1)，参数路由灵活        |
| 7   | 中间件         | 洋葱模型 + 预构建链           | 直觉清晰，运行时零额外分配     |
| 8   | 反射           | C++26 双路线                  | 面向未来，C++20 也能用         |
| 9   | 线程模型       | 1:1 (Thread:io_context)       | 无锁连接处理，round-robin 分发 |
| 10  | Cookie 解析    | 惰性解析 + `mutable optional` | 不用不解析，const 友好         |
| 11  | Cookie 编码    | RFC 6265 百分号编码           | 标准合规 + CRLF 注入防护       |
| 12  | Session 存储   | `std::any` + `mutex`          | 任意类型，线程安全             |
| 13  | Session ID     | 128 位 `thread_local` 随机数  | 不可预测 + 无锁生成            |
| 14  | Session GC     | 懒 GC（create 时触发）        | 零额外线程/定时器开销          |
| 15  | 路径安全       | `canonical()` + 逐段迭代器    | 双重防护，无边界问题           |
| 16  | 文件缓存       | ETag（size + mtime）          | 轻量有效，无哈希计算           |
| 17  | Multipart 防护 | 256 Part 上限                 | 阻止小 Part DoS 攻击           |

### 写在最后

五篇文章，从"为什么用 C++"到"完整的文件管理服务"，我们走过了一个现代 C++ Web 框架从设计到实现的完整旅程。

hical 的核心理念始终如一：**用现代 C++ 特性（协程、Concepts、PMR、反射）消除传统 C++ 网络编程的痛点，让 C++ Web 开发既高性能又不失优雅**。

> 如果这个系列对你有帮助，欢迎 Star 支持！或者提一个 Issue 告诉我们你的想法。
> 
> 全系列完结。感谢阅读。
