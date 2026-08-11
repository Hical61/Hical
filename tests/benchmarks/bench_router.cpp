/**
 * @file bench_router.cpp
 * @brief 路由分发微基准（dispatchSync / dispatch / middleware 链）
 */

#include "core/Router.h"
#include "core/HttpRequest.h"
#include "core/HttpResponse.h"
#include "core/Coroutine.h"
#include "core/Middleware.h"
#include <benchmark/benchmark.h>
#include <boost/asio.hpp>

using namespace hical;

/* ========== Router benchmark 辅助函数 ========== */

/** 创建含 count 个静态路由的 Router（同步 handler） */
static Router createStaticRouter(int count)
{
	Router router;
	for (int i = 0; i < count; ++i)
	{
		router.get("/api/v1/route" + std::to_string(i),
				   [](const HttpRequest&) -> HttpResponse
				   {
					   return HttpResponse::ok("ok");
				   });
	}
	return router;
}

/* ========== 静态路由 dispatch（协程路径）========== */

/**
 * @brief 100 条静态路由，匹配首条
 * 走 co_await dispatch 完整路径（含协程帧分配 + io_context 调度）
 */
static void BM_StaticRouteFirstHit(benchmark::State& state)
{
	auto router = createStaticRouter(100);
	HttpRequest req;
	req.setMethod(HttpMethod::hGet);
	req.setTarget("/api/v1/route0");
	boost::asio::io_context io;

	for (auto _ : state)
	{
		io.restart();
		boost::asio::co_spawn(
			io,
			[&]() -> Awaitable<void>
			{
				co_await router.dispatch(req);
			},
			boost::asio::detached);
		io.run();
	}
}

BENCHMARK(BM_StaticRouteFirstHit);

/**
 * @brief 100 条静态路由，匹配末条
 */
static void BM_StaticRouteLastHit(benchmark::State& state)
{
	auto router = createStaticRouter(100);
	HttpRequest req;
	req.setMethod(HttpMethod::hGet);
	req.setTarget("/api/v1/route99");
	boost::asio::io_context io;

	for (auto _ : state)
	{
		io.restart();
		boost::asio::co_spawn(
			io,
			[&]() -> Awaitable<void>
			{
				co_await router.dispatch(req);
			},
			boost::asio::detached);
		io.run();
	}
}

BENCHMARK(BM_StaticRouteLastHit);

/**
 * @brief 100 条静态路由，全不命中
 */
static void BM_StaticRouteMiss(benchmark::State& state)
{
	auto router = createStaticRouter(100);
	HttpRequest req;
	req.setMethod(HttpMethod::hGet);
	req.setTarget("/api/v1/nonexistent");
	boost::asio::io_context io;

	for (auto _ : state)
	{
		io.restart();
		boost::asio::co_spawn(
			io,
			[&]() -> Awaitable<void>
			{
				co_await router.dispatch(req);
			},
			boost::asio::detached);
		io.run();
	}
}

BENCHMARK(BM_StaticRouteMiss);

/* ========== 参数路由 ========== */

/**
 * @brief 参数路由匹配（如 /users/{id} → /users/42）
 */
static void BM_ParamRouteMatch(benchmark::State& state)
{
	Router router;
	for (int i = 0; i < 20; ++i)
	{
		router.get("/api/v1/resource" + std::to_string(i) + "/{id}",
				   [](const HttpRequest&) -> HttpResponse
				   {
					   return HttpResponse::ok("ok");
				   });
	}
	HttpRequest req;
	req.setMethod(HttpMethod::hGet);
	req.setTarget("/api/v1/resource10/12345");
	boost::asio::io_context io;

	for (auto _ : state)
	{
		io.restart();
		boost::asio::co_spawn(
			io,
			[&]() -> Awaitable<void>
			{
				co_await router.dispatch(req);
			},
			boost::asio::detached);
		io.run();
	}
}

BENCHMARK(BM_ParamRouteMatch);

/* ========== 大量路由 ========== */

/**
 * @brief 1000 条静态路由，匹配中间位置
 */
static void BM_LargeRouteSet(benchmark::State& state)
{
	auto router = createStaticRouter(1000);
	HttpRequest req;
	req.setMethod(HttpMethod::hGet);
	req.setTarget("/api/v1/route500");
	boost::asio::io_context io;

	for (auto _ : state)
	{
		io.restart();
		boost::asio::co_spawn(
			io,
			[&]() -> Awaitable<void>
			{
				co_await router.dispatch(req);
			},
			boost::asio::detached);
		io.run();
	}
}

BENCHMARK(BM_LargeRouteSet);

/* ========== dispatchSync 快速路径 ========== */

/**
 * @brief dispatchSync 同步快速路径（sync handler，零协程帧分配）
 */
static void BM_DispatchSync_SyncHandler(benchmark::State& state)
{
	Router router;
	router.get("/bench",
			   [](const HttpRequest&) -> HttpResponse
			   {
				   return HttpResponse::ok("ok");
			   });
	HttpRequest req;
	req.setMethod(HttpMethod::hGet);
	req.setTarget("/bench");

	for (auto _ : state)
	{
		auto result = router.dispatchSync(req);
		benchmark::DoNotOptimize(result);
	}
}

BENCHMARK(BM_DispatchSync_SyncHandler);

/**
 * @brief dispatchSync fallback 路径（async handler → 返回 nullopt）
 */
static void BM_DispatchSync_FallbackAsync(benchmark::State& state)
{
	Router router;
	router.get("/bench",
			   [](const HttpRequest&) -> Awaitable<HttpResponse>
			   {
				   co_return HttpResponse::ok("ok");
			   });
	HttpRequest req;
	req.setMethod(HttpMethod::hGet);
	req.setTarget("/bench");

	for (auto _ : state)
	{
		auto result = router.dispatchSync(req);
		benchmark::DoNotOptimize(result);
	}
}

BENCHMARK(BM_DispatchSync_FallbackAsync);

/* ========== 完整 resolve + dispatch 协程链路 ========== */

/**
 * @brief 完整 resolveRoute + dispatch 链路（协程路径）
 * 测量从 co_await router.dispatch() 到响应返回的完整路径
 */
static void BM_RouteResolveAndDispatch(benchmark::State& state)
{
	auto router = createStaticRouter(100);
	HttpRequest req;
	req.setMethod(HttpMethod::hGet);
	req.setTarget("/api/v1/route50");
	boost::asio::io_context io;

	for (auto _ : state)
	{
		io.restart();
		boost::asio::co_spawn(
			io,
			[&]() -> Awaitable<void>
			{
				co_await router.dispatch(req);
			},
			boost::asio::detached);
		io.run();
	}
}

BENCHMARK(BM_RouteResolveAndDispatch);

/* ========== 中间件链执行 ========== */

/**
 * @brief 中间件链执行（3 个空中间件 + 简单 handler）
 * 测量洋葱链构建和执行的开销
 */
static void BM_PipelineExecute(benchmark::State& state)
{
	MiddlewarePipeline pipeline;

	/* 添加 3 个空中间件，模拟常见链长 */
	pipeline.use(
		[](HttpRequest& req, MiddlewareNext next) -> Awaitable<HttpResponse>
		{
			co_return co_await next(req);
		});
	pipeline.use(
		[](HttpRequest& req, MiddlewareNext next) -> Awaitable<HttpResponse>
		{
			co_return co_await next(req);
		});
	pipeline.use(
		[](HttpRequest& req, MiddlewareNext next) -> Awaitable<HttpResponse>
		{
			co_return co_await next(req);
		});

	auto finalHandler = [](HttpRequest&) -> Awaitable<HttpResponse>
	{
		co_return HttpResponse::ok("ok");
	};
	pipeline.build(finalHandler);

	HttpRequest req;
	req.setMethod(HttpMethod::hGet);
	req.setTarget("/test");
	boost::asio::io_context io;

	for (auto _ : state)
	{
		io.restart();
		boost::asio::co_spawn(
			io,
			[&]() -> Awaitable<void>
			{
				co_await pipeline.execute(req);
			},
			boost::asio::detached);
		io.run();
	}
}

BENCHMARK(BM_PipelineExecute);
