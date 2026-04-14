# 从零构建现代C++ Web服务器（二）：协程异步与 PMR 内存池

> **系列导航**：[第一篇：设计理念](01-design-philosophy.md) | [第二篇：协程与内存池](#)（本文） | [第三篇：路由、中间件与SSL](03-router-middleware-ssl.md) | [第四篇：实战与性能](04-practice-and-performance.md) | [第五篇：Cookie、Session与文件服务](05-cookies-sessions-fileservices.md)

## 前置知识

- 了解协程关键字（`co_await`、`co_return`、`co_yield`）的基本含义
- 理解智能指针和 RAII 模式
- 了解 `std::pmr`（Polymorphic Memory Resource）的基本概念

---

## 目录

- [从零构建现代C++ Web服务器（二）：协程异步与 PMR 内存池](#从零构建现代c-web服务器二协程异步与-pmr-内存池)
  - [前置知识](#前置知识)
  - [目录](#目录)
  - [1. 从回调地狱到 co\_await](#1-从回调地狱到-co_await)
    - [1.1 传统回调方式](#11-传统回调方式)
    - [1.2 协程方式](#12-协程方式)
    - [1.3 协程的工作原理（简化）](#13-协程的工作原理简化)
  - [2. Awaitable：hical 的协程基石](#2-awaitablehical-的协程基石)
    - [工具函数](#工具函数)
  - [3. 协程在框架中的实际应用](#3-协程在框架中的实际应用)
    - [3.1 Accept 循环](#31-accept-循环)
    - [3.2 HTTP 会话处理](#32-http-会话处理)
    - [3.3 协程执行全景](#33-协程执行全景)
  - [4. PMR 内存池：为什么默认 allocator 不够好](#4-pmr-内存池为什么默认-allocator-不够好)
    - [4.1 问题分析](#41-问题分析)
    - [4.2 Benchmark 对比](#42-benchmark-对比)
  - [5. 三层内存架构深度剖析](#5-三层内存架构深度剖析)
    - [5.1 教学代码：从零实现简化版三层 PMR](#51-教学代码从零实现简化版三层-pmr)
    - [5.2 教学代码：三层管理器](#52-教学代码三层管理器)
    - [5.3 generation 计数器的妙用](#53-generation-计数器的妙用)
  - [6. PmrBuffer：零拷贝缓冲区](#6-pmrbuffer零拷贝缓冲区)
    - [6.1 设计思路](#61-设计思路)
    - [6.2 教学代码：简化版 PmrBuffer](#62-教学代码简化版-pmrbuffer)
    - [6.3 makeSpace 策略图解](#63-makespace-策略图解)
  - [7. 协程 + PMR 协同](#7-协程--pmr-协同)
  - [8. 总结](#8-总结)
    - [核心要点](#核心要点)
    - [下篇预告](#下篇预告)

---

## 1. 从回调地狱到 co_await

### 1.1 传统回调方式

假设我们要写一个 TCP Echo Server。传统的异步回调方式是这样的：

```cpp
// 传统回调方式 — 嵌套回调（回调地狱）
void doRead(tcp::socket& socket, std::array<char, 1024>& buf)
{
    socket.async_read_some(
        boost::asio::buffer(buf),
        [&](boost::system::error_code ec, size_t bytesRead)
        {
            if (ec) return;  // 错误处理

            // 读到数据后，异步写回
            boost::asio::async_write(
                socket,
                boost::asio::buffer(buf, bytesRead),
                [&](boost::system::error_code ec2, size_t /*bytesWritten*/)
                {
                    if (ec2) return;

                    // 写完后，继续读
                    doRead(socket, buf);  // 递归调用，形成循环
                });
        });
}
```

问题很明显：
- **嵌套回调**：逻辑层层嵌套，难以阅读和维护
- **错误处理分散**：每个回调都要独立处理错误
- **状态管理困难**：需要手动管理 socket 和 buffer 的生命周期
- **递归模式**：用递归调用模拟循环，违背直觉

### 1.2 协程方式

同样的逻辑，用协程重写：

```cpp
// 协程方式 — 代码像同步一样线性流动
awaitable<void> handleSession(tcp::socket socket)
{
    try
    {
        char data[1024];
        for (;;)
        {
            // 异步读取 — 看起来像阻塞读，但实际上线程不会阻塞
            auto bytesRead = co_await socket.async_read_some(
                boost::asio::buffer(data), use_awaitable);

            // 异步写入 — 同样不阻塞线程
            co_await boost::asio::async_write(
                socket, boost::asio::buffer(data, bytesRead), use_awaitable);
        }
    }
    catch (const std::exception&)
    {
        // 统一的错误处理
    }
}
```

**改进：**
- 代码像同步一样线性流动，`for (;;)` 就是真正的循环
- 错误处理用标准 try/catch，统一且直觉
- `co_await` 挂起时释放线程，其他连接可以继续处理
- socket 的生命周期由协程 frame 自动管理

### 1.3 协程的工作原理（简化）

```
协程调用 co_await socket.async_read_some(...)
    │
    ▼
挂起当前协程（保存状态到 coroutine frame）
    │
    ▼
释放线程 → 线程去处理其他任务
    │
    │  ... 操作系统完成 I/O ...
    │
    ▼
I/O 完成通知到达 → 恢复协程执行
    │
    ▼
继续执行 co_await 之后的代码
```

关键点：**协程挂起时不占用线程**。1000 个并发连接可以在 4 个线程上高效运行，因为大部分时间连接都在等待 I/O。

---

## 2. Awaitable：hical 的协程基石

hical 将 `boost::asio::awaitable<T>` 封装为一个简洁的别名：

```cpp
// hical 的协程返回类型
template <typename T = void>
using Awaitable = boost::asio::awaitable<T>;
```

为什么要封装？

1. **命名统一**：整个框架使用 `Awaitable<T>`，而不是到处写 `boost::asio::awaitable<T>`
2. **未来可替换**：如果将来标准库提供更好的协程类型，只需改一行 `using`
3. **简洁性**：`Awaitable<HttpResponse>` 比 `boost::asio::awaitable<HttpResponse>` 清爽得多

### 工具函数

hical 还提供了几个常用的协程工具函数：

```cpp
namespace hical
{
    // 在协程中休眠（自动获取 executor）
    inline Awaitable<void> sleep(double seconds)
    {
        auto executor = co_await boost::asio::this_coro::executor;
        boost::asio::steady_timer timer(executor,
            std::chrono::milliseconds(static_cast<int64_t>(seconds * 1000)));
        co_await timer.async_wait(boost::asio::use_awaitable);
    }

    // 启动一个独立协程（fire-and-forget）
    template <typename F>
    void coSpawn(boost::asio::io_context& ioCtx, F&& coroutine)
    {
        boost::asio::co_spawn(ioCtx, std::forward<F>(coroutine),
                              boost::asio::detached);
    }
}
```

`sleep()` 内部使用 `this_coro::executor` 自动获取当前协程的执行器，用户不需要手动传递 `io_context`。

---

## 3. 协程在框架中的实际应用

让我们看看 hical 如何用协程构建完整的 HTTP 服务器。

### 3.1 Accept 循环

```cpp
// HttpServer::acceptLoop() — 接受新连接的协程
Awaitable<void> HttpServer::acceptLoop()
{
    while (running_.load())
    {
        try
        {
            // 异步接受连接 — 挂起直到有新连接到达
            auto socket = co_await acceptor_->async_accept(
                boost::asio::use_awaitable);

            // 连接数限制检查
            if (maxConnections_ > 0 &&
                activeConnections_.load() >= maxConnections_)
            {
                socket.close();  // 超过上限，直接关闭
                continue;
            }

            // 为每个新连接启动独立的会话协程
            boost::asio::co_spawn(
                ioContext_,
                handleSession(std::move(socket)),
                boost::asio::detached);
        }
        catch (const boost::system::system_error& e)
        {
            if (e.code() == boost::asio::error::operation_aborted)
                break;  // 服务器停止
            // 瞬态错误（如 EMFILE）继续接受
        }
    }
}
```

### 3.2 HTTP 会话处理

```cpp
// 每个连接的处理协程
Awaitable<void> HttpServer::handleSession(tcp::socket socket)
{
    // 连接计数 RAII 守卫
    activeConnections_.fetch_add(1);
    struct Counter {
        std::atomic<size_t>& count;
        ~Counter() { count.fetch_sub(1); }
    } counter{activeConnections_};

    try
    {
        // 使用请求级 PMR 池（整个连接共享，第 5 节详解）
        auto requestPool = MemoryPool::instance().createRequestPool();
        std::pmr::polymorphic_allocator<std::byte> alloc(requestPool.get());
        beast::basic_flat_buffer<decltype(alloc)> buffer(alloc);

        for (;;)
        {
            // 解析请求（支持大小限制防 OOM 攻击）
            http::request_parser<http::string_body> parser;
            parser.body_limit(maxBodySize_);

            // 异步读取完整的 HTTP 请求
            co_await http::async_read(socket, buffer, parser,
                                      boost::asio::use_awaitable);

            auto beastReq = parser.release();
            HttpRequest req(std::move(beastReq));

            // 通过中间件管道 + 路由器处理请求
            auto res = co_await middlewarePipeline_.execute(req,
                [this](HttpRequest& r) -> Awaitable<HttpResponse> {
                    co_return co_await router_.dispatch(r);
                });

            // 异步发送响应
            co_await http::async_write(socket, res.native(),
                                       boost::asio::use_awaitable);

            if (!res.native().keep_alive())
                break;  // 非 keep-alive 连接，退出循环
        }
    }
    catch (const beast::system_error&)
    {
        // 连接关闭或网络错误
    }
    // socket 在协程结束时自动析构关闭
}
```

### 3.3 协程执行全景

```
io_context.run()
    │
    ├── co_spawn(acceptLoop)
    │       │
    │       ├── co_await async_accept()  ──→ 新连接
    │       │       │
    │       │       └── co_spawn(handleSession)
    │       │               │
    │       │               ├── co_await http::async_read()
    │       │               ├── co_await middleware.execute()
    │       │               │       ├── co_await next(req)
    │       │               │       └── co_await router.dispatch()
    │       │               └── co_await http::async_write()
    │       │
    │       └── 继续 accept 下一个连接
    │
    └── io_context 调度所有协程（单线程内无锁切换）
```

---

## 4. PMR 内存池：为什么默认 allocator 不够好

### 4.1 问题分析

高并发 Web 服务器每秒可能处理数万个请求，每个请求涉及：
- 接收缓冲区分配
- HTTP 头部解析的临时字符串
- JSON body 解析
- 响应体构建

传统 `new/delete`（即 `std::allocator`）在此场景下的问题：

```
问题 1：锁竞争
  Thread 1: new → 全局堆锁 → 等待...
  Thread 2: new → 全局堆锁 → 等待...
  Thread 3: new → 全局堆锁 → 等待...
  → 多线程同时 malloc，全局锁成为瓶颈

问题 2：内存碎片
  [used 64B][free 32B][used 128B][free 16B][used 64B]...
  → 大量小块分配/释放后，内存碎片化严重
  → 降低 CPU 缓存命中率

问题 3：分配延迟
  每次 new → 遍历空闲链表找到合适块 → 可能触发 mmap 系统调用
  → 单次分配延迟不可预测
```

### 4.2 Benchmark 对比

下面是一个简化的 benchmark，对比不同分配策略的性能：

```cpp
#include <memory_resource>
#include <chrono>
#include <iostream>

// 标准 new/delete
void benchNewDelete(int iterations, size_t size)
{
    auto start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < iterations; ++i)
    {
        auto* p = new char[size];
        delete[] p;
    }
    auto end = std::chrono::high_resolution_clock::now();
    auto ms = std::chrono::duration<double, std::milli>(end - start).count();
    std::cout << "  new/delete:       " << ms << " ms\n";
}

// PMR synchronized_pool（线程安全，有锁）
void benchSyncPool(int iterations, size_t size)
{
    std::pmr::synchronized_pool_resource pool;
    std::pmr::polymorphic_allocator<char> alloc(&pool);

    auto start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < iterations; ++i)
    {
        auto* p = alloc.allocate(size);
        alloc.deallocate(p, size);
    }
    auto end = std::chrono::high_resolution_clock::now();
    auto ms = std::chrono::duration<double, std::milli>(end - start).count();
    std::cout << "  pmr sync_pool:    " << ms << " ms\n";
}

// PMR unsynchronized_pool（单线程无锁，最快）
void benchUnsyncPool(int iterations, size_t size)
{
    std::pmr::unsynchronized_pool_resource pool;
    std::pmr::polymorphic_allocator<char> alloc(&pool);

    auto start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < iterations; ++i)
    {
        auto* p = alloc.allocate(size);
        alloc.deallocate(p, size);
    }
    auto end = std::chrono::high_resolution_clock::now();
    auto ms = std::chrono::duration<double, std::milli>(end - start).count();
    std::cout << "  pmr unsync_pool:  " << ms << " ms\n";
}

// PMR monotonic（只分配不释放，析构时整体回收）
void benchMonotonic(int iterations, size_t size)
{
    std::pmr::monotonic_buffer_resource mono(size * iterations + 1024);
    std::pmr::polymorphic_allocator<char> alloc(&mono);

    auto start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < iterations; ++i)
    {
        auto* p = alloc.allocate(size);
        (void)p;  // monotonic 不需要 deallocate
    }
    auto end = std::chrono::high_resolution_clock::now();
    auto ms = std::chrono::duration<double, std::milli>(end - start).count();
    std::cout << "  pmr monotonic:    " << ms << " ms\n";
}

int main()
{
    const int N = 1000000;
    const size_t SIZE = 256;
    std::cout << "分配/释放 " << N << " 次, 每次 " << SIZE << " 字节:\n";
    benchNewDelete(N, SIZE);
    benchSyncPool(N, SIZE);
    benchUnsyncPool(N, SIZE);
    benchMonotonic(N, SIZE);
}
```

典型结果（具体数值因硬件而异）：

```
分配/释放 1000000 次, 每次 256 字节:
  new/delete:       45.2 ms
  pmr sync_pool:    28.1 ms     (比 new/delete 快 ~40%)
  pmr unsync_pool:  12.3 ms     (比 new/delete 快 ~70%)
  pmr monotonic:     3.8 ms     (比 new/delete 快 ~90%)
```

**关键结论：**
- `unsynchronized_pool`（无锁）比 `new/delete` 快数倍，适合单线程场景
- `monotonic_buffer`（只分配不释放）最快，适合请求级的一次性分配

---

## 5. 三层内存架构深度剖析

hical 将上面三种 PMR 策略组合成一个三层架构：

```
┌──────────────────────────────────────────────┐
│          第一层：全局同步池                     │
│    synchronized_pool_resource                │
│    ┌────────────────────────────────────┐    │
│    │  TrackedResource (原子计数统计)      │    │
│    │  ↓ 上游: new_delete_resource       │    │
│    └────────────────────────────────────┘    │
│    用途：跨线程共享的全局分配                   │
│    特点：线程安全，有锁                         │
├──────────────────────────────────────────────┤
│          第二层：线程本地池                     │
│    unsynchronized_pool_resource (thread_local)│
│    ┌────────────────────────────────────┐    │
│    │  上游: 全局同步池                    │    │
│    └────────────────────────────────────┘    │
│    用途：每个线程独享的快速分配器               │
│    特点：无锁，零竞争                          │
├──────────────────────────────────────────────┤
│          第三层：请求级单调池                   │
│    monotonic_buffer_resource                 │
│    ┌────────────────────────────────────┐    │
│    │  上游: 全局同步池                    │    │
│    └────────────────────────────────────┘    │
│    用途：单次 HTTP 请求的生命周期分配           │
│    特点：只分配不释放，析构时整体回收            │
└──────────────────────────────────────────────┘
```

### 5.1 教学代码：从零实现简化版三层 PMR

```cpp
#include <memory_resource>
#include <atomic>
#include <mutex>
#include <vector>
#include <memory>

// 第一步：追踪层 — 包装上游资源，做分配统计
class TrackedResource : public std::pmr::memory_resource
{
public:
    explicit TrackedResource(std::pmr::memory_resource* upstream)
        : upstream_(upstream) {}

    size_t totalAllocations() const { return totalAllocs_.load(); }
    size_t currentBytes() const { return currentBytes_.load(); }
    size_t peakBytes() const { return peakBytes_.load(); }

protected:
    void* do_allocate(size_t bytes, size_t alignment) override
    {
        void* p = upstream_->allocate(bytes, alignment);
        totalAllocs_.fetch_add(1, std::memory_order_relaxed);

        // 更新当前字节数
        auto current = currentBytes_.fetch_add(bytes) + bytes;

        // 无锁 CAS 更新峰值
        auto peak = peakBytes_.load(std::memory_order_relaxed);
        while (current > peak &&
               !peakBytes_.compare_exchange_weak(peak, current)) {}

        return p;
    }

    void do_deallocate(void* p, size_t bytes, size_t alignment) override
    {
        upstream_->deallocate(p, bytes, alignment);
        currentBytes_.fetch_sub(bytes, std::memory_order_relaxed);
    }

    bool do_is_equal(const memory_resource& other) const noexcept override
    {
        return this == &other;
    }

private:
    std::pmr::memory_resource* upstream_;
    std::atomic<size_t> totalAllocs_{0};
    std::atomic<size_t> currentBytes_{0};
    std::atomic<size_t> peakBytes_{0};
};
```

**关键技巧：无锁 CAS 更新峰值**

```cpp
auto peak = peakBytes_.load(std::memory_order_relaxed);
while (current > peak &&
       !peakBytes_.compare_exchange_weak(peak, current)) {}
```

这段代码的含义：如果当前值 `current` 大于已知峰值 `peak`，尝试用 CAS（Compare-And-Swap）更新。如果另一个线程同时更新了峰值，CAS 会失败并重新加载最新的 `peak`，然后重试。整个过程无需互斥锁。

### 5.2 教学代码：三层管理器

```cpp
class MemoryPool
{
public:
    static MemoryPool& instance()
    {
        static MemoryPool inst;
        return inst;
    }

    // 第一层：全局同步池的分配器
    std::pmr::polymorphic_allocator<std::byte> globalAllocator()
    {
        return std::pmr::polymorphic_allocator<std::byte>(&globalPool_);
    }

    // 第二层：线程本地池的分配器（无锁）
    std::pmr::polymorphic_allocator<std::byte> threadLocalAllocator()
    {
        auto* pool = getOrCreateThreadPool();
        return std::pmr::polymorphic_allocator<std::byte>(pool);
    }

    // 第三层：创建请求级单调池
    std::unique_ptr<std::pmr::monotonic_buffer_resource>
    createRequestPool(size_t initialSize = 4096)
    {
        return std::make_unique<std::pmr::monotonic_buffer_resource>(
            initialSize, &globalPool_);
    }

private:
    MemoryPool()
        : tracked_(std::pmr::new_delete_resource())
        , globalPool_({.max_blocks_per_chunk = 128,
                       .largest_required_pool_block = 1024 * 1024},
                      &tracked_)
    {}

    // 代际感知的 thread_local 缓存
    std::pmr::unsynchronized_pool_resource* getOrCreateThreadPool()
    {
        struct ThreadCache {
            std::pmr::unsynchronized_pool_resource* pool = nullptr;
            uint64_t generation = 0;
        };

        thread_local ThreadCache cache;

        auto currentGen = generation_.load(std::memory_order_acquire);
        if (cache.pool && cache.generation == currentGen)
        {
            return cache.pool;  // 缓存命中，直接返回
        }

        // 缓存未命中：创建新的线程本地池
        auto pool = std::make_unique<std::pmr::unsynchronized_pool_resource>(
            std::pmr::pool_options{.max_blocks_per_chunk = 64,
                                   .largest_required_pool_block = 512 * 1024},
            &globalPool_);  // 上游是全局同步池

        auto* ptr = pool.get();

        {
            std::lock_guard lock(mutex_);
            threadPools_.push_back(std::move(pool));  // 转移所有权
        }

        cache.pool = ptr;
        cache.generation = currentGen;
        return ptr;
    }

    TrackedResource tracked_;
    std::pmr::synchronized_pool_resource globalPool_;

    std::mutex mutex_;
    std::vector<std::unique_ptr<std::pmr::unsynchronized_pool_resource>> threadPools_;
    std::atomic<uint64_t> generation_{0};
};
```

### 5.3 generation 计数器的妙用

```cpp
// 问题：如果运行时调用 configure() 重新配置池，
// 线程的 thread_local 缓存还指向旧的池怎么办？

void configure(const PoolConfig& config)
{
    // 1. 清理旧的线程本地池
    threadPools_.clear();

    // 2. 重建全局池
    globalPool_.~synchronized_pool_resource();
    new (&globalPool_) std::pmr::synchronized_pool_resource(
        newOptions, &tracked_);

    // 3. 递增代际计数器 → 所有 thread_local 缓存自动失效
    generation_.fetch_add(1, std::memory_order_release);
}
```

每个线程的 `ThreadCache` 保存了 `generation`。当 `configure()` 递增全局 generation 后，所有线程在下次调用 `threadLocalAllocator()` 时会发现 `cache.generation != currentGen`，从而重新创建线程本地池。

这个设计避免了需要"通知所有线程重建池"的复杂同步机制。

---

## 6. PmrBuffer：零拷贝缓冲区

hical 设计了 `PmrBuffer` 作为统一的网络读写缓冲区：

### 6.1 设计思路

```
┌─────────┬──────────────────┬────────────────┐
│ prepend │   readable data  │    writable    │
│  (8B)   │                  │    space       │
├─────────┼──────────────────┼────────────────┤
0      readIndex_         writeIndex_      buffer_.size()
```

- **prepend 区域**：预留 8 字节头部空间，用于 TCP 分包协议（长度前缀等），无需内存移动
- **可读区域**：`[readIndex_, writeIndex_)` 之间的数据
- **可写区域**：`[writeIndex_, buffer_.size())` 的空闲空间

### 6.2 教学代码：简化版 PmrBuffer

```cpp
class PmrBuffer
{
public:
    static constexpr size_t kPrependSize = 8;
    static constexpr size_t kDefaultSize = 2048;

    explicit PmrBuffer(
        std::pmr::polymorphic_allocator<std::byte> alloc = {},
        size_t initialSize = kDefaultSize)
        : buffer_(kPrependSize + initialSize, alloc)
        , readIndex_(kPrependSize)
        , writeIndex_(kPrependSize)
    {}

    // 可读字节数
    size_t readableBytes() const { return writeIndex_ - readIndex_; }

    // 可写字节数
    size_t writableBytes() const { return buffer_.size() - writeIndex_; }

    // 获取可读数据指针（零拷贝，不分配新内存）
    const char* peek() const
    {
        return reinterpret_cast<const char*>(buffer_.data() + readIndex_);
    }

    // 追加数据
    void append(const char* data, size_t len)
    {
        ensureWritableBytes(len);
        std::copy(data, data + len,
                  reinterpret_cast<char*>(buffer_.data() + writeIndex_));
        writeIndex_ += len;
    }

    // 消费指定字节数的数据
    void retrieve(size_t len)
    {
        if (len < readableBytes())
            readIndex_ += len;
        else
            retrieveAll();
    }

    void retrieveAll()
    {
        readIndex_ = kPrependSize;
        writeIndex_ = kPrependSize;
    }

    // 读取所有数据为 string
    std::string readAll()
    {
        std::string result(peek(), readableBytes());
        retrieveAll();
        return result;
    }

private:
    void ensureWritableBytes(size_t len)
    {
        if (writableBytes() < len)
        {
            makeSpace(len);
        }
    }

    void makeSpace(size_t len)
    {
        // 策略：先尝试移动数据到前面，如果空间还不够再扩容
        if (writableBytes() + (readIndex_ - kPrependSize) < len)
        {
            // 空间不够，扩容
            buffer_.resize(writeIndex_ + len);
        }
        else
        {
            // 将可读数据移动到前面，回收已消费空间
            size_t readable = readableBytes();
            std::copy(buffer_.data() + readIndex_,
                      buffer_.data() + writeIndex_,
                      buffer_.data() + kPrependSize);
            readIndex_ = kPrependSize;
            writeIndex_ = readIndex_ + readable;
        }
    }

    std::pmr::vector<std::byte> buffer_;  // 底层使用 pmr 分配器
    size_t readIndex_;
    size_t writeIndex_;
};
```

### 6.3 makeSpace 策略图解

```
初始状态（readIndex 已经前进了一段）：
┌─────────┬─ consumed ─┬──── data ────┬── writable ──┐
│ prepend │  (已消费)    │  (可读数据)   │  (空闲空间)   │
└─────────┴─────────────┴──────────────┴──────────────┘
0         8           128           256            512

调用 makeSpace 后（移动数据到前面）：
┌─────────┬──── data ────┬──────── writable ──────────┐
│ prepend │  (可读数据)   │       (回收了 consumed)      │
└─────────┴──────────────┴────────────────────────────┘
0         8             136                           512
```

通过移动数据（而非重新分配），我们回收了 `consumed` 区域的空间，避免了频繁的内存分配。

---

## 7. 协程 + PMR 协同

协程和 PMR 内存池的精妙之处在于它们的**生命周期完美匹配**：

```
HTTP 请求处理协程 handleSession()
│
├── 创建请求级 PMR 池
│   auto requestPool = MemoryPool::instance().createRequestPool(4096);
│
├── co_await http::async_read()     ← 使用 PMR 池的 buffer
│
├── HttpRequest 解析                 ← 临时数据分配在 PMR 池中
│
├── JSON 解析                        ← JSON 节点分配在 PMR 池中
│
├── co_await router_.dispatch(req)  ← 路由处理
│
├── co_await http::async_write()    ← 发送响应
│
└── 协程结束 / 下一次循环
    └── requestPool 析构 → 所有请求数据一次性释放！
        （无需逐个 delete，零碎片）
```

**关键优势：**

1. **零碎片**：请求处理期间的所有分配在连续内存中，析构时整体释放
2. **缓存友好**：连续内存意味着更高的 CPU 缓存命中率
3. **无释放开销**：`monotonic_buffer_resource` 的 deallocate 是空操作，析构时一次性归还上游
4. **协程天然作用域**：协程函数体就是请求的生命周期，PMR 池在栈上，自动 RAII

hical 的实际代码：

```cpp
Awaitable<void> HttpServer::handleSession(tcp::socket socket)
{
    // 请求级 PMR 池，整个连接生命周期复用
    auto requestPool = MemoryPool::instance().createRequestPool();
    std::pmr::polymorphic_allocator<std::byte> alloc(requestPool.get());

    // Beast 的 buffer 也使用 PMR 分配器
    beast::basic_flat_buffer<std::pmr::polymorphic_allocator<std::byte>>
        buffer(alloc);

    for (;;)
    {
        // ... 读取、处理、响应 — 全程使用 PMR 分配器
    }
    // 协程结束 → requestPool 析构 → 整体释放
}
```

---

## 8. 总结

本篇深入了两个核心技术：

| 技术           | 核心价值                       | 关键实现                         |
| -------------- | ------------------------------ | -------------------------------- |
| **协程**       | 异步代码线性化，消除回调地狱   | `Awaitable<T>` + `co_await`      |
| **PMR 三层池** | 高效内存管理，减少碎片和锁竞争 | 全局同步 → 线程无锁 → 请求级单调 |
| **PmrBuffer**  | 零拷贝网络缓冲区               | prepend + makeSpace 智能策略     |
| **协同设计**   | 协程作用域 = PMR 池生命周期    | 请求结束自动整体释放             |

### 核心要点

1. **协程不是语法糖** — 它改变了异步编程的思维方式，让错误处理、资源管理回归正常的 C++ 模式
2. **PMR 不是过早优化** — 在高并发 Web 场景下，内存分配模式是可预测的（请求级分配-释放），PMR 让我们利用这种模式
3. **三层策略各有分工** — 全局池兜底跨线程，线程池避免锁竞争，单调池实现零碎片

### 下篇预告

在第三篇中，我们将实现框架的核心功能组件：

1. **路由系统** — 静态路由哈希表 O(1) + 参数路由模式匹配
2. **洋葱模型中间件** — 预构建链式调用，const 引用捕获优化
3. **模板化 SSL** — `if constexpr` 编译期分支，零运行时开销
4. **WebSocket** — 协程封装 + 原子 CAS 关闭协调

---

> **hical** — 基于 C++26 的现代高性能 Web 框架 | [GitHub](https://github.com/user/hical)
