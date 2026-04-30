/**
 * @file test_mysql_integration.cpp
 * @brief MySQL 集成测试(需要真实 MySQL 实例)
 * 环境变量配置:
 *   MYSQL_HOST     - 默认 127.0.0.1
 *   MYSQL_PORT     - 默认 3306
 *   MYSQL_USER     - 默认 root
 *   MYSQL_PASSWORD - 默认空
 *   MYSQL_DATABASE - 默认 hical_test
 * 如果无法连接 MySQL, 测试将被跳过(GTEST_SKIP)。
 */

#ifdef HICAL_HAS_DATABASE

	#include "db/DbConfig.h"
	#include "db/DbConnection.h"
	#include "db/DbConnectionPool.h"
	#include "db/DbMiddleware.h"
	#include "db/DbResult.h"
	#include "db/MysqlConnection.h"
	#include "core/Coroutine.h"
	#include <gtest/gtest.h>
	#include <cstdlib>
	#include <memory>

	// 测试中使用静态字面量 SQL 调用无参数化重载，属于合法用途，抑制 deprecated 警告
	#if defined(__GNUC__) || defined(__clang__)
		#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
	#elif defined(_MSC_VER)
		#pragma warning(disable : 4996)
	#endif

using namespace hical;
using namespace hical::db;

static DbConfig getTestConfig()
{
	DbConfig config;
	if (auto* val = std::getenv("MYSQL_HOST"))
	{
		config.host = val;
	}
	if (auto* val = std::getenv("MYSQL_PORT"))
	{
		config.port = static_cast<uint16_t>(std::atoi(val));
	}
	if (auto* val = std::getenv("MYSQL_USER"))
	{
		config.user = val;
	}
	else
	{
		config.user = "root";
	}
	if (auto* val = std::getenv("MYSQL_PASSWORD"))
	{
		config.password = val;
	}
	if (auto* val = std::getenv("MYSQL_DATABASE"))
	{
		config.database = val;
	}
	else
	{
		config.database = "hical_test";
	}

	config.minConnections = 1;
	config.maxConnections = 4;
	config.idleCheckInterval = std::chrono::seconds(0);
	return config;
}

// 检测 MySQL 是否可用
static bool isMysqlAvailable()
{
	auto config = getTestConfig();
	boost::asio::io_context ioCtx;
	bool available = false;

	coSpawn(ioCtx,
			[&config, &ioCtx, &available]() -> Awaitable<void>
			{
				try
				{
					auto conn = co_await MysqlConnection::create(ioCtx, config);
					available = co_await conn->ping();
				}
				catch (...)
				{
					available = false;
				}
			});
	ioCtx.run();
	return available;
}

	#define SKIP_IF_NO_MYSQL()                                                                \
		do                                                                                    \
		{                                                                                     \
			static bool checked = false;                                                      \
			static bool avail = false;                                                        \
			if (!checked)                                                                     \
			{                                                                                 \
				avail = isMysqlAvailable();                                                   \
				checked = true;                                                               \
			}                                                                                 \
			if (!avail)                                                                       \
			{                                                                                 \
				auto cfg = getTestConfig();                                                   \
				GTEST_SKIP() << "Cannot connect to MySQL at " << cfg.host << ":" << cfg.port; \
			}                                                                                 \
		}                                                                                     \
		while (0)

// ============ 基础连接测试 ============

TEST(MysqlIntegrationTest, PingSucceeds)
{
	SKIP_IF_NO_MYSQL();
	auto config = getTestConfig();
	boost::asio::io_context ioCtx;
	bool pingOk = false;

	coSpawn(ioCtx,
			[&config, &ioCtx, &pingOk]() -> Awaitable<void>
			{
				auto conn = co_await MysqlConnection::create(ioCtx, config);
				pingOk = co_await conn->ping();
			});
	ioCtx.run();

	EXPECT_TRUE(pingOk);
}

TEST(MysqlIntegrationTest, SelectOne)
{
	SKIP_IF_NO_MYSQL();
	auto config = getTestConfig();
	boost::asio::io_context ioCtx;
	DbResult result;

	coSpawn(ioCtx,
			[&config, &ioCtx, &result]() -> Awaitable<void>
			{
				auto conn = co_await MysqlConnection::create(ioCtx, config);
				result = co_await conn->query("SELECT 1 AS val");
			});
	ioCtx.run();

	ASSERT_EQ(result.size(), 1);
	auto valIdx = result.columnIndex("val");
	ASSERT_NE(valIdx, DbResult::npos) << "Expected column 'val' not found";
	EXPECT_EQ(result[0][valIdx], "1");
}

TEST(MysqlIntegrationTest, BackendName)
{
	SKIP_IF_NO_MYSQL();
	auto config = getTestConfig();
	boost::asio::io_context ioCtx;
	std::string backend;

	coSpawn(ioCtx,
			[&config, &ioCtx, &backend]() -> Awaitable<void>
			{
				auto conn = co_await MysqlConnection::create(ioCtx, config);
				backend = std::string(conn->backend());
			});
	ioCtx.run();

	EXPECT_EQ(backend, "mysql");
}

// ============ CRUD 测试 ============

TEST(MysqlIntegrationTest, CreateTableInsertAndSelect)
{
	SKIP_IF_NO_MYSQL();
	auto config = getTestConfig();
	boost::asio::io_context ioCtx;
	DbResult insertResult;
	DbResult selectResult;

	coSpawn(ioCtx,
			[&config, &ioCtx, &insertResult, &selectResult]() -> Awaitable<void>
			{
				auto conn = co_await MysqlConnection::create(ioCtx, config);

				co_await conn->execute("CREATE TEMPORARY TABLE hical_test_users ("
									   "  id INT AUTO_INCREMENT PRIMARY KEY,"
									   "  name VARCHAR(64) NOT NULL,"
									   "  age INT"
									   ")");

				insertResult = co_await conn->execute("INSERT INTO hical_test_users (name, age) VALUES ('Alice', 30)");

				selectResult = co_await conn->query("SELECT * FROM hical_test_users WHERE name = 'Alice'");
			});
	ioCtx.run();

	EXPECT_EQ(insertResult.affectedRows, 1);
	EXPECT_GT(insertResult.insertId, 0);

	ASSERT_EQ(selectResult.size(), 1);
	auto nameIdx = selectResult.columnIndex("name");
	auto ageIdx = selectResult.columnIndex("age");
	ASSERT_NE(nameIdx, DbResult::npos);
	ASSERT_NE(ageIdx, DbResult::npos);
	EXPECT_EQ(selectResult[0][nameIdx], "Alice");
	EXPECT_EQ(selectResult[0][ageIdx], "30");
}

// ============ 参数化查询测试 ============

TEST(MysqlIntegrationTest, ParameterizedQuery)
{
	SKIP_IF_NO_MYSQL();
	auto config = getTestConfig();
	boost::asio::io_context ioCtx;
	DbResult result;

	coSpawn(ioCtx,
			[&config, &ioCtx, &result]() -> Awaitable<void>
			{
				auto conn = co_await MysqlConnection::create(ioCtx, config);

				co_await conn->execute("CREATE TEMPORARY TABLE hical_param_test ("
									   "  id INT PRIMARY KEY, val VARCHAR(100))");

				std::vector<std::string> insertParams = {"1", "hello"};
				co_await conn->execute("INSERT INTO hical_param_test (id, val) VALUES (?, ?)", insertParams);

				std::vector<std::string> queryParams = {"1"};
				result = co_await conn->query("SELECT val FROM hical_param_test WHERE id = ?", queryParams);
			});
	ioCtx.run();

	ASSERT_EQ(result.size(), 1);
	auto valIdx2 = result.columnIndex("val");
	ASSERT_NE(valIdx2, DbResult::npos);
	EXPECT_EQ(result[0][valIdx2], "hello");
}

// ============ 事务测试 ============

TEST(MysqlIntegrationTest, TransactionCommit)
{
	SKIP_IF_NO_MYSQL();
	auto config = getTestConfig();
	boost::asio::io_context ioCtx;
	DbResult result;

	coSpawn(ioCtx,
			[&config, &ioCtx, &result]() -> Awaitable<void>
			{
				auto conn = co_await MysqlConnection::create(ioCtx, config);

				co_await conn->execute("CREATE TEMPORARY TABLE hical_tx_test (id INT PRIMARY KEY)");

				co_await conn->beginTransaction();
				EXPECT_TRUE(conn->inTransaction());

				co_await conn->execute("INSERT INTO hical_tx_test VALUES (1)");
				co_await conn->commit();
				EXPECT_FALSE(conn->inTransaction());

				result = co_await conn->query("SELECT * FROM hical_tx_test");
			});
	ioCtx.run();

	EXPECT_EQ(result.size(), 1);
}

TEST(MysqlIntegrationTest, TransactionRollback)
{
	SKIP_IF_NO_MYSQL();
	auto config = getTestConfig();
	boost::asio::io_context ioCtx;
	DbResult result;

	coSpawn(ioCtx,
			[&config, &ioCtx, &result]() -> Awaitable<void>
			{
				auto conn = co_await MysqlConnection::create(ioCtx, config);

				co_await conn->execute("CREATE TEMPORARY TABLE hical_rb_test (id INT PRIMARY KEY)");

				co_await conn->beginTransaction();
				co_await conn->execute("INSERT INTO hical_rb_test VALUES (1)");
				co_await conn->rollback();
				EXPECT_FALSE(conn->inTransaction());

				result = co_await conn->query("SELECT * FROM hical_rb_test");
			});
	ioCtx.run();

	EXPECT_EQ(result.size(), 0);
}

// ============ 连接池 + MySQL 测试 ============

TEST(MysqlIntegrationTest, ConnectionPoolWithMysql)
{
	SKIP_IF_NO_MYSQL();
	auto config = getTestConfig();
	boost::asio::io_context ioCtx;
	auto pool = std::make_shared<DbConnectionPool>(ioCtx, config, MysqlConnection::makeFactory());

	DbResult result;

	coSpawn(ioCtx,
			[pool, &result]() -> Awaitable<void>
			{
				co_await pool->init();

				auto conn = co_await pool->acquire();
				EXPECT_NE(conn, nullptr);
				EXPECT_EQ(conn->backend(), "mysql");

				result = co_await conn->query("SELECT 42 AS answer");

				pool->release(std::move(conn));
				co_await pool->shutdown();
			});
	ioCtx.run();

	ASSERT_EQ(result.size(), 1);
	auto answerIdx = result.columnIndex("answer");
	ASSERT_NE(answerIdx, DbResult::npos);
	EXPECT_EQ(result[0][answerIdx], "42");
}

#else // !HICAL_HAS_DATABASE

	#include <gtest/gtest.h>

TEST(MysqlIntegrationTest, Disabled)
{
	GTEST_SKIP() << "HICAL_HAS_DATABASE not defined";
}

#endif // HICAL_HAS_DATABASE
