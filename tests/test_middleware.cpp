#include "core/Middleware.h"
#include "core/Coroutine.h"
#include "asio/AsioEventLoop.h"
#include <gtest/gtest.h>
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
			res.setBody("Forbidden");
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
