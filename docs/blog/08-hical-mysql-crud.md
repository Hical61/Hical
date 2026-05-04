# 用 Hical + MySQL 5 分钟搭建 CRUD API（C++20 协程版）

> C++ 访问数据库难吗？2026 年不再难了。本文用 Hical 的协程 DB 中间件，带你从零搭建一个完整的 MySQL CRUD API —— 连接池管理、事务自动提交/回滚、慢查询检测，全部开箱即用，代码比大多数 Python 教程还简洁。

---

## 三种姿势对比

| 方式             | 代码量             | 连接池 | 异步              | 防注入            |
| ---------------- | ------------------ | ------ | ----------------- | ----------------- |
| 裸 `mysql_query` | 多，手动管理连接   | 手写   | 阻塞              | 手拼字符串，危险  |
| ORM（如 ODB）    | 少，但有运行时膨胀 | 内置   | 视实现而定        | 安全              |
| Hical 协程中间件 | 少，原生协程       | 内置   | 非阻塞 `co_await` | PreparedStatement |

Hical 走第三条路：连接池是协程化的，查询全部走 PreparedStatement 防注入，事务在中间件层自动管理，业务代码只关心 SQL 逻辑。

---

## 环境准备

### 构建启用数据库支持

```bash
# Linux / macOS
cmake -B build -DHICAL_WITH_DATABASE=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)

# Windows (MSYS2 MINGW64)
cmake -B build -G Ninja -DHICAL_WITH_DATABASE=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build

# Windows (MSVC + vcpkg)
cmake -B build -DHICAL_WITH_DATABASE=ON -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
```

### 依赖说明

- **Boost >= 1.85**（DB 中间件需要 Boost.MySQL，1.85 版引入 `any_connection`）
- **OpenSSL**（MySQL TLS 连接可选，已是框架强依赖）
- CMakeLists 里加一行即可：

```cmake
target_link_libraries(my_app PRIVATE hical::hical_core)
# HICAL_WITH_DATABASE=ON 时 hical_core 自动链接 Boost.MySQL
```

---

## 建表 SQL

```sql
CREATE DATABASE IF NOT EXISTS demo CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;

USE demo;

CREATE TABLE IF NOT EXISTS users (
    id         INT UNSIGNED NOT NULL AUTO_INCREMENT,
    name       VARCHAR(64)  NOT NULL,
    email      VARCHAR(128) NOT NULL UNIQUE,
    created_at DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
```

---

## 完整 main.cpp

约 80 行，包含连接池初始化、中间件注册、4 个 CRUD 路由：

```cpp
#include "core/HttpServer.h"
#include "core/Log.h"
#include "db/DbConfig.h"
#include "db/DbConnectionPool.h"
#include "db/DbMiddleware.h"
#include "db/MysqlConnection.h"

using namespace hical;
using namespace hical::db;

// DTO：用于 POST /users 的请求体
struct CreateUserReq
{
    std::string name;
    std::string email;

    HICAL_JSON(CreateUserReq, name, REQUIRED(email))
};

int main()
{
    // 1. 配置数据库连接
    DbConfig dbCfg;
    dbCfg.host        = "127.0.0.1";
    dbCfg.port        = 3306;
    dbCfg.user        = "root";
    dbCfg.password    = "yourpassword";
    dbCfg.database    = "demo";
    dbCfg.minConnections = 2;
    dbCfg.maxConnections = 16;

    // 2. 创建连接池（工厂函数由 MysqlConnection::makeFactory() 提供）
    HttpServer server(8080);
    auto pool = std::make_shared<DbConnectionPool>(
        server.ioContext(), dbCfg, MysqlConnection::makeFactory());

    // 3. 注册 DB 中间件：autoTransaction=true，每个请求自动开启/提交/回滚事务
    server.use(makeDbMiddleware(pool, DbMiddlewareOptions{
        .autoTransaction = true,
        .injectPool      = true,
    }));

    // ── GET /users ──────────────────────────────────────────────────
    server.router().get("/users",
        [](const HttpRequest& req) -> Awaitable<HttpResponse>
        {
            auto conn   = getDbConnection(req);
            auto result = co_await conn->query("SELECT id,name,email,created_at FROM users", {});

            boost::json::array arr;
            for (const auto& row : result.rows)
            {
                arr.push_back(boost::json::object{
                    {"id",         row[0]},
                    {"name",       row[1]},
                    {"email",      row[2]},
                    {"created_at", row[3]},
                });
            }
            co_return HttpResponse::json(arr);
        });

    // ── GET /users/{id} ─────────────────────────────────────────────
    server.router().get("/users/{id}",
        [](const HttpRequest& req) -> Awaitable<HttpResponse>
        {
            auto conn   = getDbConnection(req);
            auto result = co_await conn->query(
                "SELECT id,name,email,created_at FROM users WHERE id = ?",
                {req.param("id")});

            if (result.empty())
            {
                co_return HttpResponse::notFound("user not found");
            }

            const auto& row = result[0];
            co_return HttpResponse::json(boost::json::object{
                {"id",         row[0]},
                {"name",       row[1]},
                {"email",      row[2]},
                {"created_at", row[3]},
            });
        });

    // ── POST /users ──────────────────────────────────────────────────
    server.router().post("/users",
        [](const HttpRequest& req) -> Awaitable<HttpResponse>
        {
            auto body = req.readJson<CreateUserReq>();
            auto conn = getDbConnection(req);

            auto result = co_await conn->execute(
                "INSERT INTO users(name, email) VALUES(?, ?)",
                {body.name, body.email});

            co_return HttpResponse::json(boost::json::object{
                {"id",   result.insertId},
                {"name", body.name},
            });
        });

    // ── DELETE /users/{id} ───────────────────────────────────────────
    server.router().del("/users/{id}",
        [](const HttpRequest& req) -> Awaitable<HttpResponse>
        {
            auto conn   = getDbConnection(req);
            auto result = co_await conn->execute(
                "DELETE FROM users WHERE id = ?",
                {req.param("id")});

            if (result.affectedRows == 0)
            {
                co_return HttpResponse::notFound("user not found");
            }
            co_return HttpResponse::noContent();
        });

    // 4. 初始化连接池并启动服务（异步初始化必须在 start() 前完成）
    hical::coSpawn(server.ioContext(),
        [&pool]() -> Awaitable<void>
        {
            co_await pool->init();
            HICAL_LOG_INFO("DB pool ready, idle={}", pool->idleCount());
        });

    HICAL_LOG_INFO("Server listening on :8080");
    server.start();
}
```

### 快速测试

```bash
# 创建用户
curl -X POST http://localhost:8080/users \
     -H "Content-Type: application/json" \
     -d '{"name":"Alice","email":"alice@example.com"}'
# {"id":1,"name":"Alice"}

# 查询列表
curl http://localhost:8080/users
# [{"id":1,"name":"Alice","email":"alice@example.com","created_at":"2026-05-04 12:00:00"}]

# 查询单个
curl http://localhost:8080/users/1
# {"id":1,"name":"Alice","email":"alice@example.com","created_at":"..."}

# 删除
curl -X DELETE http://localhost:8080/users/1
# HTTP 204 No Content
```

---

## 自动事务详解

`autoTransaction = true` 的效果：**中间件在路由执行前自动 `BEGIN`，路由正常返回后自动 `COMMIT`，抛出异常时自动 `ROLLBACK`**。

这意味着业务代码里完全不需要写事务控制。来看一个转账场景：

```cpp
server.router().post("/transfer",
    [](const HttpRequest& req) -> Awaitable<HttpResponse>
    {
        auto conn = getDbConnection(req);

        // 扣款
        co_await conn->execute(
            "UPDATE accounts SET balance = balance - ? WHERE id = ?",
            {"100", req.param("from_id")});

        // 模拟业务异常：中间件捕获后自动回滚，上面的扣款不会落库
        throw std::runtime_error("insufficient balance");

        // 加款（不会执行到这里）
        co_await conn->execute(
            "UPDATE accounts SET balance = balance + ? WHERE id = ?",
            {"100", req.param("to_id")});

        co_return HttpResponse::ok("ok");
    });
```

异常路径下，中间件的洋葱后置阶段会自动调用 `conn->rollback()`，无需业务层操心。

如果需要**手动**控制事务（例如部分提交），将 `autoTransaction` 设为 `false`，然后自己调用：

```cpp
co_await conn->beginTransaction();
// ... 业务逻辑 ...
co_await conn->commit();
// 出错时:
co_await conn->rollback();
```

---

## 慢查询检测

`makeQueryLogMiddleware` 必须在 `makeDbMiddleware` **之后**注册（它依赖请求中已注入的连接）：

```cpp
#include "db/DbQueryLog.h"

// 先注册 DB 中间件
server.use(makeDbMiddleware(pool, {.autoTransaction = true}));

// 再注册查询日志中间件
server.use(makeQueryLogMiddleware(QueryLogOptions{
    // 每个请求完成后汇总打印所有查询
    .onRequestComplete = [](const HttpRequest& req,
                            const std::vector<QueryLogEntry>& entries)
    {
        for (const auto& e : entries)
        {
            HICAL_LOG_DEBUG("[query] {} | {}μs | {} rows",
                e.sql, e.duration.count(), e.rowCount);
        }
    },

    // 慢查询阈值：超过 100ms 实时触发回调
    .slowQueryThreshold = std::chrono::milliseconds(100),
    .onSlowQuery = [](const QueryLogEntry& e)
    {
        HICAL_LOG_WARN("[slow query] {}ms | sql={}",
            e.duration.count() / 1000, e.sql);
    },
}));
```

`QueryLogEntry` 的字段：

| 字段              | 类型                        | 说明                 |
| ----------------- | --------------------------- | -------------------- |
| `sql`             | `std::string`               | 原始 SQL 文本        |
| `duration`        | `std::chrono::microseconds` | 执行耗时             |
| `rowCount`        | `size_t`                    | 返回行数（SELECT）   |
| `affectedRows`    | `uint64_t`                  | 影响行数（DML）      |
| `isParameterized` | `bool`                      | 是否使用了参数化查询 |

---

## 连接池参数调优

| 参数                  | 默认值 | 推荐场景                                                       |
| --------------------- | ------ | -------------------------------------------------------------- |
| `minConnections`      | 2      | 低流量服务保持 2-4，避免冷启动延迟                             |
| `maxConnections`      | 16     | 单机 Web 服务通常 16-32，不超过 MySQL `max_connections` 的 1/4 |
| `idleTimeout`         | 300s   | MySQL 默认 `wait_timeout` 8h，设 5 分钟绰绰有余                |
| `acquireTimeout`      | 5s     | 池满等待上限；超时返回 503 而非无限阻塞                        |
| `queryTimeout`        | 30s    | 防止慢查询耗尽所有连接；OLTP 场景建议设 3-5s                   |
| `healthCheckInterval` | 30s    | 定期 ping 空闲连接，防止 MySQL 服务端主动断开                  |
| `pingGracePeriod`     | 15s    | 距上次 ping 不超过 15s 则跳过，减少不必要的往返                |
| `stmtCacheSize`       | 64     | 每连接 LRU 缓存容量，见下节                                    |

高并发写入场景参考配置：

```cpp
dbCfg.minConnections      = 4;
dbCfg.maxConnections      = 32;
dbCfg.acquireTimeout      = std::chrono::seconds{3};
dbCfg.queryTimeout        = std::chrono::seconds{5};
dbCfg.healthCheckInterval = std::chrono::seconds{20};
```

---

## PreparedStatement 缓存（stmtCacheSize）

每次 `query()` / `execute()` 调用都会走 PreparedStatement —— 这是 Hical 防 SQL 注入的核心机制。但每次都向 MySQL 发送 `PREPARE` 请求会有网络往返开销。

`stmtCacheSize` 控制每个连接维护的 LRU 缓存容量（默认 64 条）：

- **首次**执行某条 SQL → 发送 `PREPARE`，缓存句柄
- **后续**执行同一 SQL → 直接复用缓存句柄，**零额外往返**
- 缓存满时 → 淘汰最久未使用的语句（LRU），释放资源

```cpp
dbCfg.stmtCacheSize = 128;  // 路由多、SQL 种类多时可调大
dbCfg.stmtCacheSize = 0;    // 0 = 禁用缓存，每次重新 PREPARE（测试用）
```

实测：对同一 SQL 的第二次调用，耗时从 ~1.2ms（含 PREPARE RTT）降到 ~0.3ms（纯执行）。在 QPS 较高的接口上效果明显。

---

## 总结

Hical 的 DB 中间件把连接池、事务、PreparedStatement 三件最繁琐的事情统一封装，让 C++20 协程写出的数据库代码和 Go/Python 一样直白：**`co_await conn->query(sql, params)` 取结果，中间件自动搞定其余一切**。

---

深度教学系列：
- [第一篇：设计理念与架构总览](01-design-philosophy.md)
- [第二篇：协程异步与 PMR 内存池](02-coroutine-and-memory.md)
- [第三篇：路由、中间件与 SSL](03-router-middleware-ssl.md)
- [第四篇：实战案例与性能调优](04-practice-and-performance.md)
- [第五篇：Cookie、Session 与文件服务](05-cookies-sessions-fileservices.md)
- [第六篇：快速上手 REST API](06-quick-rest-api.md)
- [第七篇：框架横评对比](07-framework-comparison.md)
- [第八篇：Hical + MySQL CRUD 实战](08-hical-mysql-crud.md)（本文）
- [第九篇：日志系统完全指南](09-hical-logging-guide.md)
- [第十篇：OpenAPI 自动文档生成](10-hical-openapi-swagger.md)

GitHub: [https://github.com/Hical61/Hical](https://github.com/Hical61/Hical)
