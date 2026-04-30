#ifdef HICAL_HAS_DATABASE

	#include "db/StmtCache.h"
	#include <gtest/gtest.h>

using namespace hical::db;

// boost::mysql::statement 默认构造后 valid()==false，但可安全存储和移动。
// 这些测试仅验证 LRU 缓存逻辑，不涉及真实 MySQL 连接。

TEST(StmtCacheTest, EmptyCache)
{
	StmtCache cache(8);
	EXPECT_EQ(cache.size(), 0);
	EXPECT_EQ(cache.maxSize(), 8);
	EXPECT_EQ(cache.find("SELECT 1"), nullptr);
}

TEST(StmtCacheTest, InsertAndFind)
{
	StmtCache cache(8);
	boost::mysql::statement stmt;

	auto evicted = cache.insert("SELECT 1", stmt);
	EXPECT_FALSE(evicted.has_value());
	EXPECT_EQ(cache.size(), 1);

	auto* found = cache.find("SELECT 1");
	EXPECT_NE(found, nullptr);
}

TEST(StmtCacheTest, FindReturnsNullptrOnMiss)
{
	StmtCache cache(8);
	boost::mysql::statement stmt;
	cache.insert("SELECT 1", stmt);

	EXPECT_EQ(cache.find("SELECT 2"), nullptr);
}

TEST(StmtCacheTest, LruEviction)
{
	StmtCache cache(3);
	boost::mysql::statement stmt;

	cache.insert("A", stmt);
	cache.insert("B", stmt);
	cache.insert("C", stmt);
	EXPECT_EQ(cache.size(), 3);

	// 插入 D，应淘汰 LRU = A
	auto evicted = cache.insert("D", stmt);
	EXPECT_TRUE(evicted.has_value());
	EXPECT_EQ(cache.size(), 3);
	EXPECT_EQ(cache.find("A"), nullptr); // A 已被淘汰
	EXPECT_NE(cache.find("D"), nullptr);
}

TEST(StmtCacheTest, FindPromotesToMru)
{
	StmtCache cache(3);
	boost::mysql::statement stmt;

	cache.insert("A", stmt); // LRU: A(MRU)
	cache.insert("B", stmt); // LRU: A, B(MRU)
	cache.insert("C", stmt); // LRU: A, B, C(MRU)

	// 访问 A，提升到 MRU
	cache.find("A"); // LRU: B, C, A(MRU)

	// 插入 D，应淘汰 LRU = B（不是 A）
	auto evicted = cache.insert("D", stmt);
	EXPECT_TRUE(evicted.has_value());
	EXPECT_EQ(cache.find("B"), nullptr); // B 被淘汰
	EXPECT_NE(cache.find("A"), nullptr); // A 因 find 被提升，仍在
}

TEST(StmtCacheTest, DuplicateInsertUpdates)
{
	StmtCache cache(8);
	boost::mysql::statement stmt1;
	boost::mysql::statement stmt2;

	cache.insert("SQL", stmt1);
	EXPECT_EQ(cache.size(), 1);

	// 重复插入相同 key，应更新而非新增
	auto evicted = cache.insert("SQL", stmt2);
	EXPECT_FALSE(evicted.has_value());
	EXPECT_EQ(cache.size(), 1);

	auto* found = cache.find("SQL");
	EXPECT_NE(found, nullptr);
}

TEST(StmtCacheTest, EraseExisting)
{
	StmtCache cache(8);
	boost::mysql::statement stmt;

	cache.insert("SQL", stmt);
	EXPECT_EQ(cache.size(), 1);

	auto erased = cache.erase("SQL");
	EXPECT_TRUE(erased.has_value());
	EXPECT_EQ(cache.size(), 0);
	EXPECT_EQ(cache.find("SQL"), nullptr);
}

TEST(StmtCacheTest, EraseNonExisting)
{
	StmtCache cache(8);
	auto erased = cache.erase("SQL");
	EXPECT_FALSE(erased.has_value());
}

TEST(StmtCacheTest, ClearReturnsAll)
{
	StmtCache cache(8);
	boost::mysql::statement stmt;

	cache.insert("A", stmt);
	cache.insert("B", stmt);
	cache.insert("C", stmt);
	EXPECT_EQ(cache.size(), 3);

	auto all = cache.clear();
	EXPECT_EQ(all.size(), 3);
	EXPECT_EQ(cache.size(), 0);
}

TEST(StmtCacheTest, DisabledCacheReturnsSameStatement)
{
	StmtCache cache(0); // 容量 0 = 禁用
	boost::mysql::statement stmt;

	// insert 应返回传入的 statement（不缓存）
	auto evicted = cache.insert("SQL", stmt);
	EXPECT_TRUE(evicted.has_value());
	EXPECT_EQ(cache.size(), 0);

	// find 始终返回 nullptr
	EXPECT_EQ(cache.find("SQL"), nullptr);
}

#else

	// 未启用 HICAL_HAS_DATABASE 时，仅添加一个占位测试以避免空测试套件
	#include <gtest/gtest.h>

TEST(StmtCacheTest, SkippedWithoutDatabase)
{
	GTEST_SKIP() << "HICAL_HAS_DATABASE not enabled";
}

#endif // HICAL_HAS_DATABASE
