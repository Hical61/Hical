/**
 * @file test_compile_time_chain.cpp
 * @brief 编译期中间件链与运行时 buildOptimizedChain 语义一致性测试
 */

#include "core/CompileTimeChain.h"
#include "core/Coroutine.h"
#include "asio/AsioEventLoop.h"
#include <gtest/gtest.h>
#include <atomic>
#include <vector>

using namespace hical;

namespace
{

	/* ===== 测试辅助工具 ===== */

	template <typename F>
	auto runCoroutine(F&& f)
	{
		using ReturnType = typename std::invoke_result_t<F>::value_type;

		boost::asio::io_context ioCtx;
		std::optional<ReturnType> result;

		coSpawn(ioCtx,
				[&]() -> Awaitable<void>
				{
					result = co_await f();
				});

		ioCtx.run();
		return result;
	}

	/**
	 * @brief 用 std::vector 记录执行顺序的全局状态
	 * 各个测试定义自己的 static 变量，test fixture SetUp/TearDown 中清理。
	 */
	class CompileTimeChainTest : public ::testing::Test
	{
	protected:
		void SetUp() override
		{
			mwCalls_.clear();
		}

		void TearDown() override
		{
			mwCalls_.clear();
		}

		std::vector<std::string> mwCalls_;
	};

} // namespace

/* ===== 场景 1：全 Sync 链（仅 before）洋葱执行顺序 ===== */

TEST_F(CompileTimeChainTest, AllSync_ExecutionOrder)
{
	/* 三个 SyncBefore 中间件：按注册顺序依次执行 before */
	constexpr auto kSyncB1 = [](HttpRequest&) -> SyncMiddlewareResult
	{
		return std::nullopt;
	};
	constexpr auto kSyncB2 = [](HttpRequest&) -> SyncMiddlewareResult
	{
		return std::nullopt;
	};
	constexpr auto kSyncB3 = [](HttpRequest&) -> SyncMiddlewareResult
	{
		return std::nullopt;
	};

	/* 编译期链 */
	auto compileTimeChain =
		buildCompileTimeChain<CompileTimeSyncMw<kSyncB1>, CompileTimeSyncMw<kSyncB2>, CompileTimeSyncMw<kSyncB3>>(
			[](HttpRequest&) -> Awaitable<HttpResponse>
			{
				co_return HttpResponse::ok("compile");
			});

	/* 运行期链 */
	std::vector<MiddlewareEntry> entries;
	entries.push_back({MiddlewareEntry::Type::hSync, "s1", nullptr, kSyncB1, nullptr});
	entries.push_back({MiddlewareEntry::Type::hSync, "s2", nullptr, kSyncB2, nullptr});
	entries.push_back({MiddlewareEntry::Type::hSync, "s3", nullptr, kSyncB3, nullptr});
	auto runtimeChain = MiddlewarePipeline::buildOptimizedChain(entries,
																[](HttpRequest&) -> Awaitable<HttpResponse>
																{
																	co_return HttpResponse::ok("runtime");
																});

	HttpRequest req;
	req.setMethod(HttpMethod::hGet);
	req.setTarget("/test");

	auto ctResult = runCoroutine(
		[&]()
		{
			return compileTimeChain(req);
		});
	auto rtResult = runCoroutine(
		[&]()
		{
			return runtimeChain(req);
		});

	ASSERT_TRUE(ctResult.has_value());
	ASSERT_TRUE(rtResult.has_value());
	EXPECT_EQ(ctResult->body(), "compile");
	EXPECT_EQ(rtResult->body(), "runtime");
}

/* ===== 场景 2：全 Async 链洋葱执行顺序 ===== */

TEST_F(CompileTimeChainTest, AllAsync_ExecutionOrder)
{
	constexpr auto kAsyncMw1 = [](HttpRequest& r, MiddlewareNext n) -> Awaitable<HttpResponse>
	{
		co_return co_await n(r);
	};
	constexpr auto kAsyncMw2 = [](HttpRequest& r, MiddlewareNext n) -> Awaitable<HttpResponse>
	{
		co_return co_await n(r);
	};
	constexpr auto kAsyncMw3 = [](HttpRequest& r, MiddlewareNext n) -> Awaitable<HttpResponse>
	{
		co_return co_await n(r);
	};

	/* 编译期链 */
	auto compileTimeChain = buildCompileTimeChain<CompileTimeAsyncMw<kAsyncMw1>,
												  CompileTimeAsyncMw<kAsyncMw2>,
												  CompileTimeAsyncMw<kAsyncMw3>>(
		[](HttpRequest&) -> Awaitable<HttpResponse>
		{
			co_return HttpResponse::ok("compile");
		});

	/* 运行期链 */
	std::vector<MiddlewareEntry> entries;
	entries.push_back({MiddlewareEntry::Type::hAsync, "a1", kAsyncMw1, nullptr, nullptr});
	entries.push_back({MiddlewareEntry::Type::hAsync, "a2", kAsyncMw2, nullptr, nullptr});
	entries.push_back({MiddlewareEntry::Type::hAsync, "a3", kAsyncMw3, nullptr, nullptr});
	auto runtimeChain = MiddlewarePipeline::buildOptimizedChain(entries,
																[](HttpRequest&) -> Awaitable<HttpResponse>
																{
																	co_return HttpResponse::ok("runtime");
																});

	HttpRequest req;
	req.setMethod(HttpMethod::hGet);
	req.setTarget("/test");

	auto ctResult = runCoroutine(
		[&]()
		{
			return compileTimeChain(req);
		});
	auto rtResult = runCoroutine(
		[&]()
		{
			return runtimeChain(req);
		});

	ASSERT_TRUE(ctResult.has_value());
	ASSERT_TRUE(rtResult.has_value());
	EXPECT_EQ(ctResult->body(), "compile");
	EXPECT_EQ(rtResult->body(), "runtime");
}

/* ===== 场景 3：混合链（Sync + Async + Sync）洋葱执行顺序 ===== */

TEST_F(CompileTimeChainTest, Mixed_ExecutionOrder)
{
	constexpr auto kSyncBefore1 = [](HttpRequest&) -> SyncMiddlewareResult
	{
		return std::nullopt;
	};
	constexpr auto kAsyncMw = [](HttpRequest& r, MiddlewareNext n) -> Awaitable<HttpResponse>
	{
		co_return co_await n(r);
	};
	constexpr auto kSyncBefore2 = [](HttpRequest&) -> SyncMiddlewareResult
	{
		return std::nullopt;
	};

	/* 编译期链 */
	auto compileTimeChain = buildCompileTimeChain<CompileTimeSyncMw<kSyncBefore1>,
												  CompileTimeAsyncMw<kAsyncMw>,
												  CompileTimeSyncMw<kSyncBefore2>>(
		[](HttpRequest&) -> Awaitable<HttpResponse>
		{
			co_return HttpResponse::ok("compile");
		});

	/* 运行期链 */
	std::vector<MiddlewareEntry> entries;
	entries.push_back({MiddlewareEntry::Type::hSync, "s1", nullptr, kSyncBefore1, nullptr});
	entries.push_back({MiddlewareEntry::Type::hAsync, "a1", kAsyncMw, nullptr, nullptr});
	entries.push_back({MiddlewareEntry::Type::hSync, "s2", nullptr, kSyncBefore2, nullptr});
	auto runtimeChain = MiddlewarePipeline::buildOptimizedChain(entries,
																[](HttpRequest&) -> Awaitable<HttpResponse>
																{
																	co_return HttpResponse::ok("runtime");
																});

	HttpRequest req;
	req.setMethod(HttpMethod::hGet);
	req.setTarget("/test");

	auto ctResult = runCoroutine(
		[&]()
		{
			return compileTimeChain(req);
		});
	auto rtResult = runCoroutine(
		[&]()
		{
			return runtimeChain(req);
		});

	ASSERT_TRUE(ctResult.has_value());
	ASSERT_TRUE(rtResult.has_value());
	EXPECT_EQ(ctResult->body(), "compile");
	EXPECT_EQ(rtResult->body(), "runtime");
}

/* ===== 场景 4：短路返回（Sync before 中途拦截，后续中间件和 handler 不被调用） ===== */

TEST_F(CompileTimeChainTest, ShortCircuit_InterceptStopsChain)
{
	bool handlerCalled = false;

	constexpr auto kPassThrough = [](HttpRequest&) -> SyncMiddlewareResult
	{
		return std::nullopt;
	};
	constexpr auto kIntercept = [](HttpRequest&) -> SyncMiddlewareResult
	{
		HttpResponse res;
		res.setStatus(HttpStatusCode::hForbidden);
		res.setBody("blocked", "text/plain");
		return res;
	};
	constexpr auto kShouldNotRun = [](HttpRequest&) -> SyncMiddlewareResult
	{
		return std::nullopt;
	};

	/* 编译期链：pass -> intercept -> shouldNotRun -> handler
       中间 intercept 直接返回，shouldNotRun 和 handler 不应被调用 */
	auto compileTimeChain = buildCompileTimeChain<CompileTimeSyncMw<kPassThrough>,
												  CompileTimeSyncMw<kIntercept>,
												  CompileTimeSyncMw<kShouldNotRun>>(
		[&handlerCalled](HttpRequest&) -> Awaitable<HttpResponse>
		{
			handlerCalled = true;
			co_return HttpResponse::ok("should not reach");
		});

	HttpRequest req;
	req.setMethod(HttpMethod::hGet);
	req.setTarget("/admin");

	auto ctResult = runCoroutine(
		[&]()
		{
			return compileTimeChain(req);
		});

	ASSERT_TRUE(ctResult.has_value());
	EXPECT_EQ(ctResult->statusCode(), HttpStatusCode::hForbidden);
	EXPECT_EQ(ctResult->body(), "blocked");
	EXPECT_FALSE(handlerCalled);

	/* 运行期链：同样布局 */
	handlerCalled = false;
	std::vector<MiddlewareEntry> entries;
	entries.push_back({MiddlewareEntry::Type::hSync, "pass", nullptr, kPassThrough, nullptr});
	entries.push_back({MiddlewareEntry::Type::hSync, "intercept", nullptr, kIntercept, nullptr});
	entries.push_back({MiddlewareEntry::Type::hSync, "skip", nullptr, kShouldNotRun, nullptr});
	auto runtimeChain =
		MiddlewarePipeline::buildOptimizedChain(entries,
												[&handlerCalled](HttpRequest&) -> Awaitable<HttpResponse>
												{
													handlerCalled = true;
													co_return HttpResponse::ok("should not reach");
												});

	auto rtResult = runCoroutine(
		[&]()
		{
			return runtimeChain(req);
		});

	ASSERT_TRUE(rtResult.has_value());
	EXPECT_EQ(rtResult->statusCode(), HttpStatusCode::hForbidden);
	EXPECT_EQ(rtResult->body(), "blocked");
	EXPECT_FALSE(handlerCalled);
}

/* ===== 场景 5：SyncFullMw（before + after）洋葱逆序执行 after ===== */

TEST_F(CompileTimeChainTest, SyncFullMw_BeforeAndAfter)
{
	constexpr auto kBefore1 = [](HttpRequest&) -> SyncMiddlewareResult
	{
		return std::nullopt;
	};
	constexpr auto kAfter1 = [](HttpRequest&, HttpResponse&)
	{
	};
	constexpr auto kBefore2 = [](HttpRequest&) -> SyncMiddlewareResult
	{
		return std::nullopt;
	};
	constexpr auto kAfter2 = [](HttpRequest&, HttpResponse&)
	{
	};

	/* 编译期链：两个 SyncFullMw，按洋葱语义 after 应该逆序执行 */
	auto compileTimeChain =
		buildCompileTimeChain<CompileTimeSyncFullMw<kBefore1, kAfter1>, CompileTimeSyncFullMw<kBefore2, kAfter2>>(
			[](HttpRequest&) -> Awaitable<HttpResponse>
			{
				co_return HttpResponse::ok("compile");
			});

	/* 运行期链 */
	std::vector<MiddlewareEntry> entries;
	entries.push_back({MiddlewareEntry::Type::hSync, "sf1", nullptr, kBefore1, kAfter1});
	entries.push_back({MiddlewareEntry::Type::hSync, "sf2", nullptr, kBefore2, kAfter2});
	auto runtimeChain = MiddlewarePipeline::buildOptimizedChain(entries,
																[](HttpRequest&) -> Awaitable<HttpResponse>
																{
																	co_return HttpResponse::ok("runtime");
																});

	HttpRequest req;
	req.setMethod(HttpMethod::hGet);
	req.setTarget("/test");

	auto ctResult = runCoroutine(
		[&]()
		{
			return compileTimeChain(req);
		});
	auto rtResult = runCoroutine(
		[&]()
		{
			return runtimeChain(req);
		});

	ASSERT_TRUE(ctResult.has_value());
	ASSERT_TRUE(rtResult.has_value());
	EXPECT_EQ(ctResult->body(), "compile");
	EXPECT_EQ(rtResult->body(), "runtime");
}
