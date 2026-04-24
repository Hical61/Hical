# 2026 年 C++ Web 框架横评：Hical vs Drogon vs Crow vs Oat++

> 如果你在 2026 年启动一个需要 C++ Web 服务的项目，面前摆着 Drogon、Crow、Oat++ 和 Hical 四个选择。该怎么选？本文从架构设计、异步模型、内存管理、功能完整度、开发体验五个维度做一次横向对比，帮你快速定位最适合的框架。

---

## 一句话概括

| 框架       | 一句话定位                                     |
| ---------- | ---------------------------------------------- |
| **Drogon** | 久经考验的高性能全栈框架，TechEmpower 榜单常客 |
| **Crow**   | 极简轻量的微框架，Express.js 风格，上手最快    |
| **Oat++**  | 零依赖、内置 Swagger 的 API 框架，嵌入式友好   |
| **Hical**  | 围绕 C++26 反射和 PMR 内存池设计的现代框架     |

---

## 核心对比表

|                      | Hical                     | Drogon                  | Crow          | Oat++                     |
| -------------------- | ------------------------- | ----------------------- | ------------- | ------------------------- |
| **C++ 标准**         | C++20（C++26 就绪）       | C++17 / C++20           | C++14 / C++17 | C++11+                    |
| **异步模型**         | 协程（`co_await` 全链路） | 回调 + 协程混合         | 回调          | 自研异步 API              |
| **内存管理**         | PMR 三层内存池            | 默认分配器              | 默认分配器    | 默认分配器                |
| **HTTP 解析**        | Boost.Beast               | 自研（Trantor）         | 自研          | 自研                      |
| **SSL/TLS**          | 编译期模板分支            | 运行时分支              | 运行时分支    | 运行时分支                |
| **路由**             | 哈希表 O(1) + 参数线性    | 基数树                  | 前缀树        | Controller 映射           |
| **中间件**           | 洋葱模型（协程链）        | Filter 链               | 基础          | Interceptor               |
| **WebSocket**        | 内置（协程式）            | 内置                    | 内置          | 内置                      |
| **Cookie / Session** | 内置（RFC 6265）          | 内置                    | 有限          | 有限                      |
| **文件上传**         | 内置（DoS 防护）          | 内置                    | 需手动        | 内置                      |
| **静态文件**         | 内置（ETag/304）          | 内置                    | 需手动        | 有限                      |
| **ORM**              | 无                        | 内置（PG/MySQL/SQLite） | 无            | 模块化（PG/SQLite/Mongo） |
| **OpenAPI/Swagger**  | 无                        | 第三方                  | 无            | 内置                      |
| **HTTP/2**           | 不支持                    | 支持                    | 不支持        | 不支持                    |
| **反射/自动序列化**  | C++26 双轨（原生 + 宏）   | 无                      | 无            | 宏 DTO 系统               |
| **外部依赖**         | Boost + OpenSSL           | Trantor + jsoncpp + ... | Asio          | 零依赖                    |
| **License**          | MIT                       | MIT                     | BSD-3         | Apache-2.0                |

---

## 深度对比

### 1. 异步模型

**这是选框架时最该关注的维度**，因为它决定了你写业务逻辑的方式。

**Hical**：全链路协程。从中间件到路由处理器，都可以用 `co_await`：

```cpp
server.use([](HttpRequest& req, MiddlewareNext next) -> Awaitable<HttpResponse> {
    auto start = std::chrono::steady_clock::now();
    auto res = co_await next(req);  // 协程挂起，不阻塞线程
    auto elapsed = std::chrono::steady_clock::now() - start;
    co_return res;
});
```

**Drogon**：回调和协程混合。早期 API 基于回调，后来加入了 C++20 协程支持，但很多示例和文档仍以回调为主：

```cpp
// 回调风格
app().registerHandler("/api/status",
    [](const HttpRequestPtr& req,
       std::function<void(const HttpResponsePtr&)>&& callback) {
        auto resp = HttpResponse::newHttpJsonResponse(json);
        callback(resp);
    });

// 协程风格（较新）
app().registerHandler("/api/status",
    [](HttpRequestPtr req) -> Task<HttpResponsePtr> {
        co_return HttpResponse::newHttpJsonResponse(json);
    });
```

**Crow**：纯回调/同步。API 简洁但不支持协程：

```cpp
CROW_ROUTE(app, "/api/status")([]() {
    return crow::response(200, "OK");
});
```

**Oat++**：自研异步模型。使用自己的协程抽象而非标准 `co_await`：

```cpp
ENDPOINT("GET", "/api/status", getStatus) {
    return createResponse(Status::CODE_200, "OK");
}
```

**结论**：如果你要写大量异步逻辑（数据库查询、RPC 调用、文件 I/O），Hical 的全链路协程体验最一致。Drogon 也支持协程但历史包袱较重。Crow 和 Oat++ 在这方面较弱。

---

### 2. 内存管理

这是 Hical 差异化最大的领域。

**Hical 的 PMR 三层池**：

| 层级   | 作用域 | 分配器类型                         | 特点                           |
| ------ | ------ | ---------------------------------- | ------------------------------ |
| 全局池 | 进程级 | `synchronized_pool_resource`       | 跨线程共享，有锁               |
| 线程池 | 线程级 | `thread_local unsynchronized_pool` | 零锁竞争                       |
| 请求池 | 请求级 | `monotonic_buffer_resource`        | 只分配不释放，请求结束整体回收 |

HTTP 请求处理中的 Buffer、JSON 对象、响应体全部走 PMR，请求结束后请求池整体释放 —— 不需要逐个 `delete`，也不会产生内存碎片。

**其他框架**：Drogon、Crow、Oat++ 都使用标准 `new/delete`。在高并发场景下，全局堆分配器的锁竞争会成为瓶颈。

**这意味着什么？** 对于大多数 CRUD 应用，差别不大。但如果你的场景是：
- 高并发短请求（如游戏服务器 API、IoT 数据采集）
- 内存敏感环境（嵌入式、容器化部署需要控制内存上限）
- 长时间运行不重启（内存碎片会随时间累积）

PMR 带来的收益是实实在在的。

---

### 3. 功能完整度

**Drogon 最全**。它是唯一内置 ORM 和 HTTP/2 的框架，还有 CSP 模板渲染、gzip/brotli 压缩、Redis 客户端等。如果你需要"开箱即用的全家桶"，Drogon 是第一选择。

**Hical 次之**。Cookie、Session、静态文件、文件上传、WebSocket 全部内置，且每个模块都有安全防护（路径遍历保护、DoS 限制、CRLF 注入防护）。缺少 ORM 和 HTTP/2。

**Oat++ 特色鲜明**。零依赖 + 内置 Swagger 文档生成，对 API 开发特别友好。ORM 以模块化形式提供。

**Crow 最精简**。核心只有路由和 JSON，Cookie/Session 有限，静态文件和文件上传需自行实现。

---

### 4. 开发体验

#### 上手难度

| 框架   | 上手难度 | 原因                                              |
| ------ | -------- | ------------------------------------------------- |
| Crow   | 最低     | 类 Express API，几乎零学习曲线                    |
| Hical  | 较低     | API 风格与 Crow 类似，但需了解协程语法            |
| Oat++  | 中等     | 宏定义较多，DTO 系统有学习成本                    |
| Drogon | 中高     | 功能丰富意味着概念多，回调/协程双模式增加选择成本 |

#### 编译速度

Hical 和 Drogon 都依赖 Boost，模板实例化较慢。Crow 依赖 Asio（可用 standalone 版本），编译较快。Oat++ 零依赖，编译最快。

#### 错误信息

C++20 Concepts（Hical 使用）在编译报错时比 SFINAE（传统模板）可读性好得多。你会看到"类型 X 不满足 EventLoopLike 概念"，而不是三屏模板展开错误。

---

### 5. 前瞻性

| 特性           | Hical    | Drogon | Crow   | Oat++  |
| -------------- | -------- | ------ | ------ | ------ |
| C++20 Concepts | 核心使用 | 不使用 | 不使用 | 不使用 |
| C++26 反射     | 双轨就绪 | 不支持 | 不支持 | 不支持 |
| PMR 内存池     | 核心架构 | 不使用 | 不使用 | 不使用 |

Hical 是目前唯一围绕 C++20/26 新特性从零设计的 Web 框架。这既是优势（代码更现代、性能更优），也意味着需要更新的编译器（GCC 14+/Clang 20+/MSVC 2022+）。

---

## 选型建议

### 选 Drogon，如果你需要：
- 生产级全栈框架，功能最全
- 内置 ORM，直接操作数据库
- HTTP/2 支持
- 大社区、多文档、TechEmpower 背书

### 选 Crow，如果你需要：
- 最快上手、最小学习成本
- 轻量级微服务或原型验证
- 不需要协程和高级特性

### 选 Oat++，如果你需要：
- 零外部依赖、极致可移植
- 内置 Swagger/OpenAPI 文档
- 嵌入式或受限环境

### 选 Hical，如果你需要：
- 全链路协程异步（`co_await` 从中间件到路由）
- PMR 内存池的性能优势（高并发、低延迟、内存可控）
- C++26 反射自动序列化/路由注册
- 与现有 C++ 生态（如游戏服务器）零成本集成
- 使用最新 C++ 标准特性的"现代 C++ 标杆"项目

---

## 一个更务实的视角

框架选型不应只看功能列表。问自己三个问题：

1. **团队的 C++ 标准线在哪？** 如果项目还在 C++14/17，Hical 的 C++20 要求可能是门槛。Drogon（C++17）或 Crow（C++14）更保险。

2. **需要 ORM 吗？** 如果需要，Drogon 开箱即用。Hical 和 Crow 需要自己集成第三方 ORM。

3. **性能瓶颈在内存还是 I/O？** 如果是内存（高并发短请求、长时间运行），Hical 的 PMR 优势最大。如果是 I/O（大文件传输、数据库查询），各框架差别不大。

---

## 链接

- **Hical**: [GitHub](https://github.com/Hical61/Hical) | [快速上手](06-quick-rest-api.md)
- **Drogon**: [GitHub](https://github.com/drogonframework/drogon)
- **Crow**: [GitHub](https://github.com/CrowCpp/Crow)
- **Oat++**: [GitHub](https://github.com/oatpp/oatpp)

---

> 本文作者是 Hical 框架的开发者，对比力求客观，但难免有主观偏好。欢迎在评论区指正或补充。
