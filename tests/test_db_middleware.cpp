#include "db/DbConfig.h"
#include "db/DbConnection.h"
#include "db/DbConnectionPool.h"
#include "db/DbMiddleware.h"
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
	Awaitable<DbResult> query(std::string_view /*sql*/) override
	{
		++queryCount_;
		co_return DbResult {.columns = {"id"}, .rows = {{"1"}}, .affectedRows = 0};
	}

	Awaitable<DbResult> query(std::string_view sql, std::span<const std::string> /*params*/) override
	{
		return query(sql);
	}

	Awaitable<DbResult> execute(std::string_view /*sql*/) override
	{
		co_return DbResult {.affectedRows = 1, .insertId = 1};
	}

	Awaitable<DbResult> execute(std::string_view sql, std::span<const std::string> /*params*/) override
	{
		return execute(sql);
	}

	Awaitable<void> beginTransaction() override
	{
		inTransaction_ = true;
		co_return;
	}

	Awaitable<void> commit() override
	{
		inTransaction_ = false;
		++commitCount_;
		co_return;
	}

	Awaitable<void> rollback() override
	{
		inTransaction_ = false;
		++rollbackCount_;
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
	int commitCount_ = 0;
	int rollbackCount_ = 0;
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

// ============ 中间件注入测试 ============

TEST(DbMiddlewareTest, InjectsConnectionIntoRequest)
{
	boost::asio::io_context ioCtx;
	auto config = DbConfig {.minConnections = 1,
							.maxConnections = 4,
							.idleCheckInterval = std::chrono::seconds(0),
							.healthCheckInterval = std::chrono::seconds(0),
							.pingGracePeriod = std::chrono::seconds(0)};
	auto pool = std::make_shared<DbConnectionPool>(ioCtx, config, makeMockFactory());

	bool connFound = false;

	coSpawn(ioCtx,
			[pool, &connFound]() -> Awaitable<void>
			{
				co_await pool->init();

				MiddlewarePipeline pipeline;
				pipeline.use(makeDbMiddleware(pool));

				HttpRequest req;
				req.setMethod(HttpMethod::hGet);
				req.setTarget("/test");

				auto res = co_await pipeline.execute(
					req,
					[&connFound](HttpRequest& innerReq) -> Awaitable<HttpResponse>
					{
						auto conn = innerReq.getAttribute<std::shared_ptr<DbConnection>>(DbConnectionPool::hConnKey);
						connFound = conn.has_value();
						co_return HttpResponse::ok("ok");
					});

				EXPECT_EQ(res.statusCode(), HttpStatusCode::hOk);
			});
	ioCtx.run();

	EXPECT_TRUE(connFound);
}

TEST(DbMiddlewareTest, InjectsPoolWhenEnabled)
{
	boost::asio::io_context ioCtx;
	auto config = DbConfig {.minConnections = 1,
							.maxConnections = 4,
							.idleCheckInterval = std::chrono::seconds(0),
							.healthCheckInterval = std::chrono::seconds(0),
							.pingGracePeriod = std::chrono::seconds(0)};
	auto pool = std::make_shared<DbConnectionPool>(ioCtx, config, makeMockFactory());

	bool poolFound = false;

	coSpawn(ioCtx,
			[pool, &poolFound]() -> Awaitable<void>
			{
				co_await pool->init();

				MiddlewarePipeline pipeline;
				pipeline.use(makeDbMiddleware(pool, {.injectPool = true}));

				HttpRequest req;
				req.setMethod(HttpMethod::hGet);
				req.setTarget("/test");

				co_await pipeline.execute(req,
										  [&poolFound](HttpRequest& innerReq) -> Awaitable<HttpResponse>
										  {
											  auto p = innerReq.getAttribute<std::shared_ptr<DbConnectionPool>>(
												  DbConnectionPool::hPoolKey);
											  poolFound = p.has_value();
											  co_return HttpResponse::ok("ok");
										  });
			});
	ioCtx.run();

	EXPECT_TRUE(poolFound);
}

TEST(DbMiddlewareTest, DoesNotInjectPoolWhenDisabled)
{
	boost::asio::io_context ioCtx;
	auto config = DbConfig {.minConnections = 1,
							.maxConnections = 4,
							.idleCheckInterval = std::chrono::seconds(0),
							.healthCheckInterval = std::chrono::seconds(0),
							.pingGracePeriod = std::chrono::seconds(0)};
	auto pool = std::make_shared<DbConnectionPool>(ioCtx, config, makeMockFactory());

	bool poolFound = false;

	coSpawn(ioCtx,
			[pool, &poolFound]() -> Awaitable<void>
			{
				co_await pool->init();

				MiddlewarePipeline pipeline;
				pipeline.use(makeDbMiddleware(pool, {.injectPool = false}));

				HttpRequest req;
				req.setMethod(HttpMethod::hGet);
				req.setTarget("/test");

				co_await pipeline.execute(req,
										  [&poolFound](HttpRequest& innerReq) -> Awaitable<HttpResponse>
										  {
											  auto p = innerReq.getAttribute<std::shared_ptr<DbConnectionPool>>(
												  DbConnectionPool::hPoolKey);
											  poolFound = p.has_value();
											  co_return HttpResponse::ok("ok");
										  });
			});
	ioCtx.run();

	EXPECT_FALSE(poolFound);
}

// ============ 便捷函数测试 ============

TEST(DbMiddlewareTest, GetDbConnectionHelperWorks)
{
	boost::asio::io_context ioCtx;
	auto config = DbConfig {.minConnections = 1,
							.maxConnections = 4,
							.idleCheckInterval = std::chrono::seconds(0),
							.healthCheckInterval = std::chrono::seconds(0),
							.pingGracePeriod = std::chrono::seconds(0)};
	auto pool = std::make_shared<DbConnectionPool>(ioCtx, config, makeMockFactory());

	bool success = false;

	coSpawn(ioCtx,
			[pool, &success]() -> Awaitable<void>
			{
				co_await pool->init();

				MiddlewarePipeline pipeline;
				pipeline.use(makeDbMiddleware(pool));

				HttpRequest req;
				req.setMethod(HttpMethod::hGet);
				req.setTarget("/test");

				co_await pipeline.execute(req,
										  [&success](HttpRequest& innerReq) -> Awaitable<HttpResponse>
										  {
											  auto conn = getDbConnection(innerReq);
											  EXPECT_NE(conn, nullptr);
											  EXPECT_EQ(conn->backend(), "mock");
											  success = true;
											  co_return HttpResponse::ok("ok");
										  });
			});
	ioCtx.run();

	EXPECT_TRUE(success);
}

TEST(DbMiddlewareTest, GetDbConnectionThrowsWithoutMiddleware)
{
	HttpRequest req;
	req.setMethod(HttpMethod::hGet);
	req.setTarget("/test");

	EXPECT_THROW(getDbConnection(req), std::runtime_error);
}

// ============ 自动事务测试 ============

TEST(DbMiddlewareTest, AutoTransactionCommitsOnSuccess)
{
	boost::asio::io_context ioCtx;
	auto config = DbConfig {.minConnections = 1,
							.maxConnections = 4,
							.idleCheckInterval = std::chrono::seconds(0),
							.healthCheckInterval = std::chrono::seconds(0),
							.pingGracePeriod = std::chrono::seconds(0)};
	auto pool = std::make_shared<DbConnectionPool>(ioCtx, config, makeMockFactory());

	std::shared_ptr<MockDbConnection> mockConn;

	coSpawn(ioCtx,
			[pool, &mockConn]() -> Awaitable<void>
			{
				co_await pool->init();

				MiddlewarePipeline pipeline;
				pipeline.use(makeDbMiddleware(pool, {.autoTransaction = true}));

				HttpRequest req;
				req.setMethod(HttpMethod::hGet);
				req.setTarget("/test");

				co_await pipeline.execute(req,
										  [&mockConn](HttpRequest& innerReq) -> Awaitable<HttpResponse>
										  {
											  auto conn = getDbConnection(innerReq);
											  mockConn = std::dynamic_pointer_cast<MockDbConnection>(conn);
											  EXPECT_TRUE(conn->inTransaction());
											  co_return HttpResponse::ok("ok");
										  });
			});
	ioCtx.run();

	ASSERT_NE(mockConn, nullptr);
	EXPECT_EQ(mockConn->commitCount_, 1);
	EXPECT_EQ(mockConn->rollbackCount_, 0);
}

TEST(DbMiddlewareTest, AutoTransactionRollsBackOnException)
{
	boost::asio::io_context ioCtx;
	auto config = DbConfig {.minConnections = 1,
							.maxConnections = 4,
							.idleCheckInterval = std::chrono::seconds(0),
							.healthCheckInterval = std::chrono::seconds(0),
							.pingGracePeriod = std::chrono::seconds(0)};
	auto pool = std::make_shared<DbConnectionPool>(ioCtx, config, makeMockFactory());

	std::shared_ptr<MockDbConnection> mockConn;

	coSpawn(ioCtx,
			[pool, &mockConn]() -> Awaitable<void>
			{
				co_await pool->init();

				MiddlewarePipeline pipeline;
				pipeline.use(makeDbMiddleware(pool, {.autoTransaction = true}));

				HttpRequest req;
				req.setMethod(HttpMethod::hGet);
				req.setTarget("/test");

				try
				{
					co_await pipeline.execute(req,
											  [&mockConn](HttpRequest& innerReq) -> Awaitable<HttpResponse>
											  {
												  auto conn = getDbConnection(innerReq);
												  mockConn = std::dynamic_pointer_cast<MockDbConnection>(conn);
												  throw std::runtime_error("handler error");
												  co_return HttpResponse::ok("unreachable");
											  });
				}
				catch (const std::runtime_error&)
				{
					// 预期异常
				}

				// 等待异步回滚
				co_await hical::sleep(0.1);
			});
	ioCtx.run();

	ASSERT_NE(mockConn, nullptr);
	EXPECT_EQ(mockConn->commitCount_, 0);
	EXPECT_EQ(mockConn->rollbackCount_, 1);
}

// ============ 连接归还测试 ============

TEST(DbMiddlewareTest, ConnectionReleasedAfterRequest)
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

				// 模拟多次请求，确保连接被正确归还和复用
				for (int i = 0; i < 5; ++i)
				{
					HttpRequest req;
					req.setMethod(HttpMethod::hGet);
					req.setTarget("/test");

					co_await pipeline.execute(req,
											  [](HttpRequest& innerReq) -> Awaitable<HttpResponse>
											  {
												  auto conn = getDbConnection(innerReq);
												  auto result = co_await conn->query("SELECT 1");
												  co_return HttpResponse::ok("ok");
											  });
				}

				// 5 次请求后，所有连接应已归还
				EXPECT_EQ(pool->activeCount(), 0);
			});
	ioCtx.run();
}

// ============ 洋葱模型集成测试 ============

TEST(DbMiddlewareTest, WorksWithOtherMiddleware)
{
	boost::asio::io_context ioCtx;
	auto config = DbConfig {.minConnections = 1,
							.maxConnections = 4,
							.idleCheckInterval = std::chrono::seconds(0),
							.healthCheckInterval = std::chrono::seconds(0),
							.pingGracePeriod = std::chrono::seconds(0)};
	auto pool = std::make_shared<DbConnectionPool>(ioCtx, config, makeMockFactory());

	std::vector<int> order;

	coSpawn(ioCtx,
			[pool, &order]() -> Awaitable<void>
			{
				co_await pool->init();

				MiddlewarePipeline pipeline;

				// 第一层：日志中间件
				pipeline.use(
					[&order](HttpRequest& req, MiddlewareNext next) -> Awaitable<HttpResponse>
					{
						order.push_back(1);
						auto res = co_await next(req);
						order.push_back(6);
						co_return res;
					});

				// 第二层：DB 中间件
				pipeline.use(makeDbMiddleware(pool));

				// 第三层：认证中间件
				pipeline.use(
					[&order](HttpRequest& req, MiddlewareNext next) -> Awaitable<HttpResponse>
					{
						order.push_back(3);
						auto res = co_await next(req);
						order.push_back(4);
						co_return res;
					});

				HttpRequest req;
				req.setMethod(HttpMethod::hGet);
				req.setTarget("/test");

				co_await pipeline.execute(req,
										  [&order](HttpRequest& innerReq) -> Awaitable<HttpResponse>
										  {
											  // 验证 DB 连接可用
											  auto conn = getDbConnection(innerReq);
											  EXPECT_NE(conn, nullptr);
											  co_return HttpResponse::ok("ok");
										  });
			});
	ioCtx.run();

	// 验证洋葱模型顺序：1 -> (db前置) -> 3 -> handler -> 4 -> (db后置) -> 6
	ASSERT_GE(order.size(), 4);
	EXPECT_EQ(order[0], 1);
	EXPECT_EQ(order[1], 3);
	EXPECT_EQ(order[2], 4);
	EXPECT_EQ(order[3], 6);
}
