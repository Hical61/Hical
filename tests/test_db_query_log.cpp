#include "db/DbConfig.h"
#include "db/DbConnection.h"
#include "db/DbConnectionPool.h"
#include "db/DbMiddleware.h"
#include "db/DbQueryLog.h"
#include "db/DbResult.h"
#include "core/Coroutine.h"
#include "core/HttpRequest.h"
#include "core/HttpResponse.h"
#include "core/Middleware.h"
#include <gtest/gtest.h>
#include <memory>

// 测试中使用静态字面量 SQL 调用无参数化重载，属于合法用途，抑制 deprecated 警告
#if defined(__GNUC__) || defined(__clang__)
	#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#elif defined(_MSC_VER)
	#pragma warning(disable : 4996)
#endif

using namespace hical;
using namespace hical::db;

// ============ Mock 连接实现 ============

class MockDbConnection : public DbConnection
{
public:
	Awaitable<DbResult> query(std::string_view sql) override
	{
		++queryCount_;
		lastSql_ = std::string(sql);
		co_return DbResult {.columns = {"id", "name"}, .rows = {{"1", "test"}}, .affectedRows = 0};
	}

	Awaitable<DbResult> query(std::string_view sql, std::span<const std::string> /*params*/) override
	{
		++queryCount_;
		lastSql_ = std::string(sql);
		co_return DbResult {.columns = {"id"}, .rows = {{"1"}}, .affectedRows = 0};
	}

	Awaitable<DbResult> execute(std::string_view sql) override
	{
		++executeCount_;
		lastSql_ = std::string(sql);
		co_return DbResult {.affectedRows = 1, .insertId = 42};
	}

	Awaitable<DbResult> execute(std::string_view sql, std::span<const std::string> /*params*/) override
	{
		++executeCount_;
		lastSql_ = std::string(sql);
		co_return DbResult {.affectedRows = 3, .insertId = 100};
	}

	Awaitable<void> beginTransaction() override
	{
		inTransaction_ = true;
		co_return;
	}

	Awaitable<void> commit() override
	{
		inTransaction_ = false;
		co_return;
	}

	Awaitable<void> rollback() override
	{
		inTransaction_ = false;
		co_return;
	}

	bool inTransaction() const override
	{
		return inTransaction_;
	}

	bool isAlive() const override
	{
		return true;
	}

	Awaitable<bool> ping() override
	{
		lastPing_ = std::chrono::steady_clock::now();
		co_return true;
	}

	std::string_view backend() const override
	{
		return "mock";
	}

	std::chrono::steady_clock::time_point lastActiveTime() const override
	{
		return lastActive_;
	}

	std::chrono::steady_clock::time_point lastPingTime() const override
	{
		return lastPing_;
	}

	void touch() override
	{
		lastActive_ = std::chrono::steady_clock::now();
	}

	bool inTransaction_ = false;
	int queryCount_ = 0;
	int executeCount_ = 0;
	std::string lastSql_;
	std::chrono::steady_clock::time_point lastActive_ = std::chrono::steady_clock::now();
	std::chrono::steady_clock::time_point lastPing_;
};

DbConnectionFactory makeMockFactory()
{
	return
		[](boost::asio::io_context& /*ioCtx*/, const DbConfig& /*config*/) -> Awaitable<std::shared_ptr<DbConnection>>
	{
		co_return std::make_shared<MockDbConnection>();
	};
}

// ============ 查询日志测试 ============

TEST(DbQueryLogTest, RecordsQueries)
{
	boost::asio::io_context ioCtx;
	auto config = DbConfig {.minConnections = 1,
							.maxConnections = 4,
							.idleCheckInterval = std::chrono::seconds(0),
							.healthCheckInterval = std::chrono::seconds(0),
							.pingGracePeriod = std::chrono::seconds(0)};
	auto pool = std::make_shared<DbConnectionPool>(ioCtx, config, makeMockFactory());

	std::shared_ptr<std::vector<QueryLogEntry>> capturedLog;

	coSpawn(ioCtx,
			[pool, &capturedLog]() -> Awaitable<void>
			{
				co_await pool->init();

				MiddlewarePipeline pipeline;
				pipeline.use(makeDbMiddleware(pool));
				pipeline.use(makeQueryLogMiddleware());

				HttpRequest req;
				req.setMethod(HttpMethod::hGet);
				req.setTarget("/test");

				co_await pipeline.execute(
					req,
					[&capturedLog](HttpRequest& innerReq) -> Awaitable<HttpResponse>
					{
						auto conn = getDbConnection(innerReq);

						// 执行 2 个查询
						co_await conn->query("SELECT * FROM users");
						co_await conn->execute("INSERT INTO logs VALUES (1)");

						// 获取日志
						auto log = innerReq.getAttribute<std::shared_ptr<std::vector<QueryLogEntry>>>(hQueryLogKey);
						if (log)
						{
							capturedLog = *log;
						}

						co_return HttpResponse::ok("ok");
					});
			});
	ioCtx.run();

	ASSERT_NE(capturedLog, nullptr);
	ASSERT_EQ(capturedLog->size(), 2);
	EXPECT_EQ((*capturedLog)[0].sql, "SELECT * FROM users");
	EXPECT_FALSE((*capturedLog)[0].isParameterized);
	EXPECT_EQ((*capturedLog)[0].rowCount, 1); // Mock 返回 1 行 2 列
	EXPECT_EQ((*capturedLog)[1].sql, "INSERT INTO logs VALUES (1)");
	EXPECT_EQ((*capturedLog)[1].affectedRows, 1);
}

TEST(DbQueryLogTest, RecordsDuration)
{
	boost::asio::io_context ioCtx;
	auto config = DbConfig {.minConnections = 1,
							.maxConnections = 4,
							.idleCheckInterval = std::chrono::seconds(0),
							.healthCheckInterval = std::chrono::seconds(0),
							.pingGracePeriod = std::chrono::seconds(0)};
	auto pool = std::make_shared<DbConnectionPool>(ioCtx, config, makeMockFactory());

	std::shared_ptr<std::vector<QueryLogEntry>> capturedLog;

	coSpawn(ioCtx,
			[pool, &capturedLog]() -> Awaitable<void>
			{
				co_await pool->init();

				MiddlewarePipeline pipeline;
				pipeline.use(makeDbMiddleware(pool));
				pipeline.use(makeQueryLogMiddleware());

				HttpRequest req;
				req.setMethod(HttpMethod::hGet);
				req.setTarget("/test");

				co_await pipeline.execute(
					req,
					[&capturedLog](HttpRequest& innerReq) -> Awaitable<HttpResponse>
					{
						auto conn = getDbConnection(innerReq);
						co_await conn->query("SELECT 1");
						auto log = innerReq.getAttribute<std::shared_ptr<std::vector<QueryLogEntry>>>(hQueryLogKey);
						if (log)
						{
							capturedLog = *log;
						}
						co_return HttpResponse::ok("ok");
					});
			});
	ioCtx.run();

	ASSERT_NE(capturedLog, nullptr);
	ASSERT_EQ(capturedLog->size(), 1);
	// duration 应为非负
	EXPECT_GE((*capturedLog)[0].duration.count(), 0);
}

TEST(DbQueryLogTest, ParameterizedQueryMarkedCorrectly)
{
	boost::asio::io_context ioCtx;
	auto config = DbConfig {.minConnections = 1,
							.maxConnections = 4,
							.idleCheckInterval = std::chrono::seconds(0),
							.healthCheckInterval = std::chrono::seconds(0),
							.pingGracePeriod = std::chrono::seconds(0)};
	auto pool = std::make_shared<DbConnectionPool>(ioCtx, config, makeMockFactory());

	std::shared_ptr<std::vector<QueryLogEntry>> capturedLog;

	coSpawn(ioCtx,
			[pool, &capturedLog]() -> Awaitable<void>
			{
				co_await pool->init();

				MiddlewarePipeline pipeline;
				pipeline.use(makeDbMiddleware(pool));
				pipeline.use(makeQueryLogMiddleware());

				HttpRequest req;
				req.setMethod(HttpMethod::hGet);
				req.setTarget("/test");

				co_await pipeline.execute(
					req,
					[&capturedLog](HttpRequest& innerReq) -> Awaitable<HttpResponse>
					{
						auto conn = getDbConnection(innerReq);
						std::vector<std::string> params = {"1"};
						co_await conn->query("SELECT * FROM users WHERE id = ?", std::span<const std::string>(params));
						auto log = innerReq.getAttribute<std::shared_ptr<std::vector<QueryLogEntry>>>(hQueryLogKey);
						if (log)
						{
							capturedLog = *log;
						}
						co_return HttpResponse::ok("ok");
					});
			});
	ioCtx.run();

	ASSERT_NE(capturedLog, nullptr);
	ASSERT_EQ(capturedLog->size(), 1);
	EXPECT_TRUE((*capturedLog)[0].isParameterized);
}

TEST(DbQueryLogTest, SlowQueryCallbackInvoked)
{
	boost::asio::io_context ioCtx;
	auto config = DbConfig {.minConnections = 1,
							.maxConnections = 4,
							.idleCheckInterval = std::chrono::seconds(0),
							.healthCheckInterval = std::chrono::seconds(0),
							.pingGracePeriod = std::chrono::seconds(0)};
	auto pool = std::make_shared<DbConnectionPool>(ioCtx, config, makeMockFactory());

	int slowQueryCount = 0;

	QueryLogOptions opts;
	opts.slowQueryThreshold = std::chrono::microseconds(1); // 极低阈值，所有查询都触发
	opts.onSlowQuery = [&slowQueryCount](const QueryLogEntry& /*entry*/)
	{
		++slowQueryCount;
	};

	coSpawn(ioCtx,
			[pool, &opts]() -> Awaitable<void>
			{
				co_await pool->init();

				MiddlewarePipeline pipeline;
				pipeline.use(makeDbMiddleware(pool));
				pipeline.use(makeQueryLogMiddleware(opts));

				HttpRequest req;
				req.setMethod(HttpMethod::hGet);
				req.setTarget("/test");

				co_await pipeline.execute(req,
										  [](HttpRequest& innerReq) -> Awaitable<HttpResponse>
										  {
											  auto conn = getDbConnection(innerReq);
											  co_await conn->query("SELECT 1");
											  co_await conn->query("SELECT 2");
											  co_return HttpResponse::ok("ok");
										  });
			});
	ioCtx.run();

	// Mock 查询几乎无耗时，但至少有 1 个应超过 1us 阈值
	EXPECT_GE(slowQueryCount, 1);
}

TEST(DbQueryLogTest, OnRequestCompleteCallbackInvoked)
{
	boost::asio::io_context ioCtx;
	auto config = DbConfig {.minConnections = 1,
							.maxConnections = 4,
							.idleCheckInterval = std::chrono::seconds(0),
							.healthCheckInterval = std::chrono::seconds(0),
							.pingGracePeriod = std::chrono::seconds(0)};
	auto pool = std::make_shared<DbConnectionPool>(ioCtx, config, makeMockFactory());

	int callbackCount = 0;
	size_t entryCount = 0;

	QueryLogOptions opts;
	opts.onRequestComplete =
		[&callbackCount, &entryCount](const HttpRequest& /*req*/, const std::vector<QueryLogEntry>& entries)
	{
		++callbackCount;
		entryCount = entries.size();
	};

	coSpawn(ioCtx,
			[pool, &opts]() -> Awaitable<void>
			{
				co_await pool->init();

				MiddlewarePipeline pipeline;
				pipeline.use(makeDbMiddleware(pool));
				pipeline.use(makeQueryLogMiddleware(opts));

				HttpRequest req;
				req.setMethod(HttpMethod::hGet);
				req.setTarget("/test");

				co_await pipeline.execute(req,
										  [](HttpRequest& innerReq) -> Awaitable<HttpResponse>
										  {
											  auto conn = getDbConnection(innerReq);
											  co_await conn->query("SELECT 1");
											  co_await conn->execute("INSERT INTO t VALUES (1)");
											  co_await conn->query("SELECT 2");
											  co_return HttpResponse::ok("ok");
										  });
			});
	ioCtx.run();

	EXPECT_EQ(callbackCount, 1);
	EXPECT_EQ(entryCount, 3);
}

TEST(DbQueryLogTest, ConnectionRestoredAfterMiddleware)
{
	boost::asio::io_context ioCtx;
	auto config = DbConfig {.minConnections = 1,
							.maxConnections = 4,
							.idleCheckInterval = std::chrono::seconds(0),
							.healthCheckInterval = std::chrono::seconds(0),
							.pingGracePeriod = std::chrono::seconds(0)};
	auto pool = std::make_shared<DbConnectionPool>(ioCtx, config, makeMockFactory());

	coSpawn(ioCtx,
			[pool]() -> Awaitable<void>
			{
				co_await pool->init();

				MiddlewarePipeline pipeline;
				pipeline.use(makeDbMiddleware(pool));
				pipeline.use(makeQueryLogMiddleware());

				// 多次请求确保连接正确归还
				for (int i = 0; i < 3; ++i)
				{
					HttpRequest req;
					req.setMethod(HttpMethod::hGet);
					req.setTarget("/test");

					co_await pipeline.execute(req,
											  [](HttpRequest& innerReq) -> Awaitable<HttpResponse>
											  {
												  auto conn = getDbConnection(innerReq);
												  co_await conn->query("SELECT 1");
												  co_return HttpResponse::ok("ok");
											  });
				}

				// 所有连接应已归还
				EXPECT_EQ(pool->activeCount(), 0);
				EXPECT_GE(pool->idleCount(), 1);
			});
	ioCtx.run();
}
