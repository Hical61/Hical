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

#endif // HICAL_ENABLE_MIDDLEWARE_PROFILING
