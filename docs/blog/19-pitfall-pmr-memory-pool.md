# PMR 三层内存池踩坑实录：从理论完美到实战翻车

## 引言

Hical 的内存管理采用 C++20 PMR（Polymorphic Memory Resource）三层池架构：全局同步池 → 线程本地无锁池 → 请求级单调缓冲。理论上完美——每一层解决一个特定的性能瓶颈。

但理论和实战之间，隔着一堆坑。

这篇记录了三层 PMR 池在开发和压测过程中遇到的 7 个真实问题——从跨线程 UAF 到 GC 永远不触发、从 CAS 自旋到缓冲区膨胀，每个都是排查半天以上的教训。

---

## 目录

- [PMR 三层内存池踩坑实录：从理论完美到实战翻车](#pmr-三层内存池踩坑实录从理论完美到实战翻车)
  - [引言](#引言)
  - [目录](#目录)
  - [坑 1：configure() 原地重建的 use-after-free](#坑-1configure-原地重建的-use-after-free)
  - [坑 2：generation 缓存失效的竞争窗口](#坑-2generation-缓存失效的竞争窗口)
  - [坑 3：GC 标记了但永远不回收——死线程的内存泄漏](#坑-3gc-标记了但永远不回收死线程的内存泄漏)
  - [坑 4：CAS 峰值更新的缓存行风暴](#坑-4cas-峰值更新的缓存行风暴)
  - [坑 5：请求级单调池的 upstream 选错](#坑-5请求级单调池的-upstream-选错)
  - [坑 6：PmrBuffer 缩容不及时导致内存膨胀](#坑-6pmrbuffer-缩容不及时导致内存膨胀)
  - [坑 7：allocator 传播链断裂——PMR 白忙一场](#坑-7allocator-传播链断裂pmr-白忙一场)
  - [总结：PMR 三层池的使用清单](#总结pmr-三层池的使用清单)

---

## 坑 1：configure() 原地重建的 use-after-free

**现象**：在服务启动流程中调用 `MemoryPool::configure()` 后，偶发崩溃，堆栈指向 `synchronized_pool_resource` 的内部结构。

**根因**：`configure()` 使用 placement new 原地重建全局池——先析构旧池，再构造新池：

```cpp
// MemoryPool.cpp — configure() 的原地重建
void MemoryPool::configure(const PoolConfig& config)
{
    config_ = config;

    // 先清理所有线程本地池（它们引用 globalPool_ 作为上游）
    {
        std::lock_guard<std::mutex> lock(threadPoolsMutex_);
        threadPools_.clear();
    }

    // 重建全局池（placement new 原地重建）
    globalPool_.~synchronized_pool_resource();          // ← 析构旧池
    new (&globalPool_) std::pmr::synchronized_pool_resource(
        std::pmr::pool_options{
            .max_blocks_per_chunk = config_.globalMaxBlocksPerChunk,
            .largest_required_pool_block = config_.globalLargestPoolBlock},
        &trackedResource_);                             // ← 构造新池

    // 递增代际计数器，使所有 thread_local 缓存失效
    generation_.fetch_add(1, std::memory_order_release);
}
```

问题在于：**析构旧池和构造新池之间不是原子的**。如果在这个间隙中有其他线程通过 `threadLocalAllocator()` 访问 `globalPool_`（作为 upstream），就是 use-after-free。

即使文档注释写了"必须在单线程环境中调用"：

```cpp
// MemoryPool.h — configure() 的警告注释
/**
 * @warning 线程安全约束：此方法必须在服务器启动前、单线程环境中调用。
 * 如果在多线程运行期间调用，会导致 use-after-free。
 */
void configure(const PoolConfig& config);
```

但在复杂的启动流程中（比如某个初始化模块提前启动了工作线程），这个约束很容易被违反。

**解决方案**：

1. 加 `assert` 防御（至少在 Debug 构建中能捕获）：

```cpp
assert(generation_.load(std::memory_order_relaxed) == 0
       || !"configure() should only be called before server starts");
```

2. 框架层面保证：`HttpServer::start()` 前的所有配置操作都在主线程完成，`configure()` 不暴露给用户在运行时调用
3. 如果未来需要运行时重配置：改为原子替换 pool 指针（`shared_ptr<synchronized_pool_resource>`），而非 placement new

**经验**：placement new 原地重建是 C++ 中最危险的操作之一——它让"对象还在同一个地址"成为一种假象，实际上已经是另一个对象了。除非能从语言层面保证单线程执行，否则不应该在共享资源上使用。

---

## 坑 2：generation 缓存失效的竞争窗口

**现象**：`configure()` 后，某些线程仍然使用旧的线程本地池，分配到了已被释放的内存块。

**根因**：`getOrCreateThreadPool()` 使用 `thread_local` 缓存 + `generation_` 计数器实现池的延迟重建：

```cpp
// MemoryPool.cpp — thread_local 缓存
std::pmr::unsynchronized_pool_resource* MemoryPool::getOrCreateThreadPool()
{
    struct ThreadCache
    {
        std::pmr::unsynchronized_pool_resource* pool = nullptr;
        ThreadPoolEntry* entry = nullptr;
        uint64_t generation = 0;
    };
    thread_local ThreadCache cache;

    auto currentGen = generation_.load(std::memory_order_acquire);
    if (cache.pool != nullptr && cache.generation == currentGen)
    {
        // 缓存命中——直接返回
        // ⚠️ 但如果 configure() 在 generation_.load() 之后、return 之前执行呢？
        //    cache.pool 指向的 unsynchronized_pool_resource 的上游 globalPool_ 已被重建
        //    pool 仍然持有旧 globalPool_ 的指针——UAF
        return cache.pool;
    }

    // 缓存未命中——创建新的线程本地池
    auto pool = std::make_unique<std::pmr::unsynchronized_pool_resource>(
        /* ... */, &globalPool_);  // ← 指向当前 globalPool_
    // ...
}
```

时序问题：

```
线程 A                              主线程
────────                            ────────
load generation_ → 0                
                                    configure()
                                      globalPool_.~()    ← 析构旧池
                                      new globalPool_()  ← 构造新池
                                      generation_ → 1
cache.generation == 0 ✓             
return cache.pool                   
  → pool 的 upstream 是旧 globalPool_
  → 分配时访问已析构的对象 💥
```

**解决方案**：

Hical 的策略是**在框架层面保证时序**——`configure()` 必须在任何工作线程启动之前完成。`generation_` 计数器不是为了支持运行时重配置，而是为了让意外的延迟初始化场景能正确重建缓存。

更防御性的做法是在 `configure()` 中不只清理 `threadPools_` vector，还要同步等待所有线程的 `thread_local` 缓存失效——但这在 C++ 中没有标准机制实现（`thread_local` 变量无法从外部线程访问）。

**经验**：`thread_local` 缓存 + 原子计数器的组合看似优雅，但竞争窗口很微妙。如果 `thread_local` 缓存的数据依赖外部共享状态（如 `globalPool_`），那么缓存失效和外部状态变更之间必须是原子的——而这通常做不到。

---

## 坑 3：GC 标记了但永远不回收——死线程的内存泄漏

**现象**：长时间运行的服务中，`MemoryPool::Stats::threadPoolCount` 单调增长，内存使用量持续上升，但 `gcReclaimedPools` 也在增长——看起来 GC 在工作，但内存没有减少。

**根因**：Hical 的 GC 采用**延迟释放**策略——GC 不直接调用 `pool->release()`（因为 `unsynchronized_pool_resource` 不是线程安全的），而是设置一个 `needsRelease` 标志，等待拥有者线程在下次分配时执行释放：

```cpp
// MemoryPool.cpp — gc() 只做标记
void MemoryPool::gc(std::chrono::seconds maxIdleSeconds)
{
    auto now = std::chrono::steady_clock::now();
    size_t reclaimed = 0;

    std::lock_guard<std::mutex> lock(threadPoolsMutex_);
    for (auto& entry : threadPools_)
    {
        auto lastActive = entry->lastAllocTime.load(std::memory_order_relaxed);
        if (now - lastActive > maxIdleSeconds)
        {
            // 标记需要释放，由拥有线程在下次分配时安全执行
            entry->needsRelease.store(true, std::memory_order_release);
            ++reclaimed;  // ← 这里计数的是"标记数"，不是"实际释放数"
        }
    }
    gcReclaimedPools_.fetch_add(reclaimed, std::memory_order_relaxed);
}
```

拥有者线程在下次分配时检查标志：

```cpp
// MemoryPool.cpp — getOrCreateThreadPool() 中的延迟释放
if (cache.entry->needsRelease.exchange(false, std::memory_order_acquire))
{
    cache.pool->release();  // ← 只有此线程下次分配时才执行
}
```

**问题**：如果一个线程完成任务后退出（或长时间空闲不再分配），`needsRelease` 标志永远不会被检查——内存永远不释放。

这在 Web 服务器中很常见：连接高峰期创建了大量工作线程，高峰过后线程空闲但池仍在 `threadPools_` 中占内存。GC 每 60 秒标记一次，`reclaimed` 计数不断增长（看起来 GC 在工作），但实际释放量为零。

**解决方案**：

Hical 的缓解策略是**让 `threadPools_` vector 不无限增长**——`ThreadPoolEntry` 的所有权由 `MemoryPool` 持有，线程退出时 `thread_local` 缓存被销毁，但 `ThreadPoolEntry` 留在 vector 中。GC 标记后如果长时间未执行，可以在下次 `getOrCreateThreadPool()` 时复用这些条目（而非新建）。

更彻底的方案（尚未实现）：
- 在 GC 中，对超过 N 次标记仍未释放的条目，使用 `dispatch` 到对应 io_context 线程执行释放
- 或者改用 `synchronized_pool_resource` 作为线程本地池（牺牲性能换安全），这样 GC 可以直接跨线程释放

**经验**：`unsynchronized_pool_resource` 的性能优势（无锁）是以"只能在创建线程上操作"为代价的。GC 策略必须适配这个约束——不能简单地从 GC 线程直接释放。

---

## 坑 4：CAS 峰值更新的缓存行风暴

**现象**：16 核压测时，`TrackedResource` 的分配统计成为瓶颈。火焰图显示大量时间花在 `peakBytes_` 的 CAS 循环上。

**根因**：峰值更新使用 `compare_exchange_weak` 自旋：

```cpp
// MemoryPool.h — TrackedResource::do_allocate
void* do_allocate(size_t bytes, size_t alignment) override
{
    void* p = upstream_->allocate(bytes, alignment);
    totalAllocations_.fetch_add(1, std::memory_order_relaxed);
    auto current = currentBytes_.fetch_add(bytes, std::memory_order_relaxed) + bytes;

    // 更新峰值（无锁 CAS）
    auto peak = peakBytes_.load(std::memory_order_relaxed);
    while (current > peak &&
           !peakBytes_.compare_exchange_weak(peak, current, std::memory_order_relaxed))
    {
        // 每次 CAS 失败都重新加载 peak 并重试
    }
    return p;
}
```

问题出在高并发场景：

1. 16 个线程同时分配，`currentBytes_` 的 `fetch_add` 本身就引起缓存行弹跳
2. 如果多个线程的 `current` 都大于当前 `peak`，它们会同时竞争 CAS——但只有一个能成功，其余全部重试
3. `compare_exchange_weak` 允许虚假失败（spurious failure），进一步增加重试次数
4. 四个原子变量（`totalAllocations_`、`totalDeallocations_`、`currentBytes_`、`peakBytes_`）可能在同一个或相邻缓存行上，造成 **false sharing**

**解决方案**：

Hical 的做法是接受这个代价——统计是诊断功能，不在热路径上：

1. 全局同步池的分配频率本身就低（大部分走线程本地池），CAS 竞争不严重
2. 线程本地池直接从全局池申请大块，单次分配的 `bytes` 很大，分配频率很低
3. 如果统计开销不可接受，可以用 `thread_local` 计数器 + 定期合并替代原子计数

对于 `compare_exchange_weak` vs `compare_exchange_strong` 的选择——Hical 用 weak 是因为：
- 循环本身已经包含了重试逻辑
- weak 在某些架构（ARM）上比 strong 少一条指令
- 虚假失败只是多循环一次，代价很小

**经验**：原子变量的 CAS 自旋在低竞争下几乎免费，但在高竞争（16+ 核同时写同一缓存行）下会急剧退化。如果统计功能不需要精确实时，考虑 `thread_local` 计数器 + 合并，彻底消除共享状态。

---

## 坑 5：请求级单调池的 upstream 选错

**现象**：压测时出现低概率崩溃，堆栈指向 `synchronized_pool_resource` 的内部桶结构。

**根因**：请求级单调池（`monotonic_buffer_resource`）的 upstream 直接指向全局同步池：

```cpp
// MemoryPool.cpp — createRequestPool()
std::unique_ptr<std::pmr::monotonic_buffer_resource>
MemoryPool::createRequestPool(size_t initialSize)
{
    if (initialSize == 0)
        initialSize = config_.requestPoolInitialSize;
    return std::make_unique<std::pmr::monotonic_buffer_resource>(
        initialSize, &globalPool_);  // ← upstream 是全局同步池
}
```

`monotonic_buffer_resource` 在析构时会把所有从 upstream 申请的大块还回去。如果 upstream 是全局同步池，这个"还"操作发生在请求处理线程——而全局池在高竞争下的 deallocate 路径可能存在实现 bug。

更严重的场景：如果请求在线程 A 处理，但请求级池在线程 B 析构（比如连接被转移到另一个 io_context），那么跨线程 deallocate 到全局池的行为在某些标准库实现中不够健壮。

**解决方案**：

理想的 upstream 应该是当前线程的 `unsynchronized_pool_resource`——请求在哪个线程处理，就从哪个线程的池分配和释放：

```cpp
// 更安全的做法（概念性）
auto* threadPool = getOrCreateThreadPool();
return std::make_unique<std::pmr::monotonic_buffer_resource>(
    initialSize, threadPool);  // upstream 是线程本地池
```

但这有新的问题：`unsynchronized_pool_resource` 不是线程安全的，如果请求级池在另一个线程析构就是 UB。

Hical 的实际权衡：保持 upstream 指向全局同步池（线程安全），但确保请求级池在创建它的线程上析构。在 1:1（thread:io_context）模型下，连接绑定到固定线程，这个约束自然满足。

**经验**：PMR 的 upstream 选择是一个"安全 vs 性能"的权衡。全局同步池安全但有锁竞争，线程本地池快但不能跨线程操作。选择时必须考虑池的**析构发生在哪个线程**。

---

## 坑 6：PmrBuffer 缩容不及时导致内存膨胀

**现象**：长连接场景下，某些连接的内存占用远超实际数据量。`MemoryPool::Stats::currentBytesAllocated` 持续增长。

**根因**：Hical 的 `PmrBuffer`（网络读写缓冲区）使用 2 倍扩容策略，但缩容条件非常保守：

```cpp
// PmrBuffer.h — makeSpace 扩容策略
void makeSpace(size_t len)
{
    if (writableBytes() + prependableBytes() < len + hPrependSize)
    {
        // 扩容：优先 2 倍增长减少频繁扩容
        size_t newLen;
        if (buffer_.size() * 2 > writeIndex_ + len)
        {
            newLen = buffer_.size() * 2;
        }
        else
        {
            newLen = writeIndex_ + len;
        }
        buffer_.resize(newLen);
    }
    else
    {
        // 移动数据到前面（避免扩容）
        size_t readable = readableBytes();
        std::copy(begin() + readIndex_, begin() + writeIndex_, begin() + hPrependSize);
        readIndex_ = hPrependSize;
        writeIndex_ = readIndex_ + readable;
    }
}
```

缩容只在 `retrieveAll()` 时检查，且阈值是初始容量的 2 倍：

```cpp
// PmrBuffer.h — 保守的缩容策略
void retrieveAll()
{
    // 只在超过初始容量 2 倍时才缩容
    if (buffer_.size() > (initialCapacity_ + hPrependSize) * 2)
    {
        buffer_.resize(initialCapacity_ + hPrependSize);
    }
    readIndex_ = hPrependSize;
    writeIndex_ = hPrependSize;
}
```

**典型膨胀场景**：

```
连接建立：buffer = 4KB（初始容量）
收到大请求：buffer → 8KB → 16KB → 32KB（2 倍扩容）
大请求处理完毕：retrieveAll()
  → 32KB > 4KB * 2 ✓ → 缩容到 4KB

但如果请求体刚好 9KB：
  → 4KB → 8KB → 16KB
  → retrieveAll(): 16KB > 4KB * 2 ✓ → 缩容到 4KB  ✅

如果请求体刚好 7KB：
  → 4KB → 8KB
  → retrieveAll(): 8KB > 4KB * 2 = 8KB ❌ → 不缩容
  → 后续所有小请求（<1KB）都用 8KB buffer
```

对于长连接（WebSocket、Keep-Alive），一次偶发的大消息会永久膨胀缓冲区。

**解决方案**：

Hical 选择保守缩容是有理由的——频繁缩容会触发 `pmr::vector` 的 `resize`，这在 PMR 分配器下可能产生碎片。权衡之下，偏向"多占一点内存"而非"频繁分配释放"。

如果内存膨胀成为问题，可以：
1. 降低缩容阈值（如 1.5 倍而非 2 倍）
2. 加入"连续 N 次 retrieveAll 都低于阈值"的时间衰减策略
3. 对 WebSocket 长连接设置独立的 buffer 上限

**经验**：缓冲区的扩容容易（2 倍），缩容难（什么时候缩？缩到多少？）。在 PMR 下尤其要注意：`resize` 缩容不会真正归还内存给操作系统——它只是把块还给上游 pool，pool 可能继续持有。

---

## 坑 7：allocator 传播链断裂——PMR 白忙一场

**现象**：性能剖析显示，即使配置了 PMR 三层池，大量小对象的分配仍然走的是默认 `new/delete`。

**根因**：PMR 的 allocator 传播**不是自动的**。如果忘了在某一层传递 allocator，整条链就断了：

```cpp
// ❌ 断裂的传播链
auto& pool = getRequestPool();
std::pmr::vector<std::string> headers(&pool);
//                ^^^^^^^^^^^
// std::string 不是 pmr::string！
// headers 的 vector 本身从 pool 分配
// 但 string 的堆内存走默认 new/delete

headers.push_back("Content-Type: application/json");
// "Content-Type: application/json" 超过 SSO → new/delete 分配
```

正确的写法：

```cpp
// ✅ 完整的传播链
auto& pool = getRequestPool();
std::pmr::vector<std::pmr::string> headers(&pool);
//                ^^^^^^^^^^^^^^^^^^
// pmr::string 使用 vector 传播的 allocator

headers.emplace_back("Content-Type: application/json");
// 超过 SSO 时从 pool 分配
```

更隐蔽的断裂出现在 `boost::json` 的使用中：

```cpp
// boost::json 有自己的 pmr 支持
boost::json::monotonic_resource jsonPool;
auto obj = boost::json::parse(body, &jsonPool);
// obj 内部所有字符串都走 jsonPool ✅

// 但如果从 obj 中拷贝出来：
std::string val = obj.at("key").as_string();
//          ^^^ 普通 string，走 new/delete ❌
```

**PmrBuffer 的 swap 也受此影响**——两个使用不同 allocator 的 buffer 不能 swap：

```cpp
// PmrBuffer.h — 防御性检查
void swap(PmrBuffer& rhs)
{
    // pmr::vector::swap 在分配器不相等时行为未定义
    if (buffer_.get_allocator() != rhs.buffer_.get_allocator())
    {
        throw std::logic_error(
            "PmrBuffer::swap: cannot swap buffers with different allocators");
    }
    buffer_.swap(rhs.buffer_);
    // ...
}
```

**解决方案**：

1. **代码审查清单**：所有使用 PMR 容器的地方，检查嵌套类型是否也是 PMR 版本
2. **类型别名**：框架内部统一用 `using PmrString = std::pmr::string` 等别名，减少遗忘风险
3. **SSO 边界意识**：短字符串（通常 < 16 字节）走 SSO 不分配堆内存，PMR 不介入——这不是 bug，但会影响 benchmark 结论

**经验**：PMR 的传播链是"默认断裂"的——标准库容器不会自动把 allocator 传给嵌套类型（除非满足 `uses_allocator` 协议）。每引入一层嵌套容器，都要显式确认 allocator 传播。不确定的时候，用 `TrackedResource` 的统计检查实际分配是否走了 PMR。

---

## 总结：PMR 三层池的使用清单

| #   | 坑                  | 检查项                                                    | 严重程度 |
| --- | ------------------- | --------------------------------------------------------- | -------- |
| 1   | configure() UAF     | 是否在所有工作线程启动前完成配置？                        | 致命     |
| 2   | generation 竞争窗口 | thread_local 缓存的 upstream 是否仍然有效？               | 致命     |
| 3   | 死线程内存泄漏      | GC 标记后线程是否还在活跃？threadPools_ 是否单调增长？    | 高       |
| 4   | CAS 缓存行风暴      | TrackedResource 统计在热路径上吗？核数够多吗？            | 中       |
| 5   | upstream 选错       | 请求级池在哪个线程析构？upstream 是线程安全的吗？         | 高       |
| 6   | 缓冲区膨胀          | 长连接的 buffer 有上限吗？缩容策略够积极吗？              | 中       |
| 7   | 传播链断裂          | 嵌套容器都用 pmr 版本了吗？string 用的是 pmr::string 吗？ | 高       |

核心原则：**PMR 是优化手段，不是默认选择。先用默认 `new/delete` 把功能做对，性能剖析确认分配是瓶颈后再引入——引入后必须确保传播链完整，否则白忙一场。**

---

> **踩坑实录五部曲**：[编译差异](15-pitfall-compiler-differences.md) | [Asio 协程](16-pitfall-asio-coroutine.md) | [Boost.MySQL](17-pitfall-boost-mysql.md) | [日志系统](18-pitfall-logging-system.md) | [PMR 内存池](#)（本文）
