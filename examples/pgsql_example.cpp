/**
 * @file pgsql_example.cpp
 * @brief PostgreSQL 后端(libpq)端到端示例：连接池 + 参数化查询 + RETURNING 拿 insertId + 手动事务
 * 编译需要开启 PG 后端（PG 构建在 DB 核心之上，两个开关要一起开）：
 *   cmake -B build -DHICAL_WITH_DATABASE=ON -DHICAL_WITH_PGSQL=ON ...
 *   cmake --build build --target pgsql_example
 * 运行前请确保 PostgreSQL 已启动，并建好测试库表（本示例默认连 127.0.0.1:5432 的 hical_test 库）：
 *   CREATE DATABASE hical_test;
 *   CREATE TABLE users (
 *       id         SERIAL PRIMARY KEY,
 *       name       TEXT NOT NULL,
 *       email      TEXT NOT NULL
 *   );
 *   CREATE TABLE audit_log (
 *       user_id BIGINT NOT NULL,
 *       action  TEXT   NOT NULL
 *   );
 */

#ifdef HICAL_HAS_PGSQL

	#include "core/Coroutine.h"
	#include "core/HttpServer.h"
	#include "core/Log.h"
	#include "db/DbConfig.h"
	#include "db/DbConnectionPool.h"
	#include "db/DbMiddleware.h"
	#include "db/DbQueryLog.h"
	#include "db/PgsqlConnection.h"

	#include <atomic>
	#include <chrono>
	#include <string>
	#include <thread>
	#include <vector>

using namespace hical;
using namespace hical::db;

namespace
{

	/**
	 * @brief 构造 PG 连接配置
	 * @param port PG 监听端口（默认 5432，docker 映射常见 54329）
	 * @note charset 字段对 PostgreSQL 无意义（PG 没有 MySQL 那种 SET NAMES 字符集协商），留默认即可。
	 */
	DbConfig makeConfig(uint16_t port)
	{
		DbConfig cfg;
		cfg.host = "127.0.0.1";
		cfg.port = port;
		cfg.user = "postgres";
		cfg.password = ""; // 本地免密可留空
		cfg.database = "hical_test";
		cfg.minConnections = 2;
		cfg.maxConnections = 8;
		cfg.acquireTimeout = std::chrono::seconds {5};
		cfg.stmtCacheSize = 64;
		return cfg;
	}

	/**
	 * @brief GET /users — 参数化查询(name 模糊匹配)
	 * @note 占位符用 $1/$2（不是 MySQL 的 ?），由 PgStmtCache 走 PQexecPrepared 服务端预编译。
	 */
	Awaitable<HttpResponse> listUsers(const HttpRequest& req)
	{
		auto conn = getDbConnection(req);

		// queryParam 返回 std::optional<std::string>，用 value_or 取回空串兜底
		auto nameFilter = req.queryParam("name").value_or("");
		DbResult result;

		if (nameFilter.empty())
		{
			std::vector<std::string> noParams;
			result = co_await conn->query("SELECT id, name, email FROM users ORDER BY id LIMIT 100", noParams);
		}
		else
		{
			// 用户输入必须参数化：$1 是 PostgreSQL 的占位符（MySQL 用 ?）
			std::vector<std::string> params = {nameFilter};
			result = co_await conn->query("SELECT id, name, email FROM users WHERE name LIKE $1 ORDER BY id LIMIT 100",
										  params);
		}

		auto idIdx = result.columnIndex("id");
		auto nameIdx = result.columnIndex("name");
		auto emailIdx = result.columnIndex("email");
		if (idIdx == DbResult::npos || nameIdx == DbResult::npos || emailIdx == DbResult::npos)
		{
			co_return HttpResponse::serverError();
		}

		std::string body = "[";
		for (size_t i = 0; i < result.size(); ++i)
		{
			if (i > 0)
			{
				body += ",";
			}
			const auto& row = result[i];
			body +=
				R"({"id":)" + row[idIdx] + R"(,"name":")" + row[nameIdx] + R"(","email":")" + row[emailIdx] + R"("})";
		}
		body += "]";

		co_return HttpResponse::ok(body);
	}

	/**
	 * @brief POST /users — INSERT ... RETURNING id 拿自增主键，再写审计日志(同一手动事务内)
	 * @note PostgreSQL 没有 MySQL 的 last_insert_id()，自增主键必须靠 RETURNING 子句取回。
	 *       DbResult::insertId 的来源就是 RETURNING 结果集的首行首列（无 RETURNING 时恒为 0）。
	 */
	Awaitable<HttpResponse> createUser(const HttpRequest& req)
	{
		auto conn = getDbConnection(req);

		auto name = req.formParam("name").value_or("");
		auto email = req.formParam("email").value_or("");
		if (name.empty() || email.empty())
		{
			co_return HttpResponse::badRequest("name and email required");
		}

		co_await conn->beginTransaction();
		std::exception_ptr eptr;
		uint64_t newId = 0;
		try
		{
			// INSERT ... RETURNING id：让 PG 把新插入行的自增主键回传
			std::vector<std::string> params = {name, email};
			auto result =
				co_await conn->execute("INSERT INTO users (name, email) VALUES ($1, $2) RETURNING id", params);

			// insertId 取自 RETURNING 结果集首行首列
			newId = result.insertId;

			std::vector<std::string> logParams = {std::to_string(newId), name};
			co_await conn->execute("INSERT INTO audit_log (user_id, action) VALUES ($1, $2)", logParams);

			co_await conn->commit();
		}
		catch (...)
		{
			eptr = std::current_exception();
		}

		// 回滚放在 catch 之外：C++ 协程不允许 co_await 出现在 catch 子句中
		if (eptr)
		{
			if (conn->inTransaction())
			{
				co_await conn->rollback();
			}
			std::rethrow_exception(eptr);
		}

		HttpResponse resp = HttpResponse::ok(std::to_string(newId));
		resp.setStatus(HttpStatusCode::hCreated);
		co_return resp;
	}

} // namespace

int main(int argc, char* argv[])
{
	// 首个命令行参数是 PG 端口，便于连 docker 映射的 54329（默认 5432）
	auto pgPort = static_cast<uint16_t>(argc >= 2 ? std::atoi(argv[1]) : 5432);

	HttpServer server(8080); // HttpServer 自建 base loop，不接收外部 io_context
	auto& ioCtx = server.ioContext();

	std::shared_ptr<DbConnectionPool> pool;
	std::atomic<bool> inited {false};
	std::exception_ptr initErr;

	// 用 base loop 的 io_context 初始化连接池（与 server 共享同一个 io_context）
	coSpawn(ioCtx,
			[&]() -> Awaitable<void>
			{
				try
				{
					auto p =
						std::make_shared<DbConnectionPool>(ioCtx, makeConfig(pgPort), PgsqlConnection::makeFactory());
					co_await p->init();
					pool = std::move(p);
				}
				catch (...)
				{
					initErr = std::current_exception();
				}
				inited = true;
			});

	// 手动 pump io_context 直到连接池初始化完成。
	// base loop 挂了 work guard，run() 不会返回，这里用 poll_one() + 短 sleep 推进。
	while (!inited)
	{
		if (ioCtx.poll_one() == 0)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds {1});
		}
	}
	if (initErr)
	{
		std::rethrow_exception(initErr);
	}

	QueryLogOptions qlogOpts;
	qlogOpts.slowQueryThreshold = std::chrono::microseconds {50'000}; // 50ms
	qlogOpts.onSlowQuery = [](const QueryLogEntry& e)
	{
		HICAL_LOG_WARN("[SlowQuery] {}ms  {}", e.duration.count() / 1000, e.sql);
	};

	// 注册顺序：DB 中间件在前，QueryLog 在后（QueryLog 依赖请求里已有连接）
	server.use(makeDbMiddleware(pool, {.autoTransaction = false, .injectPool = true}));
	server.use(makeQueryLogMiddleware(qlogOpts));

	server.router().get("/users", listUsers);
	server.router().post("/users", createUser);

	HICAL_LOG_INFO("pgsql_example started on :8080 (backend=pgsql)");
	server.start(); // 阻塞
	return 0;
}

#else

	#include <cstdio>

int main()
{
	std::printf("HICAL_HAS_PGSQL not defined; rebuild with -DHICAL_WITH_DATABASE=ON -DHICAL_WITH_PGSQL=ON\n");
	return 0;
}

#endif // HICAL_HAS_PGSQL
