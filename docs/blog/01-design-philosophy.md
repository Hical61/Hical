# 从零构建现代C++ Web服务器（一）：设计理念与架构总览

> **系列导航**：[第一篇：设计理念](#)（本文） | [第二篇：协程与内存池](02-coroutine-and-memory.md) | [第三篇：路由、中间件与SSL](03-router-middleware-ssl.md) | [第四篇：实战与性能](04-practice-and-performance.md)

## 前置知识

- 熟悉 C++17、C++20 基础语法（模板、智能指针、lambda、协程、Concepts）
- 了解 TCP/IP 和 HTTP 协议基本概念
- 对异步编程模型有初步认知

---

## 目录

- [1. 为什么在 2026 年用 C++ 写 Web 框架？](#1-为什么在-2026-年用-c-写-web-框架)
- [2. 现有方案分析](#2-现有方案分析)
- [3. hical 的设计目标](#3-hical-的设计目标)
- [4. 两层架构设计](#4-两层架构设计)
- [5. C++20 Concepts 做后端抽象](#5-c20-concepts-做后端抽象)
- [6. 线程模型：1 Thread : 1 io_context](#6-线程模型1-thread--1-io_context)
- [7. 全文总结](#7-全文总结)

---

## 1. 为什么在 2026 年用 C++ 写 Web 框架？

当大多数团队选择 Go、Rust 或 Node.js 构建 Web 服务时，用 C++ 写 Web 框架似乎是"逆潮流而行"。但事实是，在特定场景下 C++ 仍然不可替代：

- **极致性能需求**：游戏服务器、实时通信、高频交易等场景对延迟敏感到微秒级别
- **与现有 C++ 生态集成**：当你的业务逻辑、数据处理库本身就是 C++ 时，跨语言调用引入的开销和复杂度不可忽视
- **内存可控**：C++ 没有 GC 暂停，配合内存池可以实现完全可预测的内存行为

更重要的是，C++20/26 带来了一系列改变游戏规则的特性：

| 特性                       | 标准  | 解决的问题                                           |
| -------------------------- | ----- | ---------------------------------------------------- |
| **协程（Coroutines）**     | C++20 | 告别回调地狱，异步代码写起来像同步                   |
| **Concepts**               | C++20 | 编译期类型约束，替代 SFINAE 的可读方案               |
| **std::pmr**               | C++17 | 标准化的多态内存分配器，高效内存池不再需要自己造轮子 |
| **静态反射（Reflection）** | C++26 | 编译期自动发现类型信息，零样板代码的序列化和路由注册 |

这些特性组合在一起，使得"现代 C++ Web 框架"不再是矛盾修辞，而是一个切实可行的工程方向。hical 正是基于 C++26 构建的框架，其性能优势主要体现在三个维度：

- **内存管理**：三级 PMR 内存池使内存分配开销降至 O(1) 复杂度
- **控制流优化**：协程模型避免了回调地狱，使代码逻辑更线性
- **编译时优化**：C++26 反射实现路由自动注册和 DTO 自动序列化，减少运行时开销

---

## 2. 现有方案分析

在开始设计前，我们先审视一下 C++ Web 框架的现有格局：

| 框架            | 特点                   | 优势                 | 不足                              |
| --------------- | ---------------------- | -------------------- | --------------------------------- |
| **Drogon**      | 成熟、功能全面         | ORM、WebSocket、全栈 | 学习曲线陡，抽象较重              |
| **Crow**        | 轻量、Express 风格 API | 上手快，头文件引入   | 维护不活跃，协程支持有限          |
| **Muduo**       | 经典事件驱动库         | 久经考验的网络层     | 非 HTTP 专用，需自行构建上层      |
| **cpp-httplib** | 头文件引入，极简       | 零依赖，开箱即用     | 同步阻塞，性能天花板低            |
| **Boost.Beast** | HTTP/WebSocket 底层库  | 工业级解析器         | 太底层，缺少路由/中间件等框架能力 |

hical 的定位是：**站在 Boost.Beast 的肩膀上，不重复造 HTTP 解析器的轮子，专注在更高层提供现代 C++ 的框架能力** —— 路由、中间件、内存池、协程化 API，以及 C++26 反射驱动的自动化路由注册与 JSON 序列化。

---

## 3. hical 的设计目标

| 目标         | 具体措施                                                              |
| ------------ | --------------------------------------------------------------------- |
| **高性能**   | PMR 三层内存池减少碎片，Scatter-Gather 批量 I/O，零拷贝缓冲区         |
| **现代 C++** | C++20 协程取代回调地狱，Concepts 编译期约束，C++26 反射自动化路由注册 |
| **可扩展**   | 抽象接口层 + NetworkBackend Concept，未来可替换网络后端               |
| **开发友好** | 用户只需编写高层业务代码，框架自动处理异步和内存管理                  |
| **类型安全** | 编译期路由类型检查，Concepts 约束后端接口完整性                       |

一句话概括 hical 的架构哲学：

> **核心抽象层定义"做什么"，适配层决定"怎么做"，用户只关心"业务是什么"。**

---

## 4. 两层架构设计

### 4.1 架构全景

hical 采用**核心层 + 适配层**的两层分离架构：

```
┌──────────────────────────────────────────────────────┐
│                    用户业务代码                        │
│   server.router().get("/api", handler);              │
│   server.use(middleware);                             │
├──────────────────────────────────────────────────────┤
│              核心层 (src/core/)                        │
│  ┌──────────┐ ┌──────────┐ ┌────────────┐            │
│  │HttpServer│ │  Router  │ │ Middleware  │            │
│  │  (门面)   │ │  (路由)   │ │  Pipeline  │            │
│  └─────┬────┘ └─────┬────┘ └──────┬─────┘            │
│        └────────────┼─────────────┘                   │
│  ┌──────────────────┴───────────────────┐            │
│  │         抽象接口层                     │            │
│  │  EventLoop  TcpConnection  Timer     │            │
│  │  MemoryPool  PmrBuffer  Concepts     │            │
│  └──────────────────┬───────────────────┘            │
├──────────────────────┼───────────────────────────────┤
│              Asio 适配层 (src/asio/)                   │
│  ┌──────────────────┴───────────────────┐            │
│  │  AsioEventLoop   AsioTimer            │            │
│  │  GenericConnection<SocketType>        │            │
│  │  EventLoopPool   TcpServer            │            │
│  └──────────────────────────────────────┘            │
├──────────────────────────────────────────────────────┤
│              底层库                                    │
│  Boost.Asio   Boost.Beast   Boost.JSON   OpenSSL     │
└──────────────────────────────────────────────────────┘
```

**核心设计原则：**

1. **上层不依赖下层实现** — `src/core/` 定义纯虚接口和 Concepts，不直接引用 Boost.Asio
2. **用户不感知网络细节** — `HttpServer` 封装了全部网络操作，用户只与 Router/Request/Response 交互
3. **内存池贯穿全链路** — 从网络缓冲区到 HTTP 消息体，共享 PMR 内存池

### 4.2 教学代码：从一个简单的 EventLoop 接口开始

让我们从零开始理解这个架构。首先，定义一个事件循环的抽象接口：

```cpp
// 第一步：定义事件循环应该"能做什么"
class IEventLoop
{
public:
    virtual ~IEventLoop() = default;

    // 生命周期管理
    virtual void run() = 0;     // 启动事件循环（阻塞）
    virtual void stop() = 0;    // 停止事件循环

    // 任务调度
    virtual void post(std::function<void()> cb) = 0;     // 投递到队列（总是异步）
    virtual void dispatch(std::function<void()> cb) = 0;  // 智能调度（同线程直接执行）

    // 定时器
    virtual uint64_t runAfter(double delay, std::function<void()> cb) = 0;
    virtual uint64_t runEvery(double interval, std::function<void()> cb) = 0;
    virtual void cancelTimer(uint64_t id) = 0;

    // 线程属性
    virtual bool isInLoopThread() const = 0;
};
```

这个接口没有一行 Boost 代码。它描述的是"一个事件循环需要具备的能力"，而不是"怎样实现这些能力"。

### 4.3 教学代码：用 Boost.Asio 实现它

接下来，用 Boost.Asio 的 `io_context` 来实现这个接口：

```cpp
#include <boost/asio.hpp>
#include <atomic>
#include <thread>

class AsioEventLoop : public IEventLoop
{
public:
    AsioEventLoop()
        : workGuard_(boost::asio::make_work_guard(ioContext_))
    {}

    // 启动事件循环 — 记录线程 ID，用于后续的 isInLoopThread() 判断
    void run() override
    {
        threadId_ = std::this_thread::get_id();
        running_.store(true);
        ioContext_.run();  // 阻塞，直到 stop() 被调用
        running_.store(false);
    }

    void stop() override
    {
        workGuard_.reset();   // 释放 work guard，允许 io_context 退出
        ioContext_.stop();
    }

    // dispatch：如果当前就在事件循环线程，直接执行；否则投递到队列
    void dispatch(std::function<void()> cb) override
    {
        if (isInLoopThread())
        {
            cb();  // 同线程，直接执行，零延迟
        }
        else
        {
            post(std::move(cb));  // 跨线程，投递到队列
        }
    }

    // post：总是投递到队列（线程安全的异步投递）
    void post(std::function<void()> cb) override
    {
        boost::asio::post(ioContext_, std::move(cb));
    }

    // 延迟执行（简化版，省略定时器管理）
    uint64_t runAfter(double delay, std::function<void()> cb) override
    {
        auto timer = std::make_shared<boost::asio::steady_timer>(
            ioContext_,
            std::chrono::milliseconds(static_cast<int64_t>(delay * 1000)));

        timer->async_wait([timer, cb = std::move(cb)](const auto& ec) {
            if (!ec) cb();
        });
        return 0; // 简化版不返回有效 ID
    }

    bool isInLoopThread() const override
    {
        return threadId_ == std::this_thread::get_id();
    }

    // 暴露底层 io_context（适配层特有，接口层没有）
    boost::asio::io_context& getIoContext() { return ioContext_; }

private:
    boost::asio::io_context ioContext_;
    boost::asio::executor_work_guard<boost::asio::io_context::executor_type> workGuard_;
    std::thread::id threadId_;
    std::atomic<bool> running_{false};
};
```

**关键设计点：**

- **`work_guard`**：防止 `io_context` 在没有待处理任务时自动退出。这是 Boost.Asio 的常见模式
- **`dispatch` vs `post`**：`dispatch` 在同线程直接执行避免不必要的队列开销；`post` 总是异步，保证线程安全
- **线程 ID 记录**：`run()` 时记录线程 ID，`isInLoopThread()` 用于判断当前是否在事件循环线程中

### 4.4 为什么要分层？不直接用 Boost.Asio 不行吗？

你可能会问：既然底层用的就是 Boost.Asio，为什么不直接在业务代码中用它？答案在于三个字：**可替换性**。

```
                  抽象接口
                    │
         ┌──────────┼──────────┐
         │          │          │
    AsioBackend  (未来)IoUring  (未来)自定义
    Boost.Asio   Linux专用     测试Mock
```

- **测试友好**：单元测试时可以 Mock 事件循环，不需要真正启动网络
- **平台优化**：Linux 下可以换成 io_uring 后端，Windows 保持 IOCP
- **概念隔离**：业务层开发者不需要了解 Asio 的 executor 模型、strand 等概念

在 hical 的实际代码中，`EventLoop` 抽象接口还额外提供了 **PMR 分配器**支持：

```cpp
// hical 实际接口（比教学版多了 pmr 支持）
class EventLoop
{
public:
    // ... 生命周期、任务调度、定时器接口 ...

    // 获取事件循环关联的 pmr 分配器（线程本地池）
    virtual std::pmr::polymorphic_allocator<std::byte> allocator() const = 0;
};
```

这使得每个事件循环线程可以使用自己的无锁内存池 —— 关于这一点，我们将在第二篇详细展开。

---

## 5. C++20 Concepts 做后端抽象

### 5.1 从虚函数到 Concepts

传统做法是用虚函数多态来约束后端接口：

```cpp
// 传统方式：运行时多态（有虚函数表开销）
class IEventLoop {
public:
    virtual void run() = 0;
    virtual void stop() = 0;
    virtual void post(std::function<void()>) = 0;
    // ... 通过基类指针调用，每次都经过 vtable 间接跳转
};
```

C++20 Concepts 提供了一种**编译期**约束方式：

```cpp
// 现代方式：编译期约束（零运行时开销）
template <typename T>
concept EventLoopLike = requires(T loop, std::function<void()> func, double delay) {
    { loop.run() } -> std::same_as<void>;
    { loop.stop() } -> std::same_as<void>;
    { loop.isRunning() } -> std::convertible_to<bool>;

    { loop.post(func) } -> std::same_as<void>;
    { loop.dispatch(func) } -> std::same_as<void>;

    { loop.runAfter(delay, func) } -> std::convertible_to<uint64_t>;
    { loop.runEvery(delay, func) } -> std::convertible_to<uint64_t>;
    { loop.cancelTimer(uint64_t{}) } -> std::same_as<void>;

    { loop.isInLoopThread() } -> std::convertible_to<bool>;
    { loop.allocator() } -> std::same_as<std::pmr::polymorphic_allocator<std::byte>>;
};
```

### 5.2 对比两种方式

| 对比项     | 虚函数多态         | Concepts 约束                |
| ---------- | ------------------ | ---------------------------- |
| 检查时机   | 运行时（链接时）   | 编译期                       |
| 运行时开销 | vtable 间接调用    | 零开销（直接内联）           |
| 错误信息   | 链接错误，难以定位 | 编译器直接指出哪个要求未满足 |
| 灵活性     | 可以用基类指针     | 需要模板化使用               |

### 5.3 教学代码：NetworkBackend — 统一后端约束

hical 定义了四层 Concept，最终汇聚到一个 `NetworkBackend` 约束中：

```cpp
// 第一层：事件循环约束
template <typename T>
concept EventLoopLike = requires(T loop, ...) { /* 见上文 */ };

// 第二层：TCP 连接约束
template <typename T>
concept TcpConnectionLike = requires(T conn, const char* data, size_t len) {
    { conn.send(data, len) } -> std::same_as<void>;
    { conn.shutdown() } -> std::same_as<void>;
    { conn.close() } -> std::same_as<void>;
    { conn.connected() } -> std::convertible_to<bool>;
    { conn.bytesSent() } -> std::convertible_to<size_t>;
    { conn.bytesReceived() } -> std::convertible_to<size_t>;
};

// 第三层：定时器约束
template <typename T>
concept TimerLike = requires(T timer) {
    { timer.cancel() } -> std::same_as<void>;
    { timer.isActive() } -> std::convertible_to<bool>;
    { timer.isRepeating() } -> std::convertible_to<bool>;
    { timer.interval() } -> std::convertible_to<double>;
};

// 第四层：统一后端约束 — 将三个组件打包
template <typename T>
concept NetworkBackend =
    requires {
        typename T::EventLoopType;    // 必须定义三个关联类型
        typename T::ConnectionType;
        typename T::TimerType;
    }
    && EventLoopLike<typename T::EventLoopType>
    && TcpConnectionLike<typename T::ConnectionType>
    && TimerLike<typename T::TimerType>;
```

### 5.4 使用 NetworkBackend

定义后端只需一个简单的结构体：

```cpp
// Asio 后端 — 当前的默认实现
struct AsioBackend
{
    using EventLoopType = AsioEventLoop;
    using ConnectionType = PlainConnection;  // GenericConnection<tcp::socket>
    using TimerType = AsioTimer;
};

// 静态断言确保 AsioBackend 满足约束
static_assert(NetworkBackend<AsioBackend>,
              "AsioBackend must satisfy NetworkBackend concept");

// 未来可以添加其他后端
// struct IoUringBackend {
//     using EventLoopType = IoUringEventLoop;
//     using ConnectionType = IoUringConnection;
//     using TimerType = IoUringTimer;
// };
```

用模板消费后端：

```cpp
template <NetworkBackend Backend>
class GenericServer
{
    using Loop = typename Backend::EventLoopType;
    using Conn = typename Backend::ConnectionType;

    Loop mainLoop_;
    // ...
};

// 实例化
GenericServer<AsioBackend> server;
```

如果你定义了一个不满足约束的后端，编译器会给出清晰的错误信息，告诉你具体哪个方法缺失——这比传统的模板错误信息友好得多。

---

## 6. 线程模型：1 Thread : 1 io_context

### 6.1 为什么不用单线程 + 多个 io_context 共享？

常见的线程模型有两种：

**方案 A：多线程共享一个 io_context**
```
Thread 1 ──┐
Thread 2 ──┼── io_context (共享)
Thread 3 ──┘
```
- 优点：实现简单
- 缺点：多线程竞争 io_context 内部锁

**方案 B：每个线程独享一个 io_context（hical 选择）**
```
Thread 1 ── io_context #1 ── [conn A, conn B]
Thread 2 ── io_context #2 ── [conn C, conn D]
Thread 3 ── io_context #3 ── [conn E, conn F]
```
- 优点：线程间零共享状态，天然无锁
- 缺点：需要 round-robin 分配连接

hical 选择了方案 B，原因很简单：**在高并发场景下，锁竞争是首要敌人**。

### 6.2 教学代码：手写简化版 EventLoopPool

```cpp
#include <atomic>
#include <memory>
#include <thread>
#include <vector>

class EventLoopPool
{
public:
    explicit EventLoopPool(size_t numThreads)
    {
        // 创建 N 个独立的事件循环
        for (size_t i = 0; i < numThreads; ++i)
        {
            auto loop = std::make_unique<AsioEventLoop>();
            loop->setIndex(i);
            loops_.push_back(std::move(loop));
        }
    }

    // 启动所有线程
    void start()
    {
        for (auto& loop : loops_)
        {
            auto* ptr = loop.get();
            threads_.emplace_back([ptr]() {
                ptr->run();  // 每个线程独立运行自己的 io_context
            });
        }
    }

    // Round-Robin 获取下一个事件循环
    AsioEventLoop* getNextLoop()
    {
        // 原子递增 + 取模，线程安全且无锁
        size_t index = nextIndex_.fetch_add(1) % loops_.size();
        return loops_[index].get();
    }

    void stop()
    {
        for (auto& loop : loops_)
            loop->stop();
        for (auto& thread : threads_)
            if (thread.joinable())
                thread.join();
    }

private:
    std::vector<std::unique_ptr<AsioEventLoop>> loops_;
    std::vector<std::thread> threads_;
    std::atomic<size_t> nextIndex_{0};  // Round-Robin 计数器
};
```

**核心设计：**

- **`fetch_add` + 取模**：原子操作保证线程安全，无锁实现 round-robin
- **每线程独立 `run()`**：线程间完全独立，没有共享的 io_context
- **连接绑定**：一个连接一旦分配到某个线程，其所有 I/O 操作都在该线程内完成，无需跨线程同步

### 6.3 实际应用中的线程模型

```
┌─────────────────────────────────────────────────────┐
│                  主线程                               │
│  ┌────────────────────────┐                          │
│  │    Main io_context      │                          │
│  │  - acceptLoop()         │  接受新连接               │
│  │  - 调用 pool.getNext()  │  Round-Robin 选择线程     │
│  └─────────┬──────────────┘                          │
│            │ 新连接                                   │
├────────────┼────────────────────────────────────────┤
│            ▼                                         │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐           │
│  │ Thread 1 │  │ Thread 2 │  │ Thread N │           │
│  │io_ctx #1 │  │io_ctx #2 │  │io_ctx #N │           │
│  │          │  │          │  │          │           │
│  │ conn A   │  │ conn C   │  │ conn E   │           │
│  │ conn B   │  │ conn D   │  │ conn F   │           │
│  │          │  │          │  │          │           │
│  │ 每个线程  │  │ 有自己的  │  │ 无锁内存  │           │
│  │ thread_  │  │ thread_  │  │ 池       │           │
│  │ local池  │  │ local池  │  │          │           │
│  └──────────┘  └──────────┘  └──────────┘           │
└─────────────────────────────────────────────────────┘
```

注意：每个 IO 线程还拥有自己的 **thread_local 内存池**（无锁 `unsynchronized_pool_resource`），这使得线程内的内存分配完全避免了锁竞争。这一设计将在第二篇中详细展开。

---

## 7. 全文总结

本篇我们从零出发，建立了 hical 框架的设计直觉：

| 设计决策 | 选择                    | 核心理由                   |
| -------- | ----------------------- | -------------------------- |
| 架构分层 | 核心层 + 适配层         | 接口与实现分离，可替换后端 |
| 后端抽象 | C++20 Concepts          | 编译期约束，零运行时开销   |
| 线程模型 | 1 Thread : 1 io_context | 线程间无共享状态，天然无锁 |
| 连接分配 | Round-Robin             | 无锁原子操作，均匀分布     |

### 架构全景图

```
用户代码
   │
   ▼
HttpServer ─── Router + MiddlewarePipeline
   │
   ▼
抽象接口（EventLoop / TcpConnection / Timer）
   │          ▲
   │          │ Concepts 编译期约束
   ▼          │
AsioBackend ── AsioEventLoop + GenericConnection + AsioTimer
   │
   ▼
EventLoopPool（1:1 线程模型，Round-Robin 分配）
   │
   ▼
Boost.Asio io_context × N
```

### 下篇预告

在第二篇中，我们将深入两个核心技术：

1. **协程** — 从传统回调到 `co_await`，如何用协程优雅地处理异步 I/O
2. **PMR 三层内存池** — 全局同步池 → 线程本地无锁池 → 请求级单调池，以及它们如何协同工作

敬请期待！

---

> **hical** — 基于 C++26 的现代高性能 Web 框架 | [GitHub](https://github.com/user/hical)
