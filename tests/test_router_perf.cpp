#include "core/Router.h"
#include <gtest/gtest.h>
#include <chrono>
#include <iostream>
#include <string>

using namespace hical;

namespace
{

	/**
	 * @brief 创建指定数量的静态路由
	 */
	Router createRouterWithStaticRoutes(int count)
	{
		Router router;
		for (int i = 0; i < count; ++i)
		{
			std::string path = "/api/v1/route" + std::to_string(i);
			router.get(path,
					   [](const HttpRequest&) -> HttpResponse
					   {
						   return HttpResponse::ok("ok");
					   });
		}
		return router;
	}

	/**
	 * @brief 创建指定数量的参数路由
	 */
	Router createRouterWithParamRoutes(int count)
	{
		Router router;
		for (int i = 0; i < count; ++i)
		{
			std::string path = "/api/v1/resource" + std::to_string(i) + "/{id}";
			router.get(path,
					   [](const HttpRequest&) -> HttpResponse
					   {
						   return HttpResponse::ok("ok");
					   });
		}
		return router;
	}

	class RouterPerfTest : public ::testing::Test
	{
	protected:
		static constexpr int hWarmupIterations = 1000;
		static constexpr int hBenchIterations = 100000;
	};

} // namespace

/**
 * @brief 静态路由查找性能：命中首条
 */
TEST_F(RouterPerfTest, StaticRouteFirstHit)
{
	auto router = createRouterWithStaticRoutes(100);

	HttpRequest req;
	req.setMethod(HttpMethod::hGet);
	req.setTarget("/api/v1/route0");

	// 预热
	boost::asio::io_context io;
	for (int i = 0; i < hWarmupIterations; ++i)
	{
		boost::asio::co_spawn(
			io,
			[&]() -> Awaitable<void>
			{
				co_await router.dispatch(req);
			},
			boost::asio::detached);
	}
	io.run();
	io.restart();

	// 计时
	auto start = std::chrono::high_resolution_clock::now();
	for (int i = 0; i < hBenchIterations; ++i)
	{
		boost::asio::co_spawn(
			io,
			[&]() -> Awaitable<void>
			{
				co_await router.dispatch(req);
			},
			boost::asio::detached);
	}
	io.run();
	auto end = std::chrono::high_resolution_clock::now();

	double totalMs = std::chrono::duration<double, std::milli>(end - start).count();
	double nsPerOp = (totalMs * 1000000.0) / hBenchIterations;

	std::cout << "[静态路由-首条命中] " << hBenchIterations << " 次分发, 总耗时: " << totalMs
			  << " ms, 每次: " << nsPerOp << " ns\n";

	// 性能断言：每次分发不应超过 10us（宽松）
	EXPECT_LT(nsPerOp, 10000.0);
}

/**
 * @brief 静态路由查找性能：命中末条
 */
TEST_F(RouterPerfTest, StaticRouteLastHit)
{
	auto router = createRouterWithStaticRoutes(100);

	HttpRequest req;
	req.setMethod(HttpMethod::hGet);
	req.setTarget("/api/v1/route99");

	boost::asio::io_context io;

	auto start = std::chrono::high_resolution_clock::now();
	for (int i = 0; i < hBenchIterations; ++i)
	{
		boost::asio::co_spawn(
			io,
			[&]() -> Awaitable<void>
			{
				co_await router.dispatch(req);
			},
			boost::asio::detached);
	}
	io.run();
	auto end = std::chrono::high_resolution_clock::now();

	double totalMs = std::chrono::duration<double, std::milli>(end - start).count();
	double nsPerOp = (totalMs * 1000000.0) / hBenchIterations;

	std::cout << "[静态路由-末条命中] " << hBenchIterations << " 次分发, 总耗时: " << totalMs
			  << " ms, 每次: " << nsPerOp << " ns\n";

	// 哈希表查找：首条和末条应该性能接近
	EXPECT_LT(nsPerOp, 10000.0);
}

/**
 * @brief 静态路由查找性能：未命中
 */
TEST_F(RouterPerfTest, StaticRouteMiss)
{
	auto router = createRouterWithStaticRoutes(100);

	HttpRequest req;
	req.setMethod(HttpMethod::hGet);
	req.setTarget("/api/v1/nonexistent");

	boost::asio::io_context io;

	auto start = std::chrono::high_resolution_clock::now();
	for (int i = 0; i < hBenchIterations; ++i)
	{
		boost::asio::co_spawn(
			io,
			[&]() -> Awaitable<void>
			{
				co_await router.dispatch(req);
			},
			boost::asio::detached);
	}
	io.run();
	auto end = std::chrono::high_resolution_clock::now();

	double totalMs = std::chrono::duration<double, std::milli>(end - start).count();
	double nsPerOp = (totalMs * 1000000.0) / hBenchIterations;

	std::cout << "[静态路由-未命中]   " << hBenchIterations << " 次分发, 总耗时: " << totalMs
			  << " ms, 每次: " << nsPerOp << " ns\n";

	EXPECT_LT(nsPerOp, 10000.0);
}

/**
 * @brief 参数路由查找性能
 */
TEST_F(RouterPerfTest, ParamRouteMatch)
{
	auto router = createRouterWithParamRoutes(20);

	HttpRequest req;
	req.setMethod(HttpMethod::hGet);
	req.setTarget("/api/v1/resource10/12345");

	boost::asio::io_context io;

	auto start = std::chrono::high_resolution_clock::now();
	for (int i = 0; i < hBenchIterations; ++i)
	{
		boost::asio::co_spawn(
			io,
			[&]() -> Awaitable<void>
			{
				co_await router.dispatch(req);
			},
			boost::asio::detached);
	}
	io.run();
	auto end = std::chrono::high_resolution_clock::now();

	double totalMs = std::chrono::duration<double, std::milli>(end - start).count();
	double nsPerOp = (totalMs * 1000000.0) / hBenchIterations;

	std::cout << "[参数路由-命中]     " << hBenchIterations << " 次分发, 总耗时: " << totalMs
			  << " ms, 每次: " << nsPerOp << " ns\n";

	EXPECT_LT(nsPerOp, 50000.0);
}

/**
 * @brief 大量路由下的查找性能
 */
TEST_F(RouterPerfTest, LargeRouteSet)
{
	auto router = createRouterWithStaticRoutes(1000);

	HttpRequest req;
	req.setMethod(HttpMethod::hGet);
	req.setTarget("/api/v1/route500");

	boost::asio::io_context io;

	auto start = std::chrono::high_resolution_clock::now();
	for (int i = 0; i < hBenchIterations; ++i)
	{
		boost::asio::co_spawn(
			io,
			[&]() -> Awaitable<void>
			{
				co_await router.dispatch(req);
			},
			boost::asio::detached);
	}
	io.run();
	auto end = std::chrono::high_resolution_clock::now();

	double totalMs = std::chrono::duration<double, std::milli>(end - start).count();
	double nsPerOp = (totalMs * 1000000.0) / hBenchIterations;

	std::cout << "[1000路由-中间命中] " << hBenchIterations << " 次分发, 总耗时: " << totalMs
			  << " ms, 每次: " << nsPerOp << " ns\n";

	// 哈希表：1000 路由和 100 路由应该性能相当
	EXPECT_LT(nsPerOp, 10000.0);
}

// ============ dispatchSync benchmark（P3） ============

/**
 * @brief dispatchSync 同步快速路径：sync handler -> 零协程帧分配
 * 报谷 P3 建议：补充 benchmark 覆盖 dispatchSync 路径
 */
TEST_F(RouterPerfTest, DispatchSync_SyncHandler)
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

	// dispatchSync 是纯同步调用，无需 io_context
	for (int i = 0; i < hWarmupIterations; ++i)
	{
		(void)router.dispatchSync(req);
	}

	auto start = std::chrono::high_resolution_clock::now();
	for (int i = 0; i < hBenchIterations; ++i)
	{
		(void)router.dispatchSync(req);
	}
	auto end = std::chrono::high_resolution_clock::now();

	double totalMs = std::chrono::duration<double, std::milli>(end - start).count();
	double nsPerOp = (totalMs * 1000000.0) / hBenchIterations;

	std::cout << "[dispatchSync-sync]  " << hBenchIterations
			  << " \xE6\xAC\xA1\xE5\x88\x86\xE5\x8F\x91, \xE6\x80\xBB\xE8\x80\x97\xE6\x97\xB6: " << totalMs
			  << " ms, \xE6\xAF\x8F\xE6\xAC\xA1: " << nsPerOp << " ns\n";

	// dispatchSync 无协程帧分配，应在 < 500ns
	EXPECT_LT(nsPerOp, 500.0);
}

/**
 * @brief dispatchSync fallback：async handler -> 返回 nullopt
 * 验证 dispatchSync 在 async handler 场景下快速失败不受影响
 */
TEST_F(RouterPerfTest, DispatchSync_FallbackToAsync)
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

	for (int i = 0; i < hWarmupIterations; ++i)
	{
		(void)router.dispatchSync(req);
	}

	auto start = std::chrono::high_resolution_clock::now();
	for (int i = 0; i < hBenchIterations; ++i)
	{
		auto result = router.dispatchSync(req);
		// async handler -> dispatchSync 应返回 nullopt
		EXPECT_FALSE(result.has_value());
	}
	auto end = std::chrono::high_resolution_clock::now();

	double totalMs = std::chrono::duration<double, std::milli>(end - start).count();
	double nsPerOp = (totalMs * 1000000.0) / hBenchIterations;

	std::cout << "[dispatchSync-fb-async] " << hBenchIterations
			  << " \xE6\xAC\xA1\xE5\x88\x86\xE5\x8F\x91, \xE6\x80\xBB\xE8\x80\x97\xE6\x97\xB6: " << totalMs
			  << " ms, \xE6\xAF\x8F\xE6\xAC\xA1: " << nsPerOp << " ns\n";

	// fallback 仅做 resolveRoute + 检查 type，无 handler 调用，应与 sync 相当
	EXPECT_LT(nsPerOp, 500.0);
}

/**
 * @brief dispatchSync 与 co_await dispatch 对比
 * dispatchSync(sync handler) vs dispatch(async handler, 走协程)
 * 预期差 200-400ns（协程帧分配开销）
 * MSVC 无 HALO 优化，协程帧全部堆分配，async 路径远慢于 GCC/Clang，
 * 因此 MSVC 上仅保留相对断言，绝对阈值无意义。
 */
TEST_F(RouterPerfTest, DispatchSync_Vs_Dispatch)
{
	Router router;
	// 同一条路径分别注册 sync 和 async 两个 handler
	std::string path = "/bench-compare";
	router.get(path,
			   [](const HttpRequest&) -> HttpResponse
			   {
				   return HttpResponse::ok("ok");
			   });

	HttpRequest req;
	req.setMethod(HttpMethod::hGet);
	req.setTarget(path);

	// === dispatchSync（同步快速路径）===
	for (int i = 0; i < hWarmupIterations; ++i)
	{
		(void)router.dispatchSync(req);
	}

	auto syncStart = std::chrono::high_resolution_clock::now();
	for (int i = 0; i < hBenchIterations; ++i)
	{
		(void)router.dispatchSync(req);
	}
	auto syncEnd = std::chrono::high_resolution_clock::now();

	double syncMs = std::chrono::duration<double, std::milli>(syncEnd - syncStart).count();
	double syncNs = (syncMs * 1000000.0) / hBenchIterations;

	// === dispatch（协程路径，需 io_context）===
	boost::asio::io_context io;
	auto asyncStart = std::chrono::high_resolution_clock::now();
	for (int i = 0; i < hBenchIterations; ++i)
	{
		boost::asio::co_spawn(
			io,
			[&]() -> Awaitable<void>
			{
				(void)co_await router.dispatch(req);
			},
			boost::asio::detached);
	}
	io.run();
	auto asyncEnd = std::chrono::high_resolution_clock::now();

	double asyncMs = std::chrono::duration<double, std::milli>(asyncEnd - asyncStart).count();
	double asyncNs = (asyncMs * 1000000.0) / hBenchIterations;

	double diffNs = asyncNs - syncNs;

	std::cout << "[dispatchSync-vs-dispatch]\n";
	std::cout << "  dispatchSync: " << syncNs << " ns/op\n";
	std::cout << "  co_await dispatch: " << asyncNs << " ns/op\n";
	std::cout << "  \xE5\xB7\xAE\xE5\xBC\x82 (async - sync): " << diffNs << " ns/op\n";

#ifdef _MSC_VER
	// MSVC 协程无 HALO，帧全部堆分配，async 路径比 GCC/Clang 慢 5-10x
	EXPECT_LT(syncNs, 2000.0);
	EXPECT_LT(asyncNs, 200000.0);
#else
	EXPECT_LT(syncNs, 500.0);
	EXPECT_LT(asyncNs, 10000.0);
#endif
	// dispatchSync 应快于 co_await dispatch（协程帧分配）
	EXPECT_LT(syncNs, asyncNs);
}
