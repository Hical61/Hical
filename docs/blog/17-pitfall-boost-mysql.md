# Boost.MySQL 协程集成：连接池开发的 5 个坑

## 引言

Hical 的数据库模块（`src/db/`）是一个基于协程的连接池 + 中间件系统，后端使用 Boost.MySQL 的 `any_connection`。从 "能跑" 到 "能在生产环境跑"，中间踩了不少坑。

这篇文章记录了 Boost.MySQL 协程集成过程中遇到的 5 个真实问题，每个都附带完整的解决方案代码。

---

## 目录

- [Boost.MySQL 协程集成：连接池开发的 5 个坑](#boostmysql-协程集成连接池开发的-5-个坑)
  - [引言](#引言)
  - [目录](#目录)
  - [坑 1：any\_connection vs 强类型连接的取舍](#坑-1any_connection-vs-强类型连接的取舍)
  - [坑 2：PreparedStatement 失效与自动重试](#坑-2preparedstatement-失效与自动重试)
  - [坑 3：SET NAMES 注入风险——validateCharset 白名单](#坑-3set-names-注入风险validatecharset-白名单)
  - [坑 4：连接池 acquire 超时的竞争窗口](#坑-4连接池-acquire-超时的竞争窗口)
  - [坑 5：事务忘记 commit/rollback 的自动回滚设计](#坑-5事务忘记-commitrollback-的自动回滚设计)
    - [第一道防线：DbMiddleware 洋葱模型](#第一道防线dbmiddleware-洋葱模型)
    - [第二道防线：连接池 release 兜底回滚](#第二道防线连接池-release-兜底回滚)
  - [附：StmtCache LRU 缓存设计](#附stmtcache-lru-缓存设计)
  - [总结：Boost.MySQL 集成检查清单](#总结boostmysql-集成检查清单)

---

## 坑 1：any_connection vs 强类型连接的取舍

**现象**：第一版连接池用 Boost.MySQL 的强类型连接（`tcp_ssl_connection`），结果泛型代码全部被迫模板化——编译时间爆炸，且无法在运行时根据配置切换 TCP/SSL。

**强类型方式的问题**：

```cpp
// ❌ 强类型——泛型代码必须模板化
template <typename Connection>
class DbPool
{
    std::vector<std::unique_ptr<Connection>> idle_;
    // Connection 是 tcp_connection 还是 tcp_ssl_connection？
    // 中间件也要模板化、查询日志也要模板化...
};

// 编译时决定，运行时无法切换
using Pool = DbPool<boost::mysql::tcp_ssl_connection>;
```

**解决方案**：`any_connection`——类型擦除，运行时决定传输层：

```cpp
// MysqlConnection.h — 使用 any_connection
class MysqlConnection : public DbConnection
{
    boost::mysql::any_connection m_conn;  // 类型擦除
    StmtCache m_stmtCache;
    // ...
};

// 创建时根据配置决定 TCP 或 SSL
Awaitable<std::shared_ptr<MysqlConnection>> MysqlConnection::create(
    boost::asio::any_io_executor exec, const DbConfig& config)
{
    auto conn = std::make_shared<MysqlConnection>(exec, config.stmtCacheSize);

    auto params = boost::mysql::connect_params{};
    params.server_address =
        boost::mysql::host_and_port{config.host, static_cast<unsigned short>(config.port)};
    params.username = config.user;
    params.password = config.password;
    params.database = config.database;
    params.ssl = boost::mysql::ssl_mode::require;  // 运行时配置

    co_await conn->m_conn.async_connect(params, boost::asio::use_awaitable);

    // 设置字符集
    if (!config.charset.empty())
    {
        validateCharset(config.charset);
        co_await conn->m_conn.async_execute(
            "SET NAMES '" + config.charset + "'", results, use_awaitable);
    }

    co_return conn;
}
```

**代价**：`any_connection` 内部有一次虚函数调用开销。但对于数据库 I/O 来说完全可以忽略——一次网络往返 0.5-5ms，虚调用 < 1ns。

**额外好处**：`any_connection` 支持 `reconnect()`，对连接池的健康检查和重连逻辑更友好。

---

## 坑 2：PreparedStatement 失效与自动重试

**现象**：服务运行一段时间后，某些参数化查询突然报 `statement not found`，重启后恢复。

**根因**：MySQL 服务器在以下情况下会使 PreparedStatement 失效：

| 触发条件                    | 说明                                                |
| --------------------------- | --------------------------------------------------- |
| 服务器重启                  | 所有 statement 失效                                 |
| ALTER TABLE                 | 涉及的表的 statement 失效                           |
| FLUSH TABLES                | 所有 statement 失效                                 |
| 连接空闲超过 `wait_timeout` | 虽然连接通过 ping 保活，但 statement 句柄可能已失效 |

Hical 使用 per-connection LRU 缓存（`StmtCache`），缓存的 statement 可能已在服务器端失效，但客户端不知道——直到执行时才报错。

**解决方案**：catch-retry 模式——失败后从缓存移除、重新 prepare、再执行一次：

```cpp
// MysqlConnection.cpp — 参数化查询的完整流程
Awaitable<DbResult> MysqlConnection::query(
    std::string_view sql, std::span<const std::string> params)
{
    // 1. 从 LRU 缓存获取或 prepare
    auto stmt = co_await getOrPrepare(sql);

    // 2. 构建参数
    std::vector<boost::mysql::field_view> fields;
    fields.reserve(params.size());
    for (const auto& p : params)
        fields.emplace_back(p);

    // 3. 执行（可能因 statement 失效而失败）
    boost::mysql::results boostResults;
    bool needRetry = false;
    try
    {
        co_await m_conn.async_execute(
            stmt.bind(fields.begin(), fields.end()),
            boostResults,
            boost::asio::use_awaitable);
    }
    catch (...)
    {
        m_stmtCache.erase(sql);  // 从缓存移除失效的 statement
        needRetry = true;
    }

    // 4. 重试（在 catch 外——允许 co_await）
    if (needRetry)
    {
        auto freshStmt = co_await m_conn.async_prepare_statement(
            std::string(sql), boost::asio::use_awaitable);

        co_await m_conn.async_execute(
            freshStmt.bind(fields.begin(), fields.end()),
            boostResults,
            boost::asio::use_awaitable);

        // 5. 重试成功，放回缓存
        auto evicted = m_stmtCache.insert(std::string(sql), std::move(freshStmt));
        if (evicted)
        {
            // 被驱逐的旧 statement 异步关闭（释放服务器资源）
            try
            {
                co_await m_conn.async_close_statement(*evicted, use_awaitable);
            }
            catch (...) { }
        }
    }

    touch();
    co_return convertResults(boostResults);
}
```

**关键细节**：

1. retry 代码在 `catch` 外面——因为需要 `co_await`（C++ 标准禁止 catch 内 co_await）
2. 只重试一次——如果重新 prepare 也失败，说明是更严重的问题（连接断开），直接抛给上层
3. 被驱逐的 statement 需要异步关闭——释放 MySQL 服务器端的 prepared statement 句柄

---

## 坑 3：SET NAMES 注入风险——validateCharset 白名单

**现象**：代码审查时发现，连接初始化的 `SET NAMES` 拼接了用户配置的 charset 字符串，存在 SQL 注入风险。

**最小复现**：

```cpp
// ❌ 危险：charset 来自配置文件
std::string sql = "SET NAMES '" + config.charset + "'";
co_await conn.async_execute(sql, results, use_awaitable);

// 如果 charset 被恶意设置为：
// utf8'; DROP TABLE users; --
// 拼接后：SET NAMES 'utf8'; DROP TABLE users; --'
```

虽然 `SET NAMES` 不像 SELECT 那样能泄露数据，但恶意值仍可能导致连接状态异常或执行意外命令。

**为什么不用参数化查询？**

```cpp
// ❌ 这样不行——SET NAMES 的参数是标识符，不是值
co_await conn.async_execute(
    conn.prepare_statement("SET NAMES ?").bind("utf8"),
    results, use_awaitable);
// MySQL 报错：参数化查询不能绑定标识符
```

MySQL 的参数化查询只能绑定**值**（字符串、数字），不能绑定**标识符**（表名、列名、字符集名）。所以 `SET NAMES` 只能用字符串拼接。

**解决方案**：白名单校验——只允许合法字符：

```cpp
// MysqlConnection.cpp — 白名单校验
void MysqlConnection::validateCharset(const std::string& charset)
{
    // 白名单：仅允许字母、数字、下划线
    // 此校验与 create() 中的 "SET NAMES '" + charset + "'" 拼接强耦合，
    // 修改此校验规则前必须评估 SQL 注入风险。
    for (char c : charset)
    {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_')
        {
            throw std::invalid_argument(
                "DbConfig: invalid charset '" + charset + "'");
        }
    }
}

// 使用时——先校验再拼接
if (!config.charset.empty())
{
    validateCharset(config.charset);  // 不通过直接抛异常
    std::string sql = "SET NAMES '" + config.charset + "'";
    co_await conn->m_conn.async_execute(sql, result, use_awaitable);
}
```

**为什么注释要强调"强耦合"？**

因为校验函数和拼接代码在不同位置。如果未来有人修改了拼接方式（比如去掉了单引号包裹），白名单可能不再足够。注释明确标注了耦合关系，提醒维护者：**改一个必须检查另一个**。

**经验**：凡是不能参数化的 SQL 拼接（表名、列名、SET 命令），都必须做白名单校验。注释中标明校验与拼接的耦合关系。

---

## 坑 4：连接池 acquire 超时的竞争窗口

**现象**：极端高并发下，acquire 偶尔返回 nullptr 或抛出预期外的异常。

**背景**：连接池的 acquire 使用 `steady_timer` 做协程信号量（见前一篇文章的坑 3）。acquire 超时后需要从等待队列 `m_waiters` 中移除自己——但 release 可能恰好在同一瞬间 pop 了它。

**竞争时序**：

```
时间线：
t1: acquire 超时，co_await 返回
t2: release 被调用，pop m_waiters.front()，cancel timer，写入 result
t3: acquire 尝试从 m_waiters 移除自己——但自己已经被 pop 了

如果 t2 和 t3 几乎同时发生：
  - acquire 在队列里找不到自己（被 release pop 了）
  - 但 result 里已经有连接了
  - 如果不检查 result，这个连接就泄漏了
```

**解决方案**：double-check 模式：

```cpp
// DbConnectionPool.cpp — 超时后的 double-check
co_await timer->async_wait(redirect_error(use_awaitable, ec));

if (*result)
{
    // 正常路径：release 已转交连接
    co_return std::move(*result);
}

// 超时路径：尝试从等待队列移除自己
bool removed = false;
{
    std::lock_guard cleanLock(m_mutex);
    auto before = m_waiters.size();
    std::erase_if(m_waiters,
                  [&timer](const Waiter& w) { return w.timer == timer; });
    removed = (m_waiters.size() < before);
}

// ⚠️ 关键的 double-check：
// 如果自己不在队列里了（被 release pop 了），
// 检查是否已经收到了连接
if (!removed && *result)
{
    // release 在我们检查之前已经转交了连接
    co_return std::move(*result);
}

// 真的超时了
throw std::runtime_error("DbConnectionPool: acquire timeout");
```

**为什么 result 要用 `shared_ptr<shared_ptr<DbConnection>>`？**

这两层指针看起来丑陋，但解决了一个关键问题：

```
release 线程：  *(waiter.result) = conn;  timer->cancel();
acquire 协程：  co_await timer; ... *result?

release 写 result 和 cancel timer 不是原子的。
如果 result 是协程帧里的局部变量，release 在 cancel 之前写入时，
协程帧可能还没被调度——局部变量的地址可能不稳定（被优化器复用）。

堆分配的 shared_ptr：生命周期独立于协程帧，release 随时可以安全写入。
```

---

## 坑 5：事务忘记 commit/rollback 的自动回滚设计

**现象**：业务代码在异常路径上忘记 rollback，连接带着未完成的事务被归还到池中。下一个使用该连接的请求看到了上一个请求的脏数据。

**影响**：

```
请求 A：BEGIN → INSERT → 异常退出（忘记 rollback）→ release(conn)
请求 B：acquire(conn) → SELECT → 看到了请求 A 未提交的 INSERT ← 脏读
```

**解决方案**：**两道防线**确保事务不会泄漏。

### 第一道防线：DbMiddleware 洋葱模型

使用中间件自动管理事务生命周期——业务代码不需要手动 begin/commit/rollback：

```cpp
// DbMiddleware.h — 自动事务管理
inline MiddlewareHandler makeDbMiddleware(
    std::shared_ptr<DbConnectionPool> pool, DbMiddlewareOptions opts = {})
{
    return [pool, opts](HttpRequest& req, MiddlewareNext next) -> Awaitable<HttpResponse>
    {
        // 前置：获取连接
        auto conn = co_await pool->acquire();
        req.setAttribute(DbConnectionPool::hConnKey, conn);

        // 自动事务：开启
        if (opts.autoTransaction)
            co_await conn->beginTransaction();

        std::exception_ptr eptr;
        HttpResponse res;
        try
        {
            // 执行业务逻辑
            res = co_await next(req);

            // 后置：提交
            if (opts.autoTransaction && conn->inTransaction())
                co_await conn->commit();
        }
        catch (...)
        {
            eptr = std::current_exception();
        }

        // 异常时回滚（catch 外，允许 co_await）
        if (eptr && conn->inTransaction())
        {
            try { co_await conn->rollback(); }
            catch (...) { }
        }

        // 归还连接
        pool->release(std::move(conn));

        if (eptr) std::rethrow_exception(eptr);
        co_return res;
    };
}
```

### 第二道防线：连接池 release 兜底回滚

即使业务代码绕过了中间件直接使用连接池，release 时也会检查事务状态：

```cpp
// DbConnectionPool.cpp — release 时的事务兜底
void DbConnectionPool::release(std::shared_ptr<DbConnection> conn)
{
    if (!conn) return;

    if (conn->inTransaction())
    {
        // 残留事务——异步回滚
        // 注意：不在此处减 activeCount，避免计数低估
        auto self = shared_from_this();
        boost::asio::co_spawn(m_ioCtx,
            [self, connPtr = std::move(conn)]() mutable -> Awaitable<void>
            {
                try
                {
                    co_await connPtr->rollback();
                }
                catch (...)
                {
                    // 回滚失败——连接不可复用，直接丢弃
                    std::lock_guard lock(self->m_mutex);
                    if (self->m_activeCount > 0) --self->m_activeCount;
                    co_return;  // 连接离开作用域时析构
                }

                // 回滚成功——归入空闲池或转交等待者
                connPtr->touch();
                std::lock_guard lock(self->m_mutex);
                if (self->m_activeCount > 0) --self->m_activeCount;

                if (!self->m_waiters.empty())
                    self->wakeOneWaiter(std::move(connPtr));
                else
                    self->m_idle.push_back(std::move(connPtr));
            },
            [](std::exception_ptr) {});
        return;
    }

    // 无事务——正常归还
    std::lock_guard lock(m_mutex);
    if (m_activeCount > 0) --m_activeCount;
    // ... 转交等待者或归入空闲池 ...
}
```

**为什么不在 release 里同步回滚？**

因为 `rollback()` 是异步操作（需要 `co_await`），而 `release()` 不是协程。所以用 `co_spawn` 启动一个独立的回滚协程。

**为什么回滚期间不减 activeCount？**

```
总连接数 = activeCount + idle.size()

如果回滚期间就减了 activeCount：
  总连接数 < 实际连接数
  acquire 可能会创建超出 maxConnections 的连接
  
所以要等回滚完成（成功或失败）后再减。
```

**两道防线的关系**：

```
请求 → DbMiddleware → 业务代码
         │                  │
         │ 正常返回 → commit │
         │ 异常 → rollback   │ ← 第一道防线
         │                  │
         └─ release(conn)   │
               │            
          inTransaction()? ─┤
               │            │
           yes → co_spawn   │
                rollback    │ ← 第二道防线（兜底）
```

正常情况下第一道防线就能处理，第二道防线是保险——防止业务代码直接调用 `pool->release()` 而忘记 rollback 的情况。

---

## 附：StmtCache LRU 缓存设计

Hical 的 PreparedStatement 缓存使用经典的 `list + unordered_map` LRU 结构：

```cpp
// StmtCache.h — 数据结构
class StmtCache
{
    size_t m_maxSize;

    // 双向链表：front = MRU（最近使用），back = LRU（最久未使用）
    using LruEntry = std::pair<std::string, boost::mysql::statement>;
    std::list<LruEntry> m_lruList;

    // 哈希表：SQL → 链表迭代器
    // 透明哈希：支持 string_view 查找，避免构造临时 string
    std::unordered_map<std::string,
                       std::list<LruEntry>::iterator,
                       StringHash, StringEqual> m_map;
};
```

**find——命中时提升到 MRU**：

```cpp
boost::mysql::statement* StmtCache::find(std::string_view sql)
{
    if (m_maxSize == 0) return nullptr;  // 缓存禁用

    auto it = m_map.find(sql);  // string_view 零分配查找
    if (it == m_map.end()) return nullptr;

    // splice 到链表头部（MRU）——O(1)
    m_lruList.splice(m_lruList.begin(), m_lruList, it->second);
    return &(it->second->second);
}
```

**insert——满时驱逐 LRU，返回被驱逐的 statement 让调用方异步关闭**：

```cpp
std::optional<boost::mysql::statement> StmtCache::insert(
    const std::string& sql, boost::mysql::statement stmt)
{
    if (m_maxSize == 0) return stmt;  // 缓存禁用，直接返回让调用方关闭

    std::optional<boost::mysql::statement> evicted;

    // 已存在——更新并提升
    auto it = m_map.find(sql);
    if (it != m_map.end())
    {
        it->second->second = std::move(stmt);
        m_lruList.splice(m_lruList.begin(), m_lruList, it->second);
        return evicted;  // nullopt
    }

    // 缓存满——驱逐 LRU（链表尾部）
    if (m_lruList.size() >= m_maxSize)
    {
        auto& back = m_lruList.back();
        evicted = std::move(back.second);  // 返回被驱逐的 statement
        m_map.erase(back.first);
        m_lruList.pop_back();
    }

    // 插入到 MRU 位置
    m_lruList.emplace_front(sql, std::move(stmt));
    m_map[sql] = m_lruList.begin();

    return evicted;  // 调用方负责 async_close_statement
}
```

**设计决策**：

| 决策               | 理由                                               |
| ------------------ | -------------------------------------------------- |
| 非线程安全         | 每个 MysqlConnection 独占一个 StmtCache，无需锁    |
| 驱逐返回 statement | 关闭 statement 需要 async I/O，不能在 cache 内部做 |
| 透明哈希           | `find(string_view)` 避免构造临时 `std::string`     |
| maxSize=0 禁用模式 | 配置灵活，不需要条件编译                           |

---

## 总结：Boost.MySQL 集成检查清单

| #   | 规则                                     | 原因                                   |
| --- | ---------------------------------------- | -------------------------------------- |
| 1   | 用 `any_connection` 而非强类型连接       | 避免泛型代码模板化爆炸                 |
| 2   | PreparedStatement 执行失败时 catch-retry | 服务器可能已使 statement 失效          |
| 3   | 不能参数化的 SQL 拼接做白名单校验        | 防止 SQL 注入                          |
| 4   | 连接池用 `steady_timer` 做协程信号量     | `condition_variable` 会阻塞 io_context |
| 5   | acquire 超时后 double-check result       | release 和超时可能同时发生             |
| 6   | 事务兜底回滚：中间件 + 连接池双重保险    | 防止事务泄漏                           |
| 7   | 被驱逐的 statement 由调用方异步关闭      | cache 内部不能做 async I/O             |
| 8   | charset 白名单与 SQL 拼接标注耦合关系    | 防止维护时改一个忘另一个               |

核心原则：**数据库连接是昂贵的共享资源——获取要高效（LRU + LIFO），归还要安全（自动回滚），失败要优雅（重试 + 降级）。**

---

> **踩坑实录五部曲**：[编译差异](15-pitfall-compiler-differences.md) | [Asio 协程](16-pitfall-asio-coroutine.md) | [Boost.MySQL](#)（本文） | [日志系统](18-pitfall-logging-system.md) | [PMR 内存池](19-pitfall-pmr-memory-pool.md)
