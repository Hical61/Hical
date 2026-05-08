# Boost.Asio 协程开发的 N 个坑（附解决方案）

## 引言

Hical 的所有异步 I/O 都基于 Boost.Asio 协程（`co_await` + `boost::asio::use_awaitable`）。路由处理器返回 `Awaitable<HttpResponse>`，中间件用洋葱模型 `co_await next(req)`，连接池用 `co_await timer.async_wait()` 做非阻塞等待。

协程消除了回调地狱，但引入了一套全新的陷阱。这篇记录的每一个坑，都是在压测或线上环境中**真实触发过**的。

---

## 目录

- [Boost.Asio 协程开发的 N 个坑（附解决方案）](#boostasio-协程开发的-n-个坑附解决方案)
  - [引言](#引言)
  - [目录](#目录)
  - [坑 1：co\_await 后 this 悬挂——对象已析构](#坑-1co_await-后-this-悬挂对象已析构)
  - [坑 2：协程异常传播——catch 里不能 co\_await](#坑-2协程异常传播catch-里不能-co_await)
  - [坑 3：steady\_timer 当协程信号量的技巧](#坑-3steady_timer-当协程信号量的技巧)
  - [坑 4：jthread vs thread——精准匹配停止信号](#坑-4jthread-vs-thread精准匹配停止信号)
  - [坑 5：多线程 io\_context + 协程的线程安全陷阱](#坑-5多线程-io_context--协程的线程安全陷阱)
  - [坑 6：detached 协程的异常黑洞](#坑-6detached-协程的异常黑洞)
  - [坑 7：io\_context::stop() 不等于安全退出](#坑-7io_contextstop-不等于安全退出)
  - [总结：协程安全编程检查清单](#总结协程安全编程检查清单)

---

## 坑 1：co_await 后 this 悬挂——对象已析构

**现象**：压测时低概率崩溃，堆栈指向 TcpServer 的 accept 循环，访问了已释放的内存。

**最小复现**：

```cpp
// ❌ 危险的写法
Awaitable<void> TcpServer::acceptLoop()
{
    while (running_)
    {
        auto socket = co_await acceptor_.async_accept(use_awaitable);
        // ⚠️ 如果在 co_await 期间 TcpServer 被析构，
        //    this 已经是悬空指针！
        this->createConnection(std::move(socket));  // 💥 use-after-free
    }
}
```

**根因**：协程帧通过 `co_spawn(io_context, coroutine, detached)` 提交到 io_context。协程帧的生命周期由 io_context 管理，与创建协程的对象**完全分离**。

```
TcpServer 对象         协程帧
  [构造] ────────────── [创建]
  [运行中]              [挂起在 co_await]
  [析构] ← 先死         [恢复] ← 后恢复
                         this → 💥 野指针
```

当某个线程析构了 TcpServer（比如 `main()` 退出），而协程帧还挂在 io_context 的等待队列里——恢复执行时 `this` 就是野指针。

**解决方案**：`shared_ptr<atomic<bool>>` 作为生命周期令牌。

```cpp
// TcpServer.h — 定义生命周期标志
std::shared_ptr<std::atomic<bool>> alive_;

// 构造时：创建标志，默认 true
alive_ = std::make_shared<std::atomic<bool>>(true);

// 析构时：置 false
alive_->store(false);
```

为什么用 `shared_ptr` 包装？因为协程帧需要持有标志的引用计数。如果 `alive_` 是 TcpServer 的普通成员，TcpServer 析构后 `alive_` 也被销毁——去读它同样是 use-after-free。`shared_ptr` 保证只要有人持有引用计数，标志本身就一直活着。

**使用模式 1——accept 循环中的双重检查**：

```cpp
while (running_.load() && alive_->load())
{
    tcp::socket socket = co_await acceptor_.async_accept(use_awaitable);

    // co_await 恢复后再次检查——这是关键
    if (!alive_->load())
    {
        break;  // TcpServer 已析构，安全退出
    }
    createConnection(std::move(socket));
}
```

**使用模式 2——连接关闭回调中的守卫**：

```cpp
auto aliveFlag = alive_;  // 闭包捕获 shared_ptr（引用计数 +1）
auto* self = this;
conn->onClose(
    [aliveFlag, self](const TcpConnection::Ptr& c)
    {
        if (aliveFlag->load())  // 先检查再访问
        {
            self->removeConnection(c);
        }
    });
```

**教训**：在协程世界里，**每个 `co_await` 都是一个生命周期断裂点**。恢复后不能假设 `this`、闭包捕获的指针、甚至栈上引用仍然有效。模式：捕获 `shared_ptr` 哨兵，恢复后先检查再使用。

---

## 坑 2：协程异常传播——catch 里不能 co_await

**现象**：编译器报错 `cannot use co_await in a catch handler`。

**最小复现**：

```cpp
// ❌ 编译错误——C++ 标准禁止
try
{
    co_await conn->execute(sql);
}
catch (...)
{
    co_await conn->rollback();  // 编译器拒绝
    throw;
}
```

**根因**：C++ 标准 [dcl.fct.def.coroutine] p6 明确禁止在 `catch` 块内使用 `co_await`。原因是 `co_await` 可能挂起并恢复协程，而 catch 块依赖于栈展开的状态——恢复后这个状态可能无效。

**解决方案**：`exception_ptr` 中转模式——catch 只捕获、不处理，异步清理放到 catch 外面。

Hical 的 DbMiddleware 是这个模式的最佳示范：

```cpp
// DbMiddleware.h — 洋葱模型的异步回滚
auto conn = co_await pool->acquire();
req.setAttribute(DbConnectionPool::hConnKey, conn);

if (opts.autoTransaction)
    co_await conn->beginTransaction();

std::exception_ptr eptr;  // 异常中转
HttpResponse res;
try
{
    res = co_await next(req);  // 执行业务逻辑
    if (opts.autoTransaction && conn->inTransaction())
        co_await conn->commit();
}
catch (...)
{
    eptr = std::current_exception();  // 只捕获，不在这里 co_await
}

// ✅ catch 外面——可以 co_await
if (eptr && conn->inTransaction())
{
    try { co_await conn->rollback(); }
    catch (...) { }  // rollback 本身失败也处理不了
}

pool->release(std::move(conn));  // 归还连接

if (eptr) std::rethrow_exception(eptr);  // 重新抛出原始异常
co_return res;
```

**流程图**：

```
co_await next(req)
    │
    ├─ 成功 → co_await commit() → 归还连接 → co_return res
    │
    └─ 异常 → eptr = current_exception()
              → co_await rollback()     ← catch 外，合法
              → 归还连接
              → rethrow_exception(eptr) ← 重新抛给上层
```

**经验**：这个模式在 Hical 中被多处使用。凡是需要"异常后异步清理"的场景，都用 `exception_ptr` 中转。

---

## 坑 3：steady_timer 当协程信号量的技巧

**现象**：数据库连接池的 `acquire()` 在连接耗尽时需要等待。第一版用 `std::condition_variable` 实现——结果阻塞了整个 io_context 线程，所有协程都卡住了。

**根因**：`condition_variable::wait()` 是**阻塞**操作。而 Asio 协程运行在 io_context 的事件循环线程上——阻塞这个线程意味着整个事件循环停转：

```
┌─────────────── io_context 线程 ────────────────┐
│ handler A → handler B → co_await → handler C   │
│                            ↓                    │
│                 cv.wait() ← 阻塞整个线程！       │
│                 handler C/D/E 全部无法执行       │
└─────────────────────────────────────────────────┘

vs.

┌─────────────── io_context 线程 ────────────────┐
│ handler A → handler B → co_await timer → ...   │
│                            ↓                    │
│                 协程挂起（线程不阻塞）            │
│                 handler C/D/E 正常执行           │
│                 timer.cancel() → 协程恢复        │
└─────────────────────────────────────────────────┘
```

**解决方案**：用 `steady_timer` 代替 `condition_variable`。核心思路：`timer.cancel()` 导致 `co_await timer.async_wait()` 返回 `operation_aborted`，从而唤醒等待的协程。

**acquire 端——挂起等待**：

```cpp
// DbConnectionPool.cpp — 池满时协程挂起
struct Waiter
{
    std::shared_ptr<boost::asio::steady_timer> timer;
    std::shared_ptr<std::shared_ptr<DbConnection>> result;  // 堆分配结果槽
};

// 创建 waiter
auto timer = std::make_shared<steady_timer>(m_ioCtx, m_config.acquireTimeout);
auto result = std::make_shared<std::shared_ptr<DbConnection>>();
m_waiters.push_back({timer, result});
lock.unlock();  // 释放锁，让其他协程可以 release

// co_await 只挂起协程，不阻塞线程
boost::system::error_code ec;
co_await timer->async_wait(redirect_error(use_awaitable, ec));

if (*result)
{
    co_return std::move(*result);  // release 已转交连接
}
throw std::runtime_error("DbConnectionPool: acquire timeout");
```

**release 端——唤醒等待者**：

```cpp
// DbConnectionPool.cpp — 连接归还时唤醒
void DbConnectionPool::release(std::shared_ptr<DbConnection> conn)
{
    std::lock_guard lock(m_mutex);

    if (!m_waiters.empty())
    {
        auto waiter = std::move(m_waiters.front());
        m_waiters.pop_front();
        *(waiter.result) = std::move(conn);  // 转交连接
        waiter.timer->cancel();              // cancel 唤醒 co_await
        return;
    }
    // 无等待者，归入空闲池
    m_idle.push_back(std::move(conn));
}
```

**为什么结果要用 `shared_ptr<shared_ptr<DbConnection>>` 两层指针？**

因为 `release()` 在 cancel timer 之前就要写入结果。如果 result 是协程帧里的局部变量，release 线程写入时协程帧可能还没被调度，局部变量的地址不稳定。堆分配的 `shared_ptr` 让 result 的生命周期独立于协程帧。

**对比**：

| 方案                                 | 挂起协程 | 阻塞线程 | 唤醒机制       |
| ------------------------------------ | -------- | -------- | -------------- |
| `condition_variable`                 | ❌        | ✅ 阻塞   | `notify_one()` |
| `steady_timer`                       | ✅        | ❌ 不阻塞 | `cancel()`     |
| `boost::asio::experimental::channel` | ✅        | ❌        | 内置           |

**经验**：在 Asio 协程中，**任何阻塞操作都是错误的**。`mutex::lock()`、`condition_variable::wait()`、`future::get()` 都会冻结事件循环。所有同步原语都要替换为异步等价物。

---

## 坑 4：jthread vs thread——精准匹配停止信号

**现象**：不是 bug，但经常被问到——为什么 Hical 只有 `AsyncFileSink` 用了 `std::jthread`，其他地方全部用 `std::thread`？

**分析**：关键在于停止信号的来源。

**AsyncFileSink——需要 `stop_token`**：

后台写盘线程在循环中等待数据到达。停止时需要：

1. 唤醒正在等待的线程
2. 让线程处理完剩余数据
3. 线程安全地退出

`jthread` + `stop_token` 完美匹配：

```cpp
// AsyncFileSink.cpp
m_bgThread = std::jthread(
    [this](std::stop_token stopToken)  // 接收 stop_token
    {
        backgroundLoop(std::move(stopToken));
    });

void AsyncFileSink::backgroundLoop(std::stop_token stopToken)
{
    while (true)
    {
        std::unique_lock<std::mutex> lock(m_bufMutex);

        // stop_token 直接集成到 condition_variable_any
        // 停止请求到来时自动唤醒——无需额外 notify
        m_cond.wait_for(lock,
                        stopToken,            // ← 关键
                        m_opts.flushInterval,
                        [this]() { return !m_curBuf.empty(); });

        if (stopToken.stop_requested() && m_curBuf.empty())
            break;  // 数据处理完毕，优雅退出

        if (!m_curBuf.empty())
        {
            m_curBuf.swap(m_flushBuf);  // 双缓冲交换
            m_curBuf.clear();
        }
        // ... 锁外写盘 ...
    }
    // 关闭前排空残余数据
    // ...
}

// 析构函数什么都不用做——jthread 自动 request_stop() + join()
```

**EventLoopPool——不需要 `stop_token`**：

io_context 工作线程的停止信号来自 `io_context::stop()`，调用后 `run()` 自然返回。`stop_token` 是多余的：

```cpp
// EventLoopPool.cpp
void EventLoopPool::start()
{
    for (auto& loop : loops_)
    {
        auto* ptr = loop.get();
        threads_.emplace_back([ptr]() {
            ptr->run();  // io_context::run()
        });
    }
}

void EventLoopPool::stop()
{
    for (auto& loop : loops_)
        loop->stop();                          // io_context::stop()
    for (auto& thread : threads_)
        if (thread.joinable()) thread.join();  // 显式 join
    threads_.clear();
}
```

**决策矩阵**：

| 条件                                               | 选择                                 |
| -------------------------------------------------- | ------------------------------------ |
| 线程自己需要接收停止信号（`stop_token`）           | `std::jthread`                       |
| 停止信号来自外部机制（io_context::stop、原子标志） | `std::thread` + 显式 join            |
| 不确定                                             | 倾向 `std::thread`——避免传递错误语义 |

**经验**：不要无脑把所有 `thread` 换成 `jthread`。如果不用 `stop_token`，`jthread` 只是多了个自动 join——但会给读代码的人传递错误信号（"这个线程应该用 stop_token 停止"），增加理解成本。

---

## 坑 5：多线程 io_context + 协程的线程安全陷阱

**现象**：Hical 使用 1 thread : 1 io_context 模型（EventLoopPool），每个连接绑定到一个固定的 io_context。但在共享状态访问时仍然出现了竞争。

**架构**：

```
EventLoopPool
  ├─ io_context[0] + thread[0] ── conn_a, conn_b
  ├─ io_context[1] + thread[1] ── conn_c, conn_d
  └─ io_context[2] + thread[2] ── conn_e, conn_f

Round-Robin 分发：新连接依次分配到 io_context[0], [1], [2], [0], ...
```

每个连接只在自己绑定的 io_context 线程上执行所有操作——读、写、关闭。看起来不需要锁？

**问题**：跨连接的共享状态（路由表、中间件链、连接集合）仍然可能被多个线程同时访问：

```
io_context[0] ─ conn_a ─┐
io_context[1] ─ conn_b ─┤── 都要查路由表 → Router（只读✅）
io_context[2] ─ conn_c ─┘
                         ├── 都要操作连接集合 → connections_（需要保护⚠️）
```

**解决方案——分层保护**：

| 共享状态                | 访问模式                           | 保护策略                |
| ----------------------- | ---------------------------------- | ----------------------- |
| Router 路由表           | 启动前写入，运行时只读             | 无需锁                  |
| MiddlewarePipeline      | `build()` 预构建后缓存，运行时只读 | 无需锁                  |
| TcpServer::connections_ | 运行时增删                         | dispatch 到 accept 线程 |
| Logger::m_sinks         | 运行时可能 addSink                 | COW（读无锁）           |

连接集合的 dispatch 串行化：

```cpp
// TcpServer.cpp — 连接移除必须在 accept 线程
void TcpServer::removeConnection(const TcpConnection::Ptr& conn)
{
    baseLoop_->dispatch([this, conn]()
    {
        connections_.erase(conn);  // 在 accept 线程执行，无竞争
    });
}
```

中间件链的 `build()` 预构建——运行时零分配零锁：

```cpp
// Middleware.cpp — 从后向前构建洋葱链
MiddlewareNext MiddlewarePipeline::buildChainFrom(
    const std::vector<MiddlewareHandler>& middlewares,
    MiddlewareNext finalHandler)
{
    MiddlewareNext current = std::move(finalHandler);
    for (int i = static_cast<int>(middlewares.size()) - 1; i >= 0; --i)
    {
        auto mw = middlewares[i];
        current = [mw = std::move(mw), next = std::move(current)]
                  (HttpRequest& r) -> Awaitable<HttpResponse>
        {
            co_return co_await mw(r, next);
        };
    }
    return current;
}

// 启动时一次性构建，运行时直接调用缓存的链
pipeline.build(routerHandler);

// 运行时——无锁执行
auto response = co_await pipeline.execute(req);
```

**经验**：1:1 模型不等于完全无锁。共享状态仍需保护，但策略可以更轻量——能用"启动前初始化 + 运行时只读"就不用锁，能用 dispatch 串行化就不用 mutex。

---

## 坑 6：detached 协程的异常黑洞

**现象**：一个协程里的异常被静默吞掉，没有任何日志输出，问题排查了很久。

**根因**：`boost::asio::detached` 意味着"不关心结果"——包括异常。未捕获的异常在 detached 协程中行为不一致（不同 Asio 版本和编译器有差异），最坏情况是直接 `std::terminate()`，最好情况是静默吞掉。

```cpp
// ❌ 异常黑洞
boost::asio::co_spawn(io_ctx,
    []() -> Awaitable<void>
    {
        throw std::runtime_error("oops");  // 去哪了？
    },
    boost::asio::detached);
```

**解决方案**：

```cpp
// ✅ 始终提供异常处理器
boost::asio::co_spawn(io_ctx,
    []() -> Awaitable<void>
    {
        throw std::runtime_error("oops");
    },
    [](std::exception_ptr eptr)
    {
        if (eptr)
        {
            try { std::rethrow_exception(eptr); }
            catch (const std::exception& e) {
                HICAL_LOG_ERROR("coroutine failed: {}", e.what());
            }
        }
    });

// 或者：协程内部自行 try-catch 所有异常
boost::asio::co_spawn(io_ctx,
    []() -> Awaitable<void>
    {
        try
        {
            co_await riskyOperation();
        }
        catch (const std::exception& e)
        {
            HICAL_LOG_ERROR("operation failed: {}", e.what());
        }
    },
    boost::asio::detached);  // 协程内部已处理，detached 安全
```

**Hical 的策略**：
- 对"不应该抛异常"的协程（如 accept 循环）：协程内部 try-catch 所有异常
- 对"可能抛异常"的协程（如健康检查、idle 检测）：提供显式异常处理器

---

## 坑 7：io_context::stop() 不等于安全退出

**现象**：调用 `io_context::stop()` 后，某些资源没有被正确清理，导致析构时访问已释放的内存。

**根因**：`stop()` 只是设置标志让 `run()` 返回。但：

1. **正在执行的 handler 会跑完**——stop 不是中断
2. **挂起的协程不会被通知**——它们还在等 I/O 完成
3. **pending 的 async 操作不会被 cancel**——需要显式 cancel

```
io_context::stop()
    │
    ├─ 正在执行的 handler → 继续执行到结束 ← 可能访问即将析构的对象
    ├─ 挂起的协程 → 仍在等待 I/O ← 需要 cancel socket/timer
    └─ run() → 返回
```

**解决方案**：stop 之前先 cancel 所有活跃资源：

```cpp
// 正确的关闭顺序
void TcpServer::stop()
{
    running_.store(false);

    // 1. 关闭 acceptor（取消 pending accept）
    boost::system::error_code ec;
    acceptor_.close(ec);

    // 2. 关闭所有活跃连接（取消 pending read/write）
    for (auto& conn : connections_)
    {
        conn->close();
    }
    connections_.clear();

    // 3. 取消所有 timer
    // ...

    // 4. 最后停止 io_context
    for (auto& loop : loops_)
    {
        loop->stop();
    }
}
```

**经验**：`io_context::stop()` 只是"告诉 run() 别再等了"，不是"安全关闭所有资源"。正确的关闭流程是：先 cancel 所有 I/O 操作，再 stop io_context，最后 join 线程。

---

## 总结：协程安全编程检查清单

| #   | 规则                                                      | 原因                           |
| --- | --------------------------------------------------------- | ------------------------------ |
| 1   | 每个 `co_await` 后检查对象存活性                          | 协程帧生命周期独立于对象       |
| 2   | catch 里不 co_await，用 `exception_ptr` 中转              | C++ 标准限制                   |
| 3   | 不在协程中使用阻塞同步原语                                | 会冻结整个 io_context 事件循环 |
| 4   | `co_spawn` 始终提供异常处理器                             | `detached` 的异常行为不可靠    |
| 5   | stop io_context 前先 cancel 所有 I/O                      | stop 不会主动 cancel           |
| 6   | 共享状态用 dispatch 串行化或 COW                          | 1:1 模型不等于无锁             |
| 7   | jthread 用于 stop_token 场景，thread 用于 io_context 场景 | 精准匹配停止信号               |

核心原则：**在协程世界里，每个 `co_await` 都是一个时间旅行门——恢复时世界可能已经变了。不要假设任何状态在 `co_await` 前后保持一致。**

---

> **踩坑实录五部曲**：[编译差异](15-pitfall-compiler-differences.md) | [Asio 协程](#)（本文） | [Boost.MySQL](17-pitfall-boost-mysql.md) | [日志系统](18-pitfall-logging-system.md) | [PMR 内存池](19-pitfall-pmr-memory-pool.md)
