#include "core/Session.h"
#include <gtest/gtest.h>
#include <thread>

using namespace hical;

// ============ Session 数据存储测试 ============

TEST(SessionTest, SetAndGet)
{
	Session s("test-id");
	s.set("user", std::string("alice"));
	s.set("age", 30);

	auto user = s.get<std::string>("user");
	ASSERT_TRUE(user.has_value());
	EXPECT_EQ(*user, "alice");

	auto age = s.get<int>("age");
	ASSERT_TRUE(age.has_value());
	EXPECT_EQ(*age, 30);
}

TEST(SessionTest, GetMissingKeyReturnsNullopt)
{
	Session s("test-id");
	EXPECT_FALSE(s.get<std::string>("nonexistent").has_value());
}

TEST(SessionTest, GetWrongTypeReturnsNullopt)
{
	Session s("test-id");
	s.set("num", 42);
	// 正确类型
	EXPECT_TRUE(s.get<int>("num").has_value());
	// 错误类型
	EXPECT_FALSE(s.get<std::string>("num").has_value());
}

TEST(SessionTest, Has)
{
	Session s("test-id");
	EXPECT_FALSE(s.has("key"));
	s.set("key", std::string("value"));
	EXPECT_TRUE(s.has("key"));
}

TEST(SessionTest, Remove)
{
	Session s("test-id");
	s.set("key", std::string("val"));
	EXPECT_TRUE(s.has("key"));
	s.remove("key");
	EXPECT_FALSE(s.has("key"));
}

TEST(SessionTest, Clear)
{
	Session s("test-id");
	s.set("a", 1);
	s.set("b", 2);
	s.clear();
	EXPECT_FALSE(s.has("a"));
	EXPECT_FALSE(s.has("b"));
}

TEST(SessionTest, DirtyFlag)
{
	Session s("test-id");
	EXPECT_FALSE(s.isDirty());
	s.markDirty();
	EXPECT_TRUE(s.isDirty());
}

// ============ SessionManager 测试 ============

TEST(SessionManagerTest, CreateAndFind)
{
	SessionManager mgr;
	auto s = mgr.create();
	ASSERT_TRUE(s != nullptr);
	EXPECT_EQ(s->id().size(), 32u); // 128 位 = 32 位十六进制

	auto found = mgr.find(s->id());
	ASSERT_TRUE(found != nullptr);
	EXPECT_EQ(found->id(), s->id());
}

TEST(SessionManagerTest, FindNonExistent)
{
	SessionManager mgr;
	auto result = mgr.find("nonexistent-session-id");
	EXPECT_EQ(result, nullptr);
}

TEST(SessionManagerTest, Destroy)
{
	SessionManager mgr;
	auto s = mgr.create();
	auto id = s->id();
	EXPECT_NE(mgr.find(id), nullptr);

	mgr.destroy(id);
	EXPECT_EQ(mgr.find(id), nullptr);
}

TEST(SessionManagerTest, Count)
{
	SessionManager mgr;
	EXPECT_EQ(mgr.count(), 0u);
	auto s1 = mgr.create();
	auto s2 = mgr.create();
	EXPECT_EQ(mgr.count(), 2u);
	mgr.destroy(s1->id());
	EXPECT_EQ(mgr.count(), 1u);
}

TEST(SessionManagerTest, SessionIdsAreUnique)
{
	SessionManager mgr;
	std::vector<std::string> ids;
	for (int i = 0; i < 100; ++i)
	{
		ids.push_back(mgr.create()->id());
	}
	// 检查无重复
	std::sort(ids.begin(), ids.end());
	EXPECT_EQ(std::unique(ids.begin(), ids.end()), ids.end());
}

TEST(SessionManagerTest, GcRemovesExpiredSessions)
{
	SessionOptions opts;
	opts.maxAge = 1; // 1 秒过期
	SessionManager mgr(opts);

	auto s = mgr.create();
	EXPECT_EQ(mgr.count(), 1u);

	// 等待超过 maxAge
	std::this_thread::sleep_for(std::chrono::milliseconds(1100));

	mgr.gc();
	EXPECT_EQ(mgr.count(), 0u);
}

TEST(SessionManagerTest, FindExpiredSessionReturnsNull)
{
	SessionOptions opts;
	opts.maxAge = 1; // 1 秒
	SessionManager mgr(opts);

	auto s = mgr.create();
	auto id = s->id();

	std::this_thread::sleep_for(std::chrono::milliseconds(1100));

	EXPECT_EQ(mgr.find(id), nullptr);
}

// ============ Session 中间件集成测试（逻辑层） ============

TEST(SessionManagerTest, ThreadSafeCreateAndFind)
{
	SessionManager mgr;
	constexpr int nThreads = 8;
	constexpr int nOps = 50;
	std::vector<std::thread> threads;
	std::atomic<int> created {0};

	for (int i = 0; i < nThreads; ++i)
	{
		threads.emplace_back(
			[&]()
			{
				for (int j = 0; j < nOps; ++j)
				{
					auto s = mgr.create();
					s->set("x", j);
					++created;
				}
			});
	}
	for (auto& t : threads)
	{
		t.join();
	}
	EXPECT_EQ(mgr.count(), static_cast<size_t>(nThreads * nOps));
}
