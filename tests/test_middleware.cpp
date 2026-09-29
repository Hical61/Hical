/**
 * @file test_middleware.cpp
 * @brief 中间件管线测试（洋葱模型、Sync 快速路径、profiling 统计）
 */

#include "core/Middleware.h"
#include "core/Coroutine.h"
#include "asio/AsioEventLoop.h"
#include <gtest/gtest.h>
#include <stdexcept>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

using namespace hical;

// 辅助：在事件循环中运行协程
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

TEST(MiddlewareTest, EmptyPipeline)
{
	MiddlewarePipeline pipeline;
	EXPECT_EQ(pipeline.size(), 0);

	HttpRequest req;
	req.setMethod(HttpMethod::hGet);
	req.setTarget("/test");

	auto result = runCoroutine(
		[&]()
		{
			return pipeline.execute(req,
									[](HttpRequest&) -> Awaitable<HttpResponse>
									{
										co_return HttpResponse::ok("final");
									});
		});

	ASSERT_TRUE(result.has_value());
	EXPECT_EQ(result->body(), "final");
}

TEST(MiddlewareTest, SingleMiddleware)
{
	MiddlewarePipeline pipeline;

	pipeline.use(
		[](HttpRequest& req, MiddlewareNext next) -> Awaitable<HttpResponse>
		{
			auto res = co_await next(req);
			res.setHeader("X-Middleware", "applied");
			co_return res;
		});

	EXPECT_EQ(pipeline.size(), 1);

	HttpRequest req;
	req.setMethod(HttpMethod::hGet);
	req.setTarget("/test");

	auto result = runCoroutine(
		[&]()
		{
			return pipeline.execute(req,
									[](HttpRequest&) -> Awaitable<HttpResponse>
									{
										co_return HttpResponse::ok("body");
									});
		});

	ASSERT_TRUE(result.has_value());
	EXPECT_EQ(result->body(), "body");
	EXPECT_EQ(result->header("X-Middleware"), "applied");
}

TEST(MiddlewareTest, ExecutionOrder)
{
	MiddlewarePipeline pipeline;
	std::vector<int> order;

	pipeline.use(
		[&order](HttpRequest& req, MiddlewareNext next) -> Awaitable<HttpResponse>
		{
			order.push_back(1); // 前置
			auto res = co_await next(req);
			order.push_back(4); // 后置
			co_return res;
		});

	pipeline.use(
		[&order](HttpRequest& req, MiddlewareNext next) -> Awaitable<HttpResponse>
		{
			order.push_back(2); // 前置
			auto res = co_await next(req);
			order.push_back(3); // 后置
			co_return res;
		});

	HttpRequest req;
	req.setMethod(HttpMethod::hGet);
	req.setTarget("/test");

	runCoroutine(
		[&]()
		{
			return pipeline.execute(req,
									[](HttpRequest&) -> Awaitable<HttpResponse>
									{
										co_return HttpResponse::ok("done");
									});
		});

	// 洋葱模型：1 -> 2 -> handler -> 3 -> 4
	ASSERT_EQ(order.size(), 4);
	EXPECT_EQ(order[0], 1);
	EXPECT_EQ(order[1], 2);
	EXPECT_EQ(order[2], 3);
	EXPECT_EQ(order[3], 4);
}

TEST(MiddlewareTest, Intercept)
{
	MiddlewarePipeline pipeline;
	std::atomic<bool> handlerCalled {false};

	// 拦截中间件：直接返回 403，不调用 next
	pipeline.use(
		[](HttpRequest&, MiddlewareNext) -> Awaitable<HttpResponse>
		{
			HttpResponse res;
			res.setStatus(HttpStatusCode::hForbidden);
			res.setBody("Forbidden", "text/plain");
			co_return res;
		});

	HttpRequest req;
	req.setMethod(HttpMethod::hGet);
	req.setTarget("/admin");

	auto result = runCoroutine(
		[&]()
		{
			return pipeline.execute(req,
									[&handlerCalled](HttpRequest&) -> Awaitable<HttpResponse>
									{
										handlerCalled = true;
										co_return HttpResponse::ok("should not reach");
									});
		});

	ASSERT_TRUE(result.has_value());
	EXPECT_EQ(result->statusCode(), HttpStatusCode::hForbidden);
	EXPECT_FALSE(handlerCalled.load());
}

TEST(MiddlewareTest, ModifyRequest)
{
	MiddlewarePipeline pipeline;

	// 中间件在前置逻辑中给请求添加头
	pipeline.use(
		[](HttpRequest& req, MiddlewareNext next) -> Awaitable<HttpResponse>
		{
			// 注意：中间件目前不能修改 const 请求
			// 但可以在后置逻辑中修改响应
			auto res = co_await next(req);
			res.setHeader("X-Processed", "true");
			co_return res;
		});

	HttpRequest req;
	req.setMethod(HttpMethod::hGet);
	req.setTarget("/test");

	auto result = runCoroutine(
		[&]()
		{
			return pipeline.execute(req,
									[](HttpRequest&) -> Awaitable<HttpResponse>
									{
										co_return HttpResponse::ok("ok");
									});
		});

	ASSERT_TRUE(result.has_value());
	EXPECT_EQ(result->header("X-Processed"), "true");
}

// 测试 build() 后调用 use() 抛异常
TEST(MiddlewareTest, UseAfterBuildThrows)
{
	MiddlewarePipeline pipeline;

	pipeline.use(
		[](HttpRequest& req, MiddlewareNext next) -> Awaitable<HttpResponse>
		{
			co_return co_await next(req);
		});

	pipeline.build(
		[](HttpRequest&) -> Awaitable<HttpResponse>
		{
			co_return HttpResponse::ok("final");
		});

	EXPECT_THROW(pipeline.use(
					 [](HttpRequest& req, MiddlewareNext next) -> Awaitable<HttpResponse>
					 {
						 co_return co_await next(req);
					 }),
				 std::logic_error);
}

// 测试空管道 build() 后也不能 use()
TEST(MiddlewareTest, UseAfterBuildEmptyPipelineThrows)
{
	MiddlewarePipeline pipeline;

	pipeline.build(
		[](HttpRequest&) -> Awaitable<HttpResponse>
		{
			co_return HttpResponse::ok("final");
		});

	EXPECT_THROW(pipeline.use(
					 [](HttpRequest& req, MiddlewareNext next) -> Awaitable<HttpResponse>
					 {
						 co_return co_await next(req);
					 }),
				 std::logic_error);
}

// 测试 build() 前 use() 正常
TEST(MiddlewareTest, UseBeforeBuildSucceeds)
{
	MiddlewarePipeline pipeline;

	EXPECT_NO_THROW(pipeline.use(
		[](HttpRequest& req, MiddlewareNext next) -> Awaitable<HttpResponse>
		{
			co_return co_await next(req);
		}));

	EXPECT_EQ(pipeline.size(), 1);
}

// ============ 命名中间件 ============

TEST(MiddlewareTest, NamedUseWorks)
{
	MiddlewarePipeline pipeline;

	pipeline.use("logger",
				 [](HttpRequest& req, MiddlewareNext next) -> Awaitable<HttpResponse>
				 {
					 co_return co_await next(req);
				 });

	pipeline.use("auth",
				 [](HttpRequest& req, MiddlewareNext next) -> Awaitable<HttpResponse>
				 {
					 co_return co_await next(req);
				 });

	EXPECT_EQ(pipeline.size(), 2);
}

// ============ execute(req, finalHandler) 重载 ============

// 这个重载以前只从 entries_ 里挑 hAsync 搭链，Sync 中间件在任何编译模式下都被吞掉。
// 用它跑一遍带拦截的 Sync 中间件，钉死「Sync 也得进链」。
TEST(MiddlewareTest, ExecuteWithFinalHandlerRunsSyncMiddleware)
{
	MiddlewarePipeline pipeline;
	int syncCalls = 0;

	pipeline.use(
		[&syncCalls](HttpRequest& req) -> SyncMiddlewareResult
		{
			++syncCalls;
			if (req.path() == "/blocked")
			{
				HttpResponse res;
				res.setStatus(HttpStatusCode::hUnauthorized);
				res.setBody("nope", "text/plain");
				return res;
			}
			return std::nullopt;
		});

	pipeline.use("passthrough",
				 [](HttpRequest& req, MiddlewareNext next) -> Awaitable<HttpResponse>
				 {
					 co_return co_await next(req);
				 });

	// 被 Sync 中间件拦下
	// 注意 req 必须建在用例作用域里：execute 是协程，协程体要等 co_await 才跑，
	// 塞进 runCoroutine 的 lambda 里的话 f() 一返回 req 就析构了，链上读到的就是悬垂对象。
	HttpRequest blockedReq;
	blockedReq.setMethod(HttpMethod::hGet);
	blockedReq.setTarget("/blocked");

	auto blocked = runCoroutine(
		[&]()
		{
			return pipeline.execute(blockedReq,
									[](HttpRequest&) -> Awaitable<HttpResponse>
									{
										co_return HttpResponse::ok("handler");
									});
		});
	ASSERT_TRUE(blocked.has_value());
	EXPECT_EQ(blocked->statusCode(), HttpStatusCode::hUnauthorized);
	EXPECT_EQ(syncCalls, 1);

	// 放行时 Sync 中间件照样执行，最终处理器正常返回
	HttpRequest openReq;
	openReq.setMethod(HttpMethod::hGet);
	openReq.setTarget("/open");

	auto passed = runCoroutine(
		[&]()
		{
			return pipeline.execute(openReq,
									[](HttpRequest&) -> Awaitable<HttpResponse>
									{
										co_return HttpResponse::ok("handler");
									});
		});
	ASSERT_TRUE(passed.has_value());
	EXPECT_EQ(passed->body(), "handler");
	EXPECT_EQ(syncCalls, 2);
}

// ============ 裸 SyncAfterHandler 重载（use(SyncAfterHandler)） ============

// after-only 重载最核心的语义：after 必须在最终处理器「之后」跑。
// 只看响应内容区分不出先后（顺序反了结果也一样），所以这里记录执行序列来断言。
TEST(MiddlewareTest, AfterOnlyHandler_RunsAfterFinalHandler_OrderRecorded)
{
	MiddlewarePipeline pipeline;
	std::vector<std::string> order;

	pipeline.use(
		[&order](HttpRequest&, HttpResponse& res) -> void
		{
			order.push_back("after");
			res.setHeader("X-After-Only", "hit");
		});

	pipeline.build(
		[&order](HttpRequest&) -> Awaitable<HttpResponse>
		{
			order.push_back("handler");
			co_return HttpResponse::ok("body");
		});

	HttpRequest req;
	req.setMethod(HttpMethod::hGet);
	req.setTarget("/test");

	auto result = runCoroutine(
		[&]()
		{
			return pipeline.execute(req);
		});

	ASSERT_TRUE(result.has_value());

	// 先把执行顺序钉死再看响应内容：顺序反了内容未必错，这条断言才是关键
	ASSERT_EQ(order.size(), 2u);
	EXPECT_EQ(order[0], "handler"); // 先 handler
	EXPECT_EQ(order[1], "after");   // 后 after

	EXPECT_EQ(result->body(), "body");
	EXPECT_EQ(result->header("X-After-Only"), "hit"); // after 确实跑了
}

// 多个 after-only 条目之间：后注册的在洋葱里更靠内，它的 after 先退出。
// 即执行顺序 = 注册顺序的逆序。
TEST(MiddlewareTest, MultipleAfterOnlyHandlers_RunInReverseRegistrationOrder)
{
	MiddlewarePipeline pipeline;
	std::vector<std::string> order;

	pipeline.use(
		[&order](HttpRequest&, HttpResponse&) -> void
		{
			order.push_back("first");
		});
	pipeline.use(
		[&order](HttpRequest&, HttpResponse&) -> void
		{
			order.push_back("second");
		});

	pipeline.build(
		[&order](HttpRequest&) -> Awaitable<HttpResponse>
		{
			order.push_back("handler");
			co_return HttpResponse::ok("body");
		});

	HttpRequest req;
	req.setMethod(HttpMethod::hGet);
	req.setTarget("/test");

	auto result = runCoroutine(
		[&]()
		{
			return pipeline.execute(req);
		});

	ASSERT_TRUE(result.has_value());
	ASSERT_EQ(order.size(), 3u);
	EXPECT_EQ(order[0], "handler");
	EXPECT_EQ(order[1], "second"); // 后注册的先跑
	EXPECT_EQ(order[2], "first");
}

// 命名重载：命名 after-only 同样只占一个条目，且照常执行
TEST(MiddlewareTest, NamedAfterOnlyHandler_RegisteredAndRuns)
{
	MiddlewarePipeline pipeline;
	int calls = 0;

	pipeline.use("named-after",
				 [&calls](HttpRequest&, HttpResponse& res) -> void
				 {
					 ++calls;
					 res.setHeader("X-Named-After", "hit");
				 });

	EXPECT_EQ(pipeline.size(), 1);

	pipeline.build(
		[](HttpRequest&) -> Awaitable<HttpResponse>
		{
			co_return HttpResponse::ok("body");
		});

	HttpRequest req;
	req.setMethod(HttpMethod::hGet);
	req.setTarget("/test");

	auto result = runCoroutine(
		[&]()
		{
			return pipeline.execute(req);
		});

	ASSERT_TRUE(result.has_value());
	EXPECT_EQ(calls, 1);
	EXPECT_EQ(result->header("X-Named-After"), "hit");
}

// after-only 重载也得守住「build() 之后不许再加中间件」这条线
TEST(MiddlewareTest, UseAfterOnlyAfterBuildThrows)
{
	MiddlewarePipeline pipeline;

	pipeline.build(
		[](HttpRequest&) -> Awaitable<HttpResponse>
		{
			co_return HttpResponse::ok("final");
		});

	EXPECT_THROW(pipeline.use(
					 [](HttpRequest&, HttpResponse&) -> void
					 {
					 }),
				 std::logic_error);
}

// 边界：传了个空的 SyncAfterHandler，链条构建和执行都不能崩
TEST(MiddlewareTest, EmptyAfterOnlyHandler_BuiltAndExecutedWithoutCrash)
{
	MiddlewarePipeline pipeline;
	pipeline.use(SyncAfterHandler {}); // 空 std::function，等价于没注册 after

	EXPECT_EQ(pipeline.size(), 1);

	pipeline.build(
		[](HttpRequest&) -> Awaitable<HttpResponse>
		{
			co_return HttpResponse::ok("still-ok");
		});

	HttpRequest req;
	req.setMethod(HttpMethod::hGet);
	req.setTarget("/test");

	auto result = runCoroutine(
		[&]()
		{
			return pipeline.execute(req);
		});

	ASSERT_TRUE(result.has_value());
	EXPECT_EQ(result->body(), "still-ok");
}

// ============ Profiling 测试（仅在编译选项开启时生效） ============

#ifdef HICAL_ENABLE_MIDDLEWARE_PROFILING

TEST(MiddlewareProfilingTest, TimingStatsRecordCallCount)
{
	MiddlewarePipeline pipeline;

	pipeline.use("fast",
				 [](HttpRequest& req, MiddlewareNext next) -> Awaitable<HttpResponse>
				 {
					 co_return co_await next(req);
				 });

	pipeline.use("slow",
				 [](HttpRequest& req, MiddlewareNext next) -> Awaitable<HttpResponse>
				 {
					 auto res = co_await next(req);
					 co_return res;
				 });

	pipeline.build(
		[](HttpRequest&) -> Awaitable<HttpResponse>
		{
			co_return HttpResponse::ok("ok");
		});

	// 执行 3 次
	for (int i = 0; i < 3; ++i)
	{
		auto result = runCoroutine(
			[&]()
			{
				HttpRequest req;
				return pipeline.execute(req);
			});
		ASSERT_TRUE(result.has_value());
	}

	auto stats = pipeline.getTimingStats();
	ASSERT_EQ(stats.size(), 2);
	EXPECT_EQ(stats[0].name, "fast");
	EXPECT_EQ(stats[1].name, "slow");
	EXPECT_EQ(stats[0].callCount, 3);
	EXPECT_EQ(stats[1].callCount, 3);
	EXPECT_GE(stats[0].avgTimeMs, 0.0);
	EXPECT_GE(stats[1].avgTimeMs, 0.0);
}

TEST(MiddlewareProfilingTest, ResetTimingStats)
{
	MiddlewarePipeline pipeline;
	pipeline.use("mw",
				 [](HttpRequest& req, MiddlewareNext next) -> Awaitable<HttpResponse>
				 {
					 co_return co_await next(req);
				 });
	pipeline.build(
		[](HttpRequest&) -> Awaitable<HttpResponse>
		{
			co_return HttpResponse::ok("ok");
		});

	auto result = runCoroutine(
		[&]()
		{
			HttpRequest req;
			return pipeline.execute(req);
		});
	ASSERT_TRUE(result.has_value());

	pipeline.resetTimingStats();
	auto stats = pipeline.getTimingStats();
	ASSERT_EQ(stats.size(), 1);
	EXPECT_EQ(stats[0].callCount, 0);
}

// profiling 链是按 vector<MiddlewareHandler> 搭的，Sync 条目没有 handler 可挂，
// 硬上就是「不执行」而不是「不计时」。这条用例盯着「宁可少统计也不能少执行」。
TEST(MiddlewareProfilingTest, SyncMiddlewareStillRunsInsteadOfBeingDropped)
{
	MiddlewarePipeline pipeline;
	int syncCalls = 0;

	pipeline.use("auth-like",
				 [&syncCalls](HttpRequest&) -> SyncMiddlewareResult
				 {
					 ++syncCalls;
					 HttpResponse res;
					 res.setStatus(HttpStatusCode::hUnauthorized);
					 res.setBody("nope", "text/plain");
					 return res;
				 });

	pipeline.build(
		[](HttpRequest&) -> Awaitable<HttpResponse>
		{
			co_return HttpResponse::ok("handler");
		});

	HttpRequest req;
	req.setMethod(HttpMethod::hGet);
	req.setTarget("/private");

	// req 得活到 co_await 结束（协程体是延迟跑的），所以建在用例作用域里
	auto result = runCoroutine(
		[&]()
		{
			return pipeline.execute(req);
		});

	ASSERT_TRUE(result.has_value());
	EXPECT_EQ(syncCalls, 1);                                        // 真执行了
	EXPECT_EQ(result->statusCode(), HttpStatusCode::hUnauthorized); // 拦截也生效了

	// 回退的管线不做 profiling，统计里不该留 callCount 恒为 0 的假行
	EXPECT_TRUE(pipeline.getTimingStats().empty());
}

// Sync + Async 混编：Async 照常计时路径上的行为不变，Sync 也不能丢
TEST(MiddlewareProfilingTest, MixedSyncAndAsyncFallsBackWithoutFakeStats)
{
	MiddlewarePipeline pipeline;
	int syncCalls = 0;
	int asyncCalls = 0;

	pipeline.use("sync",
				 [&syncCalls](HttpRequest&) -> SyncMiddlewareResult
				 {
					 ++syncCalls;
					 return std::nullopt;
				 });

	pipeline.use("async",
				 [&asyncCalls](HttpRequest& req, MiddlewareNext next) -> Awaitable<HttpResponse>
				 {
					 ++asyncCalls;
					 co_return co_await next(req);
				 });

	pipeline.build(
		[](HttpRequest&) -> Awaitable<HttpResponse>
		{
			co_return HttpResponse::ok("ok");
		});

	HttpRequest req;
	auto result = runCoroutine(
		[&]()
		{
			return pipeline.execute(req);
		});

	ASSERT_TRUE(result.has_value());
	EXPECT_EQ(result->body(), "ok");
	EXPECT_EQ(syncCalls, 1);
	EXPECT_EQ(asyncCalls, 1);
	EXPECT_TRUE(pipeline.getTimingStats().empty());
}

// HttpServer 就是这么用的：build() 之后再用 buildFor() 预构建 WS/SSE 那条链，
// 这条链同样不能丢 Sync 条目。
TEST(MiddlewareProfilingTest, BuildForKeepsSyncMiddleware)
{
	MiddlewarePipeline pipeline;
	int syncCalls = 0;

	pipeline.use("sync",
				 [&syncCalls](HttpRequest&) -> SyncMiddlewareResult
				 {
					 ++syncCalls;
					 return std::nullopt;
				 });

	pipeline.build(
		[](HttpRequest&) -> Awaitable<HttpResponse>
		{
			co_return HttpResponse::ok("main");
		});

	auto wsChain = pipeline.buildFor(
		[](HttpRequest&) -> Awaitable<HttpResponse>
		{
			co_return HttpResponse::ok("");
		});

	HttpRequest mainReq;
	auto mainRes = runCoroutine(
		[&]()
		{
			return pipeline.execute(mainReq);
		});
	ASSERT_TRUE(mainRes.has_value());
	EXPECT_EQ(mainRes->body(), "main");
	EXPECT_EQ(syncCalls, 1);

	HttpRequest wsReq;
	auto wsRes = runCoroutine(
		[&]()
		{
			return wsChain(wsReq);
		});
	ASSERT_TRUE(wsRes.has_value());
	EXPECT_EQ(syncCalls, 2);
}

#endif // HICAL_ENABLE_MIDDLEWARE_PROFILING
