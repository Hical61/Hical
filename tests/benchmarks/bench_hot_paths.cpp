/**
 * @file bench_hot_paths.cpp
 * @brief 参数路由、嵌套 JSON 和首次表单解析的对照微基准
 */

#include "core/CompileTimeJson.h"
#include "core/HttpRequest.h"
#include "core/Router.h"
#include <benchmark/benchmark.h>
#include <string>
#include <vector>

using namespace hical;

namespace
{

	constexpr int64_t kRequestBatchSize = 32;

	enum class RouteProbe
	{
		hHit,
		hMiss,
		hMethodNotAllowed
	};

	struct FlatDto
	{
		std::string name;
		std::string description;
		int count;

		HICAL_JSON(FlatDto, name, description, count)
	};

	struct DtoGroup
	{
		std::string title;
		std::vector<FlatDto> items;

		HICAL_JSON(DtoGroup, title, items)
	};

	struct NestedDto
	{
		std::string version;
		DtoGroup group;

		HICAL_JSON(NestedDto, version, group)
	};

	// 参数放在最后一个字面量之前，让未命中的候选也经过参数段。
	template <RouteProbe Probe, bool StaticRoute = false>
	void routeResolve(benchmark::State& state)
	{
		const auto routeCount = state.range(0);
		const std::string parameter(static_cast<size_t>(state.range(1)), 'x');
		Router router;
		for (int64_t i = 0; i < routeCount; ++i)
		{
			router.get("/items/" + (StaticRoute ? parameter : "{id}") + "/route" + std::to_string(i),
					   [](const HttpRequest&) -> HttpResponse
					   {
						   return HttpResponse::ok();
					   });
		}
		const auto target =
			"/items/" + parameter + "/route" + std::to_string(Probe == RouteProbe::hMiss ? routeCount : routeCount - 1);
		const auto method = Probe == RouteProbe::hMethodNotAllowed ? HttpMethod::hPost : HttpMethod::hGet;
		std::vector<HttpRequest> requests;
		requests.reserve(kRequestBatchSize);

		// 每批重新构造请求，避免反复命中同一个请求的参数容量；准备和析构不计时。
		while (state.KeepRunningBatch(kRequestBatchSize))
		{
			state.PauseTiming();
			for (int64_t i = 0; i < kRequestBatchSize; ++i)
			{
				auto& req = requests.emplace_back();
				req.setMethod(method);
				req.setTarget(target);
			}
			state.ResumeTiming();
			for (auto& req : requests)
			{
				auto result = router.resolveRoute(req);
				benchmark::DoNotOptimize(result);
			}
			state.PauseTiming();
			requests.clear();
			state.ResumeTiming();
		}
		state.SetItemsProcessed(state.iterations());
	}

	void paramExists(benchmark::State& state)
	{
		Router router;
		const std::string parameter(static_cast<size_t>(state.range(1)), 'x');
		for (int64_t i = 0; i < state.range(0); ++i)
		{
			router.get("/items/{id}/route" + std::to_string(i),
					   [](const HttpRequest&) -> HttpResponse
					   {
						   return HttpResponse::ok();
					   });
		}
		const auto target = "/items/" + parameter + "/route" + std::to_string(state.range(0) - 1);
		for (auto _ : state)
		{
			benchmark::DoNotOptimize(router.exists(HttpMethod::hGet, target));
		}
	}

	void flatJson(benchmark::State& state)
	{
		const FlatDto dto {"item", std::string(static_cast<size_t>(state.range(0)), 'x'), 42};
		for (auto _ : state)
		{
			auto json = meta::compileTimeToJson(dto);
			benchmark::DoNotOptimize(json);
		}
	}

	void nestedJson(benchmark::State& state)
	{
		const NestedDto dto {
			"v1",
			{"items",
			 std::vector<FlatDto>(static_cast<size_t>(state.range(0)),
								  {"item", std::string(static_cast<size_t>(state.range(1)), 'x'), 42})}};
		for (auto _ : state)
		{
			auto json = meta::compileTimeToJson(dto);
			benchmark::DoNotOptimize(json);
		}
	}

	void firstFormParse(benchmark::State& state)
	{
		const auto bodySize = static_cast<size_t>(state.range(0));
		const std::string key = "value=";
		const auto body = key + std::string(bodySize - key.size(), 'x');
		std::vector<HttpRequest> requests;
		requests.reserve(kRequestBatchSize);
		while (state.KeepRunningBatch(kRequestBatchSize))
		{
			state.PauseTiming();
			for (int64_t i = 0; i < kRequestBatchSize; ++i)
			{
				auto& req = requests.emplace_back();
				req.setMethod(HttpMethod::hPost);
				req.setHeader("Content-Type", "application/x-www-form-urlencoded");
				req.setBody(body);
			}
			state.ResumeTiming();
			for (const auto& req : requests)
			{
				auto* params = &req.formParams();
				benchmark::DoNotOptimize(params);
			}
			state.PauseTiming();
			requests.clear();
			state.ResumeTiming();
		}
		state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(bodySize));
		state.SetItemsProcessed(state.iterations());
	}

} // namespace

BENCHMARK_TEMPLATE(routeResolve, RouteProbe::hHit)->ArgsProduct({{1, 16, 128}, {8, 256}});
BENCHMARK_TEMPLATE(routeResolve, RouteProbe::hMiss)->ArgsProduct({{1, 16, 128}, {8, 256}});
BENCHMARK_TEMPLATE(routeResolve, RouteProbe::hMethodNotAllowed)->ArgsProduct({{1, 16, 128}, {8, 256}});
BENCHMARK_TEMPLATE(routeResolve, RouteProbe::hHit, true)->ArgsProduct({{1, 16, 128}, {8, 256}});
BENCHMARK_TEMPLATE(routeResolve, RouteProbe::hMiss, true)->ArgsProduct({{1, 16, 128}, {8, 256}});
BENCHMARK_TEMPLATE(routeResolve, RouteProbe::hMethodNotAllowed, true)->ArgsProduct({{1, 16, 128}, {8, 256}});
BENCHMARK(paramExists)->ArgsProduct({{1, 16, 128}, {8, 256}});
BENCHMARK(flatJson)->Arg(8)->Arg(256);
BENCHMARK(nestedJson)->ArgsProduct({{1, 16, 128}, {8, 256}});
BENCHMARK(firstFormParse)->Arg(128)->Arg(64 * 1024);
