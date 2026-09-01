/**
 * @file test_pgsql_unit.cpp
 * @brief PgStmtCache LRU 缓存纯单元测试(不依赖真实 PG, 不连网)
 */

#ifdef HICAL_HAS_PGSQL

	#include "db/PgStmtCache.h"
	#include <gtest/gtest.h>
	#include <string>

using namespace hical::db;

// ============ 缓存命中 / 未命中 ============

TEST(PgStmtCacheTest, Find_MissingKey_ReturnsNullopt)
{
	PgStmtCache cache(4);

	EXPECT_FALSE(cache.find("SELECT 1").has_value());
}

TEST(PgStmtCacheTest, InsertAndFind_Hit_ReturnsStmtName)
{
	PgStmtCache cache(4);

	cache.insert("SELECT $1", "stmt_1");

	auto name = cache.find("SELECT $1");
	ASSERT_TRUE(name.has_value());
	EXPECT_EQ(*name, "stmt_1");
}

TEST(PgStmtCacheTest, Insert_DuplicateSql_UpdatesName)
{
	PgStmtCache cache(4);

	cache.insert("SELECT $1", "stmt_1");
	// 同名 SQL 用新名字覆盖（例如失效重试后重新 prepare 拿到新名字）
	cache.insert("SELECT $1", "stmt_2");

	auto name = cache.find("SELECT $1");
	ASSERT_TRUE(name.has_value());
	EXPECT_EQ(*name, "stmt_2");
	EXPECT_EQ(cache.size(), size_t(1));
}

// ============ LRU 淘汰 ============

TEST(PgStmtCacheTest, Evict_WorstRecentlyUsed_WhenFull)
{
	PgStmtCache cache(3);

	cache.insert("SELECT $1", "stmt_1");
	cache.insert("SELECT $2", "stmt_2");
	cache.insert("SELECT $3", "stmt_3");

	// 访问 stmt_1，让它从 LRU 位置提升到 MRU
	EXPECT_TRUE(cache.find("SELECT $1").has_value());

	// 插入第四个，淘汰 LRU：此时最久未用的是 SELECT $2
	cache.insert("SELECT $4", "stmt_4");

	EXPECT_EQ(cache.size(), size_t(3));
	EXPECT_FALSE(cache.find("SELECT $2").has_value()); // 被淘汰
	EXPECT_TRUE(cache.find("SELECT $1").has_value());  // 因访问过，保留
	EXPECT_TRUE(cache.find("SELECT $3").has_value());
	EXPECT_TRUE(cache.find("SELECT $4").has_value());
}

TEST(PgStmtCacheTest, Insert_WhenCacheDisabled_DoesNotStore)
{
	// maxSize = 0 表示禁用缓存
	PgStmtCache cache(0);

	cache.insert("SELECT $1", "stmt_1");

	EXPECT_EQ(cache.size(), size_t(0));
	EXPECT_FALSE(cache.find("SELECT $1").has_value());
}

// ============ erase / clear ============

TEST(PgStmtCacheTest, Erase_RemovesEntry)
{
	PgStmtCache cache(4);

	cache.insert("SELECT $1", "stmt_1");
	EXPECT_EQ(cache.size(), size_t(1));

	cache.erase("SELECT $1");

	EXPECT_EQ(cache.size(), size_t(0));
	EXPECT_FALSE(cache.find("SELECT $1").has_value());
}

TEST(PgStmtCacheTest, Erase_MissingKey_NoCrash)
{
	PgStmtCache cache(4);

	cache.erase("NOT EXIST");

	EXPECT_EQ(cache.size(), size_t(0));
}

TEST(PgStmtCacheTest, Clear_EmptiesCache)
{
	PgStmtCache cache(4);

	cache.insert("SELECT $1", "stmt_1");
	cache.insert("SELECT $2", "stmt_2");
	cache.clear();

	EXPECT_EQ(cache.size(), size_t(0));
	EXPECT_FALSE(cache.find("SELECT $1").has_value());
	EXPECT_FALSE(cache.find("SELECT $2").has_value());
}

// ============ 边界场景补充 ============

TEST(PgStmtCacheTest, CapacityOne_InsertSecondEvictsFirst)
{
	PgStmtCache cache(1);

	cache.insert("SELECT $1", "stmt_1");
	EXPECT_EQ(cache.size(), size_t(1));

	// 容量为 1：插入第二条必须淘汰第一条
	cache.insert("SELECT $2", "stmt_2");
	EXPECT_EQ(cache.size(), size_t(1));
	EXPECT_FALSE(cache.find("SELECT $1").has_value());
	EXPECT_TRUE(cache.find("SELECT $2").has_value());
}

TEST(PgStmtCacheTest, InsertFullThenUpdateExisting_DoesNotEvictOther)
{
	PgStmtCache cache(2);

	cache.insert("SELECT $1", "stmt_1");
	cache.insert("SELECT $2", "stmt_2");
	EXPECT_EQ(cache.size(), size_t(2));

	// 缓存已满时更新已存在的键，走「更新名字」分支而非「先淘汰」，不会误淘汰其它条目
	cache.insert("SELECT $1", "stmt_1_updated");
	EXPECT_EQ(cache.size(), size_t(2));
	EXPECT_TRUE(cache.find("SELECT $2").has_value());
	auto name = cache.find("SELECT $1");
	ASSERT_TRUE(name.has_value());
	EXPECT_EQ(*name, "stmt_1_updated");
}

TEST(PgStmtCacheTest, EraseThenReinsert_Works)
{
	PgStmtCache cache(4);

	cache.insert("SELECT $1", "stmt_1");
	cache.erase("SELECT $1");
	cache.insert("SELECT $1", "stmt_1_reborn");

	EXPECT_EQ(cache.size(), size_t(1));
	auto name = cache.find("SELECT $1");
	ASSERT_TRUE(name.has_value());
	EXPECT_EQ(*name, "stmt_1_reborn");
}

TEST(PgStmtCacheTest, Find_WithStdStringKey_TransparentHashMatches)
{
	PgStmtCache cache(4);

	cache.insert("SELECT $1", "stmt_1");

	// 用 std::string（而非 string_view）作为查找键，验证透明哈希两种 key 类型等价
	std::string sqlKey = "SELECT $1";
	auto name = cache.find(sqlKey);
	ASSERT_TRUE(name.has_value());
	EXPECT_EQ(*name, "stmt_1");
}

#else // !HICAL_HAS_PGSQL

	#include <gtest/gtest.h>

TEST(PgStmtCacheTest, Disabled)
{
	GTEST_SKIP() << "HICAL_HAS_PGSQL not defined";
}

#endif // HICAL_HAS_PGSQL
