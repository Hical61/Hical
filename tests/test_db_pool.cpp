#include "db/DbConfig.h"
#include "db/DbConnection.h"
#include "db/DbConnectionPool.h"
#include "db/DbResult.h"
#include "core/Coroutine.h"
#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>

using namespace hical;
using namespace hical::db;

// ============ Mock 连接实现 ============

class MockDbConnection : public DbConnection
{
public:
	MockDbConnection() = default;

	Awaitable<DbResult> query(std::string_view /*sql*/) override
	{
		++queryCount_;
		co_return DbResult {.columns = {"id", "name"}, .rows = {{"1", "test"}}, .affectedRows = 0};
	}

	Awaitable<DbResult> query(std::string_view sql, std::span<const std::string> /*params*/) override
	{
		return query(sql);
	}

	Awaitable<DbResult> execute(std::string_view /*sql*/) override
	{
		++executeCount_;
		co_return DbResult {.affectedRows = 1, .insertId = 42};
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
		return alive_;
	}

	Awaitable<bool> ping() override
	{
		if (alive_)
		{
			lastPing_ = std::chrono::steady_clock::now();
		}
		co_return alive_;
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

	// 测试用控制和观察
	bool alive_ = true;
	bool inTransaction_ = false;
	int queryCount_ = 0;
	int executeCount_ = 0;
	int commitCount_ = 0;
	int rollbackCount_ = 0;
	std::chrono::steady_clock::time_point lastActive_ = std::chrono::steady_clock::now();
	std::chrono::steady_clock::time_point lastPing_;
};

// Mock 连接工厂
DbConnectionFactory makeMockFactory()
{
	return
		[](boost::asio::io_context& /*ioCtx*/, const DbConfig& /*config*/) -> Awaitable<std::shared_ptr<DbConnection>>
	{
		co_return std::make_shared<MockDbConnection>();
	};
}

// 辅助：在事件循环中运行协程
template <typename F>
auto runCoroutine(F&& f)
{
	using ReturnType = typename std::invoke_result_t<F>::value_type;

	boost::asio::io_context ioCtx;
	std::optional<ReturnType> result;
	std::exception_ptr eptr;

	coSpawn(ioCtx,
			[&]() -> Awaitable<void>
			{
				try
				{
					result = co_await f();
				}
				catch (...)
				{
					eptr = std::current_exception();
				}
			});

	ioCtx.run();

	if (eptr)
	{
		std::rethrow_exception(eptr);
	}

	return result;
}

// 辅助：运行 void 协程
template <typename F>
void runVoidCoroutine(F&& f)
{
	boost::asio::io_context ioCtx;
	std::exception_ptr eptr;

	coSpawn(ioCtx,
			[&]() -> Awaitable<void>
			{
				try
				{
					co_await f();
				}
				catch (...)
				{
					eptr = std::current_exception();
				}
			});

	ioCtx.run();

	if (eptr)
	{
		std::rethrow_exception(eptr);
	}
}

// ============ DbResult 测试 ============

TEST(DbResultTest, EmptyResult)
{
	DbResult result;
	EXPECT_TRUE(result.empty());
	EXPECT_EQ(result.size(), 0);
	EXPECT_EQ(result.affectedRows, 0);
	EXPECT_EQ(result.insertId, 0);
}

TEST(DbResultTest, WithRows)
{
	DbResult result;
	result.columns = {"id", "name"};
	result.rows = {{"1", "Alice"}, {"2", "Bob"}};
	EXPECT_FALSE(result.empty());
	EXPECT_EQ(result.size(), 2);
	auto idxName = result.columnIndex("name");
	ASSERT_NE(idxName, DbResult::npos);
	EXPECT_EQ(result[0][idxName], "Alice");
	EXPECT_EQ(result[1][idxName], "Bob");
}

TEST(DbResultTest, InsertResult)
{
	DbResult result;
	result.affectedRows = 1;
	result.insertId = 42;
	EXPECT_TRUE(result.empty());
	EXPECT_EQ(result.affectedRows, 1);
	EXPECT_EQ(result.insertId, 42);
}

// ============ 连接池初始化测试 ============

TEST(DbConnectionPoolTest, InitCreatesMinConnections)
{
	boost::asio::io_context ioCtx;
	auto config = DbConfig {.minConnections = 3,
							.maxConnections = 10,
							.idleCheckInterval = std::chrono::seconds(0),
							.healthCheckInterval = std::chrono::seconds(0),
							.pingGracePeriod = std::chrono::seconds(0)};
	auto pool = std::make_shared<DbConnectionPool>(ioCtx, config, makeMockFactory());

	coSpawn(ioCtx,
			[pool]() -> Awaitable<void>
			{
				co_await pool->init();
			});
	ioCtx.run();

	EXPECT_EQ(pool->idleCount(), 3);
	EXPECT_EQ(pool->activeCount(), 0);
	EXPECT_EQ(pool->totalCount(), 3);
}

// ============ 连接获取/归还测试 ============

TEST(DbConnectionPoolTest, AcquireAndRelease)
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

				// 获取一个连接
				auto conn = co_await pool->acquire();
				EXPECT_NE(conn, nullptr);
				EXPECT_EQ(pool->activeCount(), 1);

				// 归还
				pool->release(std::move(conn));
			});
	ioCtx.run();

	// 归还后
	EXPECT_EQ(pool->activeCount(), 0);
	EXPECT_GE(pool->idleCount(), 1);
}

TEST(DbConnectionPoolTest, AcquireMultiple)
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

				// 获取多个连接
				auto conn1 = co_await pool->acquire();
				auto conn2 = co_await pool->acquire();
				auto conn3 = co_await pool->acquire();

				EXPECT_EQ(pool->activeCount(), 3);

				// 全部归还
				pool->release(std::move(conn1));
				pool->release(std::move(conn2));
				pool->release(std::move(conn3));
			});
	ioCtx.run();

	EXPECT_EQ(pool->activeCount(), 0);
}

// ============ 连接活性检查测试 ============

TEST(DbConnectionPoolTest, DeadConnectionIsRecreated)
{
	std::atomic<int> createCount {0};
	DbConnectionFactory factory = [&createCount](boost::asio::io_context& /*ioCtx*/,
												 const DbConfig& /*config*/) -> Awaitable<std::shared_ptr<DbConnection>>
	{
		auto conn = std::make_shared<MockDbConnection>();
		int count = ++createCount;
		// 第一个连接标记为死亡，后续的活着
		if (count == 1)
		{
			conn->alive_ = false;
		}
		co_return conn;
	};

	boost::asio::io_context ioCtx;
	auto config = DbConfig {.minConnections = 1,
							.maxConnections = 4,
							.idleCheckInterval = std::chrono::seconds(0),
							.healthCheckInterval = std::chrono::seconds(0),
							.pingGracePeriod = std::chrono::seconds(0)};
	auto pool = std::make_shared<DbConnectionPool>(ioCtx, config, factory);

	coSpawn(ioCtx,
			[pool]() -> Awaitable<void>
			{
				co_await pool->init();

				// 获取连接时，第一个（死连接）会被丢弃，创建新连接
				auto conn = co_await pool->acquire();
				EXPECT_NE(conn, nullptr);
				EXPECT_TRUE(conn->isAlive());

				pool->release(std::move(conn));
			});
	ioCtx.run();

	// 创建了 1 个初始 + 1 个替代 = 至少 2 个
	EXPECT_GE(createCount.load(), 2);
}

// ============ 池满等待测试 ============

TEST(DbConnectionPoolTest, AcquireTimeoutWhenPoolFull)
{
	boost::asio::io_context ioCtx;
	auto config = DbConfig {.minConnections = 1,
							.maxConnections = 1,
							.acquireTimeout = std::chrono::seconds(1),
							.idleCheckInterval = std::chrono::seconds(0),
							.healthCheckInterval = std::chrono::seconds(0),
							.pingGracePeriod = std::chrono::seconds(0)};
	auto pool = std::make_shared<DbConnectionPool>(ioCtx, config, makeMockFactory());

	bool timeoutThrown = false;

	coSpawn(ioCtx,
			[pool, &timeoutThrown]() -> Awaitable<void>
			{
				co_await pool->init();

				// 占满池
				auto conn1 = co_await pool->acquire();
				EXPECT_EQ(pool->activeCount(), 1);

				// 第二次获取应该超时
				try
				{
					auto conn2 = co_await pool->acquire();
					(void)conn2;
				}
				catch (const std::runtime_error& e)
				{
					timeoutThrown = true;
					EXPECT_NE(std::string(e.what()).find("timeout"), std::string::npos);
				}

				pool->release(std::move(conn1));
			});
	ioCtx.run();

	EXPECT_TRUE(timeoutThrown);
}

// ============ 池关闭测试 ============

TEST(DbConnectionPoolTest, ShutdownClearsIdleConnections)
{
	boost::asio::io_context ioCtx;
	auto config = DbConfig {.minConnections = 3,
							.maxConnections = 10,
							.idleCheckInterval = std::chrono::seconds(0),
							.healthCheckInterval = std::chrono::seconds(0),
							.pingGracePeriod = std::chrono::seconds(0)};
	auto pool = std::make_shared<DbConnectionPool>(ioCtx, config, makeMockFactory());

	coSpawn(ioCtx,
			[pool]() -> Awaitable<void>
			{
				co_await pool->init();
				EXPECT_EQ(pool->idleCount(), 3);

				co_await pool->shutdown();
				EXPECT_EQ(pool->idleCount(), 0);
			});
	ioCtx.run();
}

// ============ 事务残留回滚测试 ============

TEST(DbConnectionPoolTest, ReleaseRollsBackTransaction)
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

				auto conn = co_await pool->acquire();
				mockConn = std::dynamic_pointer_cast<MockDbConnection>(conn);

				// 模拟开启事务但不提交
				co_await conn->beginTransaction();
				EXPECT_TRUE(conn->inTransaction());

				// 归还连接（应自动回滚）
				pool->release(conn);

				// 等待异步回滚完成
				co_await hical::sleep(0.1);
			});
	ioCtx.run();

	// 回滚应该被调用了
	EXPECT_EQ(mockConn->rollbackCount_, 1);
}

// ============ 统计测试 ============

TEST(DbConnectionPoolTest, Statistics)
{
	boost::asio::io_context ioCtx;
	auto config = DbConfig {.minConnections = 2,
							.maxConnections = 8,
							.idleCheckInterval = std::chrono::seconds(0),
							.healthCheckInterval = std::chrono::seconds(0),
							.pingGracePeriod = std::chrono::seconds(0)};
	auto pool = std::make_shared<DbConnectionPool>(ioCtx, config, makeMockFactory());

	coSpawn(ioCtx,
			[pool]() -> Awaitable<void>
			{
				co_await pool->init();

				EXPECT_EQ(pool->idleCount(), 2);
				EXPECT_EQ(pool->activeCount(), 0);
				EXPECT_EQ(pool->waitingCount(), 0);
				EXPECT_EQ(pool->totalCount(), 2);

				auto conn1 = co_await pool->acquire();
				auto conn2 = co_await pool->acquire();

				EXPECT_EQ(pool->activeCount(), 2);
				EXPECT_LE(pool->idleCount(), 2);

				pool->release(std::move(conn1));
				pool->release(std::move(conn2));
			});
	ioCtx.run();
}

// ============ 健康检查测试 ============

TEST(DbConnectionPoolTest, HealthCheckRemovesDeadConnections)
{
	// 使用短健康检查间隔
	boost::asio::io_context ioCtx;
	auto config = DbConfig {.minConnections = 1,
							.maxConnections = 4,
							.idleCheckInterval = std::chrono::seconds(0),
							.healthCheckInterval = std::chrono::seconds(1),
							.pingGracePeriod = std::chrono::seconds(0)};

	std::vector<std::shared_ptr<MockDbConnection>> createdConns;
	DbConnectionFactory factory =
		[&createdConns](boost::asio::io_context& /*ioCtx*/,
						const DbConfig& /*config*/) -> Awaitable<std::shared_ptr<DbConnection>>
	{
		auto conn = std::make_shared<MockDbConnection>();
		createdConns.push_back(conn);
		co_return conn;
	};

	auto pool = std::make_shared<DbConnectionPool>(ioCtx, config, factory);

	coSpawn(ioCtx,
			[pool, &createdConns]() -> Awaitable<void>
			{
				co_await pool->init();
				EXPECT_EQ(pool->idleCount(), 1);
				EXPECT_EQ(createdConns.size(), 1);

				// 标记第一个连接为死亡
				createdConns[0]->alive_ = false;

				// 等待健康检查运行
				co_await hical::sleep(1.5);

				// 死连接应被移除，新连接应被补充（minConnections=1）
				EXPECT_GE(createdConns.size(), 2);

				co_await pool->shutdown();
			});
	ioCtx.run();
}

TEST(DbConnectionPoolTest, HealthCheckReplenishesToMinConnections)
{
	boost::asio::io_context ioCtx;
	auto config = DbConfig {.minConnections = 3,
							.maxConnections = 10,
							.idleCheckInterval = std::chrono::seconds(0),
							.healthCheckInterval = std::chrono::seconds(1),
							.pingGracePeriod = std::chrono::seconds(0)};

	std::atomic<int> createCount {0};
	std::vector<std::shared_ptr<MockDbConnection>> createdConns;
	std::mutex connsMutex;
	DbConnectionFactory factory = [&](boost::asio::io_context& /*ioCtx*/,
									  const DbConfig& /*config*/) -> Awaitable<std::shared_ptr<DbConnection>>
	{
		auto conn = std::make_shared<MockDbConnection>();
		++createCount;
		std::lock_guard lock(connsMutex);
		createdConns.push_back(conn);
		co_return conn;
	};

	auto pool = std::make_shared<DbConnectionPool>(ioCtx, config, factory);

	coSpawn(ioCtx,
			[pool, &createdConns, &connsMutex]() -> Awaitable<void>
			{
				co_await pool->init();
				EXPECT_EQ(pool->idleCount(), 3);

				// 杀掉 2 个空闲连接
				{
					std::lock_guard lock(connsMutex);
					createdConns[0]->alive_ = false;
					createdConns[1]->alive_ = false;
				}

				// 等待健康检查
				co_await hical::sleep(1.5);

				// 应补充回 3 个
				EXPECT_GE(pool->idleCount(), 1); // 至少原来的 1 个活着 + 补充的

				co_await pool->shutdown();
			});
	ioCtx.run();
}

TEST(DbConnectionPoolTest, AcquireSkipsPingWithinGracePeriod)
{
	boost::asio::io_context ioCtx;
	auto config = DbConfig {
		.minConnections = 1,
		.maxConnections = 4,
		.idleCheckInterval = std::chrono::seconds(0),
		.healthCheckInterval = std::chrono::seconds(1),
		.pingGracePeriod = std::chrono::seconds(60) // 宽限期很长
	};

	int pingCount = 0;
	DbConnectionFactory factory = [&pingCount](boost::asio::io_context& /*ioCtx*/,
											   const DbConfig& /*config*/) -> Awaitable<std::shared_ptr<DbConnection>>
	{
		auto conn = std::make_shared<MockDbConnection>();
		// 模拟刚被 ping 过
		conn->lastPing_ = std::chrono::steady_clock::now();
		co_return conn;
	};

	auto pool = std::make_shared<DbConnectionPool>(ioCtx, config, factory);

	coSpawn(ioCtx,
			[pool]() -> Awaitable<void>
			{
				co_await pool->init();

				// acquire 应跳过 ping（因为 lastPingTime 在宽限期内）
				auto conn = co_await pool->acquire();
				EXPECT_NE(conn, nullptr);

				pool->release(std::move(conn));
				co_await pool->shutdown();
			});
	ioCtx.run();
}

TEST(DbConnectionPoolTest, AcquirePingsWhenGracePeriodExpired)
{
	boost::asio::io_context ioCtx;
	auto config = DbConfig {
		.minConnections = 1,
		.maxConnections = 4,
		.idleCheckInterval = std::chrono::seconds(0),
		.healthCheckInterval = std::chrono::seconds(1),
		.pingGracePeriod = std::chrono::seconds(0) // 宽限期为 0，总是 ping
	};

	std::atomic<int> pingCount {0};
	DbConnectionFactory factory = [&pingCount](boost::asio::io_context& /*ioCtx*/,
											   const DbConfig& /*config*/) -> Awaitable<std::shared_ptr<DbConnection>>
	{
		auto conn = std::make_shared<MockDbConnection>();
		// lastPingTime 默认为 epoch（从未 ping），skipPing 条件不满足
		co_return conn;
	};

	auto pool = std::make_shared<DbConnectionPool>(ioCtx, config, factory);

	coSpawn(ioCtx,
			[pool]() -> Awaitable<void>
			{
				co_await pool->init();

				auto conn = co_await pool->acquire();
				EXPECT_NE(conn, nullptr);
				// 连接应该被 ping 过了（因为宽限期为 0）
				auto mock = std::dynamic_pointer_cast<MockDbConnection>(conn);
				// lastPingTime 应已更新（由 acquire 中的 ping 触发）
				EXPECT_NE(mock->lastPing_, std::chrono::steady_clock::time_point {});

				pool->release(std::move(conn));
				co_await pool->shutdown();
			});
	ioCtx.run();
}
