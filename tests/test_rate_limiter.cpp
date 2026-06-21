/**
 * @file test_rate_limiter.cpp
 * @brief Token Bucket 速率限制中间件测试
 */

#include "core/RateLimiter.h"
#include "core/Middleware.h"
#include "core/HttpRequest.h"
#include "core/HttpResponse.h"
#include "core/HttpTypes.h"
#include <gtest/gtest.h>
#include <chrono>
#include <thread>

using namespace hical;

// ──────────────────────────────────────────────
// 1. 基本限流：rate=10, burst=10, 1 秒内消费 11 个应被限流
// ──────────────────────────────────────────────
TEST(RateLimiterTest, BasicRateLimit)
{
	RateLimiter limiter(RateLimiterOptions {.config = {10.0, 10.0},
											.keyExtractor = [](const HttpRequest&)
											{
												return std::string("test-key");
											}});

	auto now = std::chrono::steady_clock::now();

	// 前 10 个应通过
	for (int i = 0; i < 10; ++i)
	{
		EXPECT_TRUE(limiter.check("test-key", now));
	}

	// 第 11 个应被限流
	EXPECT_FALSE(limiter.check("test-key", now));
}

// ──────────────────────────────────────────────
// 2. 桶会随时间 refill token
// ──────────────────────────────────────────────
TEST(RateLimiterTest, TokenRefillOverTime)
{
	RateLimiter limiter(RateLimiterOptions {.config = {10.0, 10.0},
											.keyExtractor = [](const HttpRequest&)
											{
												return std::string("test-key");
											}});

	auto now = std::chrono::steady_clock::now();

	// 消费 10 个 token，桶空
	for (int i = 0; i < 10; ++i)
	{
		EXPECT_TRUE(limiter.check("test-key", now));
	}
	EXPECT_FALSE(limiter.check("test-key", now));

	// 假装过了 100ms（rate=10 tokens/s → 应恢复 ~1 token）
	now += std::chrono::milliseconds(100);
	EXPECT_TRUE(limiter.check("test-key", now));
}

// ──────────────────────────────────────────────
// 3. burst 允许瞬时超出平均速率
// ──────────────────────────────────────────────
TEST(RateLimiterTest, BurstAllowsShortSpike)
{
	RateLimiter limiter(RateLimiterOptions {.config = {1.0, 5.0}, // 每秒 1 个，burst 5
											.keyExtractor = [](const HttpRequest&)
											{
												return std::string("test-key");
											}});

	auto now = std::chrono::steady_clock::now();

	// 瞬时消费 5 个（burst 容量）
	for (int i = 0; i < 5; ++i)
	{
		EXPECT_TRUE(limiter.check("test-key", now));
	}

	// 第 6 个应被限流（rate=1, 桶还没 refill）
	EXPECT_FALSE(limiter.check("test-key", now));

	// 等 1 秒，恢复 ~1 个 token
	now += std::chrono::seconds(1);
	EXPECT_TRUE(limiter.check("test-key", now));
}

// ──────────────────────────────────────────────
// 4. 不同 key 独立限流
// ──────────────────────────────────────────────
TEST(RateLimiterTest, IndependentKeys)
{
	RateLimiter limiter(RateLimiterOptions {.config = {1.0, 1.0}, // 每秒 1 个，burst 1
											.keyExtractor = [](const HttpRequest&)
											{
												return std::string("test-key");
											}});

	auto now = std::chrono::steady_clock::now();

	EXPECT_TRUE(limiter.check("key-a", now));
	EXPECT_FALSE(limiter.check("key-a", now)); // key-a 被限流

	EXPECT_TRUE(limiter.check("key-b", now));  // key-b 独立桶
	EXPECT_FALSE(limiter.check("key-b", now)); // key-b 也被限流
}

// ──────────────────────────────────────────────
// 5. 中间件工厂：低于速率返回 nullopt
// ──────────────────────────────────────────────
TEST(RateLimiterTest, SyncBeforeHandler_AllowsRequest)
{
	auto mw = makeRateLimiterMiddleware(RateLimiterOptions {.config = {1000.0, 1000.0}, // 极高速率，确保通过
															.keyExtractor = [](const HttpRequest&)
															{
																return std::string("allow-test");
															}});

	HttpRequest req;
	req.setMethod(HttpMethod::hGet);
	req.setTarget("/api/test");

	auto result = mw(req);
	// 通过 → nullopt
	EXPECT_FALSE(result.has_value());
}

// ──────────────────────────────────────────────
// 6. 中间件工厂：超限返回 429 + 限流头
// ──────────────────────────────────────────────
TEST(RateLimiterTest, SyncBeforeHandler_Returns429)
{
	auto mw = makeRateLimiterMiddleware(RateLimiterOptions {.config = {1.0, 1.0}, // 每秒 1 个，burst 1
															.keyExtractor = [](const HttpRequest&)
															{
																return std::string("429-test");
															}});

	HttpRequest req;
	req.setMethod(HttpMethod::hGet);
	req.setTarget("/api/test");

	// 第一次通过
	auto result1 = mw(req);
	EXPECT_FALSE(result1.has_value());

	// 第二次被限流
	auto result2 = mw(req);
	ASSERT_TRUE(result2.has_value());
	EXPECT_EQ(result2->statusCode(), HttpStatusCode::hTooManyRequests);
	// 应有限流头
	EXPECT_FALSE(result2->header("Retry-After").empty());
	EXPECT_FALSE(result2->header("X-RateLimit-Limit").empty());
}

// ──────────────────────────────────────────────
// 7. 默认 keyExtractor：X-Forwarded-For
// ──────────────────────────────────────────────
TEST(RateLimiterTest, DefaultKeyExtractor_XForwardedFor)
{
	// 创建一个低速率的限流器，用默认 keyExtractor
	auto mw = makeRateLimiterMiddleware(RateLimiterOptions {.config = {1.0, 1.0}});

	HttpRequest req;
	req.setMethod(HttpMethod::hGet);
	req.setTarget("/api/test");
	req.setHeader("X-Forwarded-For", "10.0.0.1");

	// 第一次通过
	auto result1 = mw(req);
	EXPECT_FALSE(result1.has_value());

	// 第二次被限流
	auto result2 = mw(req);
	ASSERT_TRUE(result2.has_value());
	EXPECT_EQ(result2->statusCode(), HttpStatusCode::hTooManyRequests);
}

// ──────────────────────────────────────────────
// 8. 默认 keyExtractor：remote_addr 属性优先
// ──────────────────────────────────────────────
TEST(RateLimiterTest, DefaultKeyExtractor_RemoteAddrAttr)
{
	auto mw = makeRateLimiterMiddleware(RateLimiterOptions {.config = {1.0, 1.0}});

	HttpRequest req;
	req.setMethod(HttpMethod::hGet);
	req.setTarget("/api/test");
	req.setAttribute(kRemoteAddrKey, std::string("192.168.1.100"));

	// 第一次通过
	auto result1 = mw(req);
	EXPECT_FALSE(result1.has_value());

	// 同一 IP 的第二次被限流
	auto result2 = mw(req);
	ASSERT_TRUE(result2.has_value());
	EXPECT_EQ(result2->statusCode(), HttpStatusCode::hTooManyRequests);
}

// ──────────────────────────────────────────────
// 9. 多 key 不互相干扰
// ──────────────────────────────────────────────
TEST(RateLimiterTest, MultipleKeysDontInterfere)
{
	auto mw = makeRateLimiterMiddleware(RateLimiterOptions {.config = {2.0, 2.0},
															.keyExtractor = [](const HttpRequest& req)
															{
																return std::string(req.header("X-Api-Key"));
															}});

	HttpRequest reqA, reqB;
	reqA.setMethod(HttpMethod::hGet);
	reqA.setTarget("/api/data");
	reqA.setHeader("X-Api-Key", "key-a");

	reqB.setMethod(HttpMethod::hGet);
	reqB.setTarget("/api/data");
	reqB.setHeader("X-Api-Key", "key-b");

	// 各自第 1 次通过
	EXPECT_FALSE(mw(reqA).has_value());
	EXPECT_FALSE(mw(reqB).has_value());

	// key-a 第 2 次通过（burst=2）
	EXPECT_FALSE(mw(reqA).has_value());

	// key-b 第 2 次通过（独立桶，不受 key-a 影响）
	EXPECT_FALSE(mw(reqB).has_value());

	// key-a burst 耗尽，第 3 次被限流
	auto resultA3 = mw(reqA);
	EXPECT_TRUE(resultA3.has_value());
	EXPECT_EQ(resultA3->statusCode(), HttpStatusCode::hTooManyRequests);

	// key-b 也耗尽 burst，第 3 次被限流
	auto resultB3 = mw(reqB);
	EXPECT_TRUE(resultB3.has_value());
	EXPECT_EQ(resultB3->statusCode(), HttpStatusCode::hTooManyRequests);
}

// ──────────────────────────────────────────────
// 10. 最大条目限制（maxEntries）
// ──────────────────────────────────────────────
TEST(RateLimiterTest, MaxEntriesLimit)
{
	RateLimiter limiter(RateLimiterOptions {
		.config = {100.0, 100.0},
		.keyExtractor =
			[](const HttpRequest&)
		{
			return std::string("test-key");
		},
		.maxEntries = 2,
	});

	auto now = std::chrono::steady_clock::now();

	// 前 2 个 key 应通过
	EXPECT_TRUE(limiter.check("key1", now));
	EXPECT_TRUE(limiter.check("key2", now));

	// 第 3 个 key 应被限流（超出 maxEntries）
	// 注意：check 会创建新桶失败，返回 false
	EXPECT_FALSE(limiter.check("key3", now));
}

// ──────────────────────────────────────────────
// 11. 高并发场景锁竞争（简单验证不崩溃）
// ──────────────────────────────────────────────
TEST(RateLimiterTest, ConcurrencyStability)
{
	RateLimiter limiter(RateLimiterOptions {.config = {10000.0, 10000.0}, // 极高速率，聚焦并发而非限流行为
											.keyExtractor = [](const HttpRequest&)
											{
												return std::string("test");
											}});

	const int kThreads = 8;
	const int kCallsPerThread = 1000;
	std::atomic<int> allowedCount {0};
	std::vector<std::thread> threads;

	auto now = std::chrono::steady_clock::now();

	for (int t = 0; t < kThreads; ++t)
	{
		threads.emplace_back(
			[&, t]()
			{
				for (int i = 0; i < kCallsPerThread; ++i)
				{
					auto key = "concurrent-" + std::to_string(t % 4);
					if (limiter.check(key, now))
					{
						allowedCount.fetch_add(1, std::memory_order_relaxed);
					}
				}
			});
	}

	for (auto& th : threads)
	{
		th.join();
	}

	// 不崩溃、不死锁即可
	EXPECT_GT(allowedCount.load(), 0);
}
