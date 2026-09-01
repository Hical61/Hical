/**
 * @file test_pgsql_conn.cpp
 * @brief PostgreSQL 连接与静态 SQL 集成测试(需要真实 PG 实例)
 * 环境变量配置:
 *   HICAL_PG_HOST     - 默认 127.0.0.1
 *   HICAL_PG_PORT     - 默认 54329
 *   HICAL_PG_USER     - 默认 postgres
 *   HICAL_PG_PASSWORD - 默认空
 *   HICAL_PG_DATABASE - 默认 hical_test
 * 如果无法连接 PostgreSQL, 测试将被跳过(GTEST_SKIP)。
 */

#ifdef HICAL_HAS_PGSQL

	#include "db/DbConfig.h"
	#include "db/DbResult.h"
	#include "db/PgsqlConnection.h"
	#include "core/Coroutine.h"
	#include <gtest/gtest.h>
	#include <cstdlib>
	#include <memory>
	#include <string>
	#include <vector>

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
	config.port = 54329;
	if (auto* val = std::getenv("HICAL_PG_HOST"))
	{
		config.host = val;
	}
	if (auto* val = std::getenv("HICAL_PG_PORT"))
	{
		config.port = static_cast<uint16_t>(std::atoi(val));
	}
	if (auto* val = std::getenv("HICAL_PG_USER"))
	{
		config.user = val;
	}
	else
	{
		config.user = "postgres";
	}
	if (auto* val = std::getenv("HICAL_PG_PASSWORD"))
	{
		config.password = val;
	}
	if (auto* val = std::getenv("HICAL_PG_DATABASE"))
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

// 检测 PostgreSQL 是否可用
static bool isPgsqlAvailable()
{
	auto config = getTestConfig();
	boost::asio::io_context ioCtx;
	bool available = false;

	coSpawn(ioCtx,
			[&config, &ioCtx, &available]() -> Awaitable<void>
			{
				try
				{
					auto conn = co_await PgsqlConnection::create(ioCtx, config);
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

	#define SKIP_IF_NO_PGSQL()                                                                     \
		do                                                                                         \
		{                                                                                          \
			static bool checked = false;                                                           \
			static bool avail = false;                                                             \
			if (!checked)                                                                          \
			{                                                                                      \
				avail = isPgsqlAvailable();                                                        \
				checked = true;                                                                    \
			}                                                                                      \
			if (!avail)                                                                            \
			{                                                                                      \
				auto cfg = getTestConfig();                                                        \
				GTEST_SKIP() << "Cannot connect to PostgreSQL at " << cfg.host << ":" << cfg.port; \
			}                                                                                      \
		}                                                                                          \
		while (0)

// ============ 基础连接测试 ============

TEST(PgsqlConnTest, Backend_ReturnsPgsql)
{
	SKIP_IF_NO_PGSQL();
	auto config = getTestConfig();
	boost::asio::io_context ioCtx;
	std::string backend;

	coSpawn(ioCtx,
			[&config, &ioCtx, &backend]() -> Awaitable<void>
			{
				auto conn = co_await PgsqlConnection::create(ioCtx, config);
				backend = std::string(conn->backend());
			});
	ioCtx.run();

	EXPECT_EQ(backend, "pgsql");
}

TEST(PgsqlConnTest, Ping_Succeeds)
{
	SKIP_IF_NO_PGSQL();
	auto config = getTestConfig();
	boost::asio::io_context ioCtx;
	bool pingOk = false;

	coSpawn(ioCtx,
			[&config, &ioCtx, &pingOk]() -> Awaitable<void>
			{
				auto conn = co_await PgsqlConnection::create(ioCtx, config);
				pingOk = co_await conn->ping();
			});
	ioCtx.run();

	EXPECT_TRUE(pingOk);
}

TEST(PgsqlConnTest, SelectOne_ReturnsSingleCell)
{
	SKIP_IF_NO_PGSQL();
	auto config = getTestConfig();
	boost::asio::io_context ioCtx;
	DbResult result;

	coSpawn(ioCtx,
			[&config, &ioCtx, &result]() -> Awaitable<void>
			{
				auto conn = co_await PgsqlConnection::create(ioCtx, config);
				result = co_await conn->query("SELECT 1 AS one");
			});
	ioCtx.run();

	ASSERT_EQ(result.size(), 1);
	auto oneIdx = result.columnIndex("one");
	ASSERT_NE(oneIdx, DbResult::npos) << "Expected column 'one' not found";
	EXPECT_EQ(result[0][oneIdx], "1");
}

// ============ CRUD 测试 ============

TEST(PgsqlConnTest, CreateTableInsertAndSelect_RoundTrip)
{
	SKIP_IF_NO_PGSQL();
	auto config = getTestConfig();
	boost::asio::io_context ioCtx;
	DbResult insertResult;
	DbResult selectResult;

	coSpawn(ioCtx,
			[&config, &ioCtx, &insertResult, &selectResult]() -> Awaitable<void>
			{
				auto conn = co_await PgsqlConnection::create(ioCtx, config);

				// 清理可能残留的同名表，避免跨运行污染
				co_await conn->execute("DROP TABLE IF EXISTS hical_pg_test_users");

				co_await conn->execute("CREATE TABLE hical_pg_test_users ("
									   "  id SERIAL PRIMARY KEY,"
									   "  name VARCHAR(64) NOT NULL,"
									   "  age INT"
									   ")");

				insertResult =
					co_await conn->execute("INSERT INTO hical_pg_test_users (name, age) VALUES ('Alice', 30)");

				selectResult = co_await conn->query("SELECT name, age FROM hical_pg_test_users WHERE name = 'Alice'");
			});
	ioCtx.run();

	EXPECT_EQ(insertResult.affectedRows, 1);

	ASSERT_EQ(selectResult.size(), 1);
	auto nameIdx = selectResult.columnIndex("name");
	auto ageIdx = selectResult.columnIndex("age");
	ASSERT_NE(nameIdx, DbResult::npos);
	ASSERT_NE(ageIdx, DbResult::npos);
	EXPECT_EQ(selectResult[0][nameIdx], "Alice");
	EXPECT_EQ(selectResult[0][ageIdx], "30");
}

TEST(PgsqlConnTest, NullValue_RendersEmptyCell)
{
	SKIP_IF_NO_PGSQL();
	auto config = getTestConfig();
	boost::asio::io_context ioCtx;
	DbResult result;

	coSpawn(ioCtx,
			[&config, &ioCtx, &result]() -> Awaitable<void>
			{
				auto conn = co_await PgsqlConnection::create(ioCtx, config);
				result = co_await conn->query("SELECT NULL AS nothing");
			});
	ioCtx.run();

	ASSERT_EQ(result.size(), 1);
	auto idx = result.columnIndex("nothing");
	ASSERT_NE(idx, DbResult::npos);
	EXPECT_EQ(result[0][idx], "");
}

// ============ 事务与存活检测测试 ============

TEST(PgsqlConnTest, BeginCommit_PersistsData)
{
	SKIP_IF_NO_PGSQL();
	auto config = getTestConfig();
	boost::asio::io_context ioCtx;
	bool inTxBefore = false;
	bool inTxAfter = false;
	bool committed = false;
	DbResult selectResult;

	coSpawn(ioCtx,
			[&config, &ioCtx, &inTxBefore, &inTxAfter, &committed, &selectResult]() -> Awaitable<void>
			{
				auto conn = co_await PgsqlConnection::create(ioCtx, config);

				co_await conn->execute("DROP TABLE IF EXISTS hical_pg_tx_commit");
				co_await conn->execute("CREATE TABLE hical_pg_tx_commit ("
									   "  id SERIAL PRIMARY KEY,"
									   "  name VARCHAR(64) NOT NULL"
									   ")");

				co_await conn->beginTransaction();
				inTxBefore = conn->inTransaction();

				co_await conn->execute("INSERT INTO hical_pg_tx_commit (name) VALUES ('committed')");
				co_await conn->commit();
				inTxAfter = conn->inTransaction();
				committed = true;

				selectResult = co_await conn->query("SELECT name FROM hical_pg_tx_commit");
			});
	ioCtx.run();

	EXPECT_TRUE(inTxBefore);
	EXPECT_FALSE(inTxAfter);
	ASSERT_TRUE(committed);
	ASSERT_EQ(selectResult.size(), 1);
	auto nameIdx = selectResult.columnIndex("name");
	ASSERT_NE(nameIdx, DbResult::npos);
	EXPECT_EQ(selectResult[0][nameIdx], "committed");
}

TEST(PgsqlConnTest, BeginRollback_DiscardsData)
{
	SKIP_IF_NO_PGSQL();
	auto config = getTestConfig();
	boost::asio::io_context ioCtx;
	bool inTxBefore = false;
	bool inTxAfter = false;
	long long committedCount = -1;
	long long rolledBackCount = -1;

	coSpawn(ioCtx,
			[&config, &ioCtx, &inTxBefore, &inTxAfter, &committedCount, &rolledBackCount]() -> Awaitable<void>
			{
				auto conn = co_await PgsqlConnection::create(ioCtx, config);

				co_await conn->execute("DROP TABLE IF EXISTS hical_pg_tx_rollback");
				co_await conn->execute("CREATE TABLE hical_pg_tx_rollback ("
									   "  id SERIAL PRIMARY KEY,"
									   "  name VARCHAR(64) NOT NULL"
									   ")");

				// 先正常提交一条, 作为「基线」留在表里
				co_await conn->beginTransaction();
				co_await conn->execute("INSERT INTO hical_pg_tx_rollback (name) VALUES ('kept')");
				co_await conn->commit();

				// 第二次事务里插入后回滚, 该行应当消失
				co_await conn->beginTransaction();
				inTxBefore = conn->inTransaction();
				co_await conn->execute("INSERT INTO hical_pg_tx_rollback (name) VALUES ('discarded')");
				co_await conn->rollback();
				inTxAfter = conn->inTransaction();

				auto kept = co_await conn->query("SELECT COUNT(*) AS c FROM hical_pg_tx_rollback WHERE name = 'kept'");
				auto discarded =
					co_await conn->query("SELECT COUNT(*) AS c FROM hical_pg_tx_rollback WHERE name = 'discarded'");

				auto keptIdx = kept.columnIndex("c");
				auto discardedIdx = discarded.columnIndex("c");
				try
				{
					committedCount = std::stoll(kept[0][keptIdx]);
					rolledBackCount = std::stoll(discarded[0][discardedIdx]);
				}
				catch (...)
				{
					committedCount = -1;
					rolledBackCount = -1;
				}
			});
	ioCtx.run();

	EXPECT_TRUE(inTxBefore);
	EXPECT_FALSE(inTxAfter);
	// 回滚后: 基线保留, 回滚插入的那行不存在 => 数据未被持久化
	EXPECT_EQ(committedCount, 1);
	EXPECT_EQ(rolledBackCount, 0);
}

TEST(PgsqlConnTest, Ping_UpdatesLastPingTime)
{
	SKIP_IF_NO_PGSQL();
	auto config = getTestConfig();
	boost::asio::io_context ioCtx;
	bool initiallyAlive = false;
	bool pingOk = false;
	std::chrono::steady_clock::time_point lastPing;
	std::chrono::steady_clock::time_point lastActiveBefore;
	std::chrono::steady_clock::time_point lastActiveAfter;

	coSpawn(
		ioCtx,
		[&config, &ioCtx, &initiallyAlive, &pingOk, &lastPing, &lastActiveBefore, &lastActiveAfter]() -> Awaitable<void>
		{
			auto conn = co_await PgsqlConnection::create(ioCtx, config);
			initiallyAlive = conn->isAlive();

			lastActiveBefore = conn->lastPingTime();
			pingOk = co_await conn->ping();
			lastPing = conn->lastPingTime();
			lastActiveAfter = conn->lastActiveTime();
			(void)lastActiveAfter;
		});
	ioCtx.run();

	EXPECT_TRUE(initiallyAlive);
	EXPECT_TRUE(pingOk);
	// ping 成功后 lastPingTime 应晚于 ping 前的记录
	EXPECT_GT(lastPing, lastActiveBefore);
}

TEST(PgsqlConnTest, ParameterizedQuery_DollarPlaceholder_ReturnsSum)
{
	SKIP_IF_NO_PGSQL();
	auto config = getTestConfig();
	boost::asio::io_context ioCtx;
	DbResult result;

	coSpawn(ioCtx,
			[&config, &ioCtx, &result]() -> Awaitable<void>
			{
				auto conn = co_await PgsqlConnection::create(ioCtx, config);
				// PostgreSQL 用 $1 $2 占位，与 MySQL 的 ? 不同
				result = co_await conn->query("SELECT $1::int + $2::int AS sum", std::vector<std::string> {"20", "22"});
			});
	ioCtx.run();

	ASSERT_EQ(result.size(), 1);
	auto sumIdx = result.columnIndex("sum");
	ASSERT_NE(sumIdx, DbResult::npos);
	EXPECT_EQ(result[0][sumIdx], "42");
}

TEST(PgsqlConnTest, ParameterizedInsert_ReturningId_ReturnsInsertId)
{
	SKIP_IF_NO_PGSQL();
	auto config = getTestConfig();
	boost::asio::io_context ioCtx;
	DbResult insertResult;
	DbResult selectResult;

	coSpawn(ioCtx,
			[&config, &ioCtx, &insertResult, &selectResult]() -> Awaitable<void>
			{
				auto conn = co_await PgsqlConnection::create(ioCtx, config);

				co_await conn->execute("DROP TABLE IF EXISTS hical_pg_test_params");
				co_await conn->execute("CREATE TABLE hical_pg_test_params ("
									   "  id SERIAL PRIMARY KEY,"
									   "  name VARCHAR(64) NOT NULL"
									   ")");

				// INSERT ... RETURNING id 是第一行第一列拿 insertId 的唯一途径
				insertResult =
					co_await conn->execute("INSERT INTO hical_pg_test_params (name) VALUES ($1) RETURNING id",
										   std::vector<std::string> {"Bob"});

				selectResult = co_await conn->query("SELECT name FROM hical_pg_test_params WHERE id = $1::int",
													std::vector<std::string> {std::to_string(insertResult.insertId)});
			});
	ioCtx.run();

	EXPECT_GT(insertResult.insertId, uint64_t(0));
	ASSERT_EQ(selectResult.size(), 1);
	auto nameIdx = selectResult.columnIndex("name");
	ASSERT_NE(nameIdx, DbResult::npos);
	EXPECT_EQ(selectResult[0][nameIdx], "Bob");
}

// 普通 SELECT 首列碰巧是整数时, insertId 不应被误填为查询结果值。
// insertId 语义只属于 INSERT ... RETURNING, query 结果里它必须保持 0。
TEST(PgsqlConnTest, PlainSelect_IntegerFirstColumn_InsertIdStaysZero)
{
	SKIP_IF_NO_PGSQL();
	auto config = getTestConfig();
	boost::asio::io_context ioCtx;
	DbResult result;

	coSpawn(ioCtx,
			[&config, &ioCtx, &result]() -> Awaitable<void>
			{
				auto conn = co_await PgsqlConnection::create(ioCtx, config);

				co_await conn->execute("DROP TABLE IF EXISTS hical_pg_test_selid");
				co_await conn->execute("CREATE TABLE hical_pg_test_selid ("
									   "  id SERIAL PRIMARY KEY,"
									   "  name VARCHAR(64) NOT NULL"
									   ")");
				co_await conn->execute("INSERT INTO hical_pg_test_selid (name) VALUES ('alice')");

				result = co_await conn->query("SELECT id FROM hical_pg_test_selid WHERE name = 'alice'");
			});
	ioCtx.run();

	ASSERT_EQ(result.size(), 1);
	auto idIdx = result.columnIndex("id");
	ASSERT_NE(idIdx, DbResult::npos);
	// 行数据里 id 确实是 1, 但 insertId 必须保持 0, 不能被误当成插入主键
	EXPECT_EQ(result[0][idIdx], "1");
	EXPECT_EQ(result.insertId, uint64_t(0));
}

TEST(PgsqlConnTest, ParameterizedQuery_Injection_IsTreatedAsLiteral)
{
	SKIP_IF_NO_PGSQL();
	auto config = getTestConfig();
	boost::asio::io_context ioCtx;
	DbResult result;

	coSpawn(ioCtx,
			[&config, &ioCtx, &result]() -> Awaitable<void>
			{
				auto conn = co_await PgsqlConnection::create(ioCtx, config);

				co_await conn->execute("DROP TABLE IF EXISTS hical_pg_test_users");
				co_await conn->execute("CREATE TABLE hical_pg_test_users ("
									   "  id SERIAL PRIMARY KEY,"
									   "  name VARCHAR(64) NOT NULL"
									   ")");
				co_await conn->execute("INSERT INTO hical_pg_test_users (name) VALUES ('secure')");

				// 恶意参数含单引号与 OR 条件；走服务端预编译, 整体被当 name 的字符串字面量,
				// 匹配不到任何行 => 空结果集。若被拼进 SQL 则会返回 secure 行。
				result = co_await conn->query("SELECT name FROM hical_pg_test_users WHERE name = $1",
											  std::vector<std::string> {"' OR '1'='1"});
			});
	ioCtx.run();

	EXPECT_EQ(result.size(), 0);
}

// commit 真正失败（延期约束在 COMMIT 时才检查）时 inTransaction_ 必须保持 true，
// 让上层知道「还没提交成功」，可以 rollback 或重试，而不是误以为已提交。
TEST(PgsqlConnTest, CommitFailure_InTransactionStaysTrue)
{
	SKIP_IF_NO_PGSQL();
	auto config = getTestConfig();
	boost::asio::io_context ioCtx;
	bool inTxAfterBegin = false;
	bool commitFailed = false;
	bool inTxAfterFailedCommit = false;
	bool inTxAfterRollback = false;

	coSpawn(ioCtx,
			[&config, &ioCtx, &inTxAfterBegin, &commitFailed, &inTxAfterFailedCommit, &inTxAfterRollback]()
				-> Awaitable<void>
			{
				auto conn = co_await PgsqlConnection::create(ioCtx, config);

				// DEFERRABLE INITIALLY DEFERRED：冲突在 COMMIT 时才暴露，而非 INSERT 时。
				// 这样两次 INSERT 都成功，唯有 COMMIT 返回 FATAL_ERROR，是测 commit 失败
				// 的干净手段（普通 UNIQUE 冲突会在 INSERT 时就报错进入 aborted 态）。
				co_await conn->execute("DROP TABLE IF EXISTS hical_pg_tx_abort");
				co_await conn->execute("CREATE TABLE hical_pg_tx_abort ("
									   "  id INT,"
									   "  CONSTRAINT uq_id UNIQUE (id) DEFERRABLE INITIALLY DEFERRED"
									   ")");

				co_await conn->beginTransaction();
				inTxAfterBegin = conn->inTransaction();

				// 两条插入都成功（约束延期），冲突留给 COMMIT
				co_await conn->execute("INSERT INTO hical_pg_tx_abort (id) VALUES (1)");
				co_await conn->execute("INSERT INTO hical_pg_tx_abort (id) VALUES (1)");

				try
				{
					co_await conn->commit();
				}
				catch (...)
				{
					commitFailed = true;
				}
				inTxAfterFailedCommit = conn->inTransaction();

				co_await conn->rollback();
				inTxAfterRollback = conn->inTransaction();
			});
	ioCtx.run();

	EXPECT_TRUE(inTxAfterBegin);
	EXPECT_TRUE(commitFailed);
	// 关键断言：commit 失败后状态仍为「事务中」，不能误判为已提交
	EXPECT_TRUE(inTxAfterFailedCommit);
	// rollback 后复位
	EXPECT_FALSE(inTxAfterRollback);
}

// 违反约束的 INSERT 必须抛异常（而非静默返回空结果集），验证 PGRES_FATAL_ERROR
// 已通过 convertResults 传播，与 MysqlConnection 的行为对齐。
TEST(PgsqlConnTest, ConstraintViolation_Throws)
{
	SKIP_IF_NO_PGSQL();
	auto config = getTestConfig();
	boost::asio::io_context ioCtx;
	bool threw = false;

	coSpawn(ioCtx,
			[&config, &ioCtx, &threw]() -> Awaitable<void>
			{
				auto conn = co_await PgsqlConnection::create(ioCtx, config);

				co_await conn->execute("DROP TABLE IF EXISTS hical_pg_tx_cv");
				co_await conn->execute("CREATE TABLE hical_pg_tx_cv ("
									   "  id INT PRIMARY KEY"
									   ")");
				co_await conn->execute("INSERT INTO hical_pg_tx_cv (id) VALUES (1)");

				try
				{
					co_await conn->execute("INSERT INTO hical_pg_tx_cv (id) VALUES (1)");
				}
				catch (...)
				{
					threw = true;
				}
			});
	ioCtx.run();

	EXPECT_TRUE(threw);
}

// SELECT 无结果集（DML 走 PGRES_COMMAND_OK）时 columns 应为空、insertId 应为 0。
// 已由 CreateTable... 的 insertResult 验证 affectedRows，这里补 insertId/columns 边界。
TEST(PgsqlConnTest, Insert_NoReturning_InsertIdAndColumnsEmpty)
{
	SKIP_IF_NO_PGSQL();
	auto config = getTestConfig();
	boost::asio::io_context ioCtx;
	DbResult insertResult;

	coSpawn(ioCtx,
			[&config, &ioCtx, &insertResult]() -> Awaitable<void>
			{
				auto conn = co_await PgsqlConnection::create(ioCtx, config);

				co_await conn->execute("DROP TABLE IF EXISTS hical_pg_test_dml");
				co_await conn->execute("CREATE TABLE hical_pg_test_dml ("
									   "  id SERIAL PRIMARY KEY,"
									   "  name VARCHAR(64) NOT NULL"
									   ")");

				// 不带 RETURNING 的 INSERT 走 PGRES_COMMAND_OK，没有结果集
				insertResult = co_await conn->execute("INSERT INTO hical_pg_test_dml (name) VALUES ('x')");
			});
	ioCtx.run();

	EXPECT_EQ(insertResult.affectedRows, uint64_t(1));
	EXPECT_TRUE(insertResult.columns.empty());
	EXPECT_EQ(insertResult.insertId, uint64_t(0));
}

#else // !HICAL_HAS_PGSQL

	#include <gtest/gtest.h>

TEST(PgsqlConnTest, Disabled)
{
	GTEST_SKIP() << "HICAL_HAS_PGSQL not defined";
}

#endif // HICAL_HAS_PGSQL
