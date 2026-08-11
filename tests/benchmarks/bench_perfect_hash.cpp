/**
 * @file bench_perfect_hash.cpp
 * @brief PerfectHashRouter dispatchSync 性能微基准
 */

#include "core/Router.h"
#include "core/PerfectHashRouter.h"
#include "core/HttpRequest.h"
#include "core/HttpResponse.h"
#include "core/MetaRoutes.h"
#include "core/Reflection.h"
#include <benchmark/benchmark.h>
#include <string>
#include <vector>

using namespace hical;

/* ========== 辅助函数 ========== */

/** 创建含 count 个静态路由的 Router（命令式，无完美哈希） */
static Router createPlainRouter(int count)
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

/** 创建含 count 个静态路由的 Router 并注入完美哈希 */
static Router createPhrRouter(int count)
{
	Router router;
	std::vector<std::pair<HttpMethod, std::string_view>> keys;
	std::vector<std::string> paths;
	keys.reserve(count);
	paths.reserve(count);

	for (int i = 0; i < count; ++i)
	{
		paths.push_back("/api/v1/route" + std::to_string(i));
		router.get(paths.back(),
				   [](const HttpRequest&) -> HttpResponse
				   {
					   return HttpResponse::ok("ok");
				   });
		keys.emplace_back(HttpMethod::hGet, paths.back());
	}

	auto lookup = RuntimePerfectHashLookup::buildFromKeys(keys);
	if (lookup.valid())
	{
		router.setPerfectHashLookup(std::move(lookup));
	}
	return router;
}

/* ========== Benchmark ========== */

/**
 * @brief dispatchSync 不用完美哈希 — 走 unordered_map O(1) 查找
 */
static void BM_DispatchSync_WithoutPerfectHash(benchmark::State& state)
{
	auto router = createPlainRouter(100);
	HttpRequest req;
	req.setMethod(HttpMethod::hGet);
	req.setTarget("/api/v1/route50");

	for (auto _ : state)
	{
		auto result = router.dispatchSync(req);
		benchmark::DoNotOptimize(result);
	}
}

BENCHMARK(BM_DispatchSync_WithoutPerfectHash);

/**
 * @brief dispatchSync 用完美哈希 — 走 Multiply-Shift 完美哈希查找
 */
static void BM_DispatchSync_WithPerfectHash(benchmark::State& state)
{
	auto router = createPhrRouter(100);
	HttpRequest req;
	req.setMethod(HttpMethod::hGet);
	req.setTarget("/api/v1/route50");

	for (auto _ : state)
	{
		auto result = router.dispatchSync(req);
		benchmark::DoNotOptimize(result);
	}
}

BENCHMARK(BM_DispatchSync_WithPerfectHash);

/**
 * @brief 完美哈希 miss 后回退普通查找
 * 追加一条不在完美哈希表中的路由，测量回退路径性能
 */
static void BM_DispatchSync_PerfectHashMissFallback(benchmark::State& state)
{
	auto router = createPhrRouter(100);
	router.get("/fallback/extra",
			   [](const HttpRequest&) -> HttpResponse
			   {
				   return HttpResponse::ok("extra");
			   });
	HttpRequest req;
	req.setMethod(HttpMethod::hGet);
	req.setTarget("/fallback/extra");

	for (auto _ : state)
	{
		auto result = router.dispatchSync(req);
		benchmark::DoNotOptimize(result);
	}
}

BENCHMARK(BM_DispatchSync_PerfectHashMissFallback);

/**
 * @brief 完美哈希首条和末条命中 — 验证 O(1) 无位置相关性
 */
static void BM_DispatchSync_FirstHit(benchmark::State& state)
{
	auto router = createPhrRouter(100);
	HttpRequest req;
	req.setMethod(HttpMethod::hGet);
	req.setTarget("/api/v1/route0");

	for (auto _ : state)
	{
		auto result = router.dispatchSync(req);
		benchmark::DoNotOptimize(result);
	}
}

BENCHMARK(BM_DispatchSync_FirstHit);

static void BM_DispatchSync_LastHit(benchmark::State& state)
{
	auto router = createPhrRouter(100);
	HttpRequest req;
	req.setMethod(HttpMethod::hGet);
	req.setTarget("/api/v1/route99");

	for (auto _ : state)
	{
		auto result = router.dispatchSync(req);
		benchmark::DoNotOptimize(result);
	}
}

BENCHMARK(BM_DispatchSync_LastHit);

/* ========== HICAL_ROUTES 注入后性能 ========== */

namespace
{

	/** 10 个静态路由 handler，模拟反射注册 */
	struct PhrBenchHandler
	{
		HttpResponse h0(const HttpRequest&)
		{
			return HttpResponse::ok("0");
		}
		HICAL_HANDLER(Get, "/b/0", h0)

		HttpResponse h1(const HttpRequest&)
		{
			return HttpResponse::ok("1");
		}
		HICAL_HANDLER(Get, "/b/1", h1)

		HttpResponse h2(const HttpRequest&)
		{
			return HttpResponse::ok("2");
		}
		HICAL_HANDLER(Get, "/b/2", h2)

		HttpResponse h3(const HttpRequest&)
		{
			return HttpResponse::ok("3");
		}
		HICAL_HANDLER(Get, "/b/3", h3)

		HttpResponse h4(const HttpRequest&)
		{
			return HttpResponse::ok("4");
		}
		HICAL_HANDLER(Get, "/b/4", h4)

		HttpResponse h5(const HttpRequest&)
		{
			return HttpResponse::ok("5");
		}
		HICAL_HANDLER(Get, "/b/5", h5)

		HttpResponse h6(const HttpRequest&)
		{
			return HttpResponse::ok("6");
		}
		HICAL_HANDLER(Get, "/b/6", h6)

		HttpResponse h7(const HttpRequest&)
		{
			return HttpResponse::ok("7");
		}
		HICAL_HANDLER(Get, "/b/7", h7)

		HttpResponse h8(const HttpRequest&)
		{
			return HttpResponse::ok("8");
		}
		HICAL_HANDLER(Get, "/b/8", h8)

		HttpResponse h9(const HttpRequest&)
		{
			return HttpResponse::ok("9");
		}
		HICAL_HANDLER(Get, "/b/9", h9)

		HICAL_ROUTES(PhrBenchHandler, h0, h1, h2, h3, h4, h5, h6, h7, h8, h9)
	};

} // namespace

/**
 * @brief MetaRoutes 注入后的 dispatchSync 性能
 * 通过 HICAL_HANDLER + HICAL_ROUTES + meta::registerRoutes 自动注入完美哈希
 */
static void BM_DispatchSync_MetaRoutesInjected(benchmark::State& state)
{
	Router router;
	PhrBenchHandler handler;
	meta::registerRoutes(router, handler);

	HttpRequest req;
	req.setMethod(HttpMethod::hGet);
	req.setTarget("/b/5");

	for (auto _ : state)
	{
		auto result = router.dispatchSync(req);
		benchmark::DoNotOptimize(result);
	}
}

BENCHMARK(BM_DispatchSync_MetaRoutesInjected);
