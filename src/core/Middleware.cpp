#include "Middleware.h"
#include <stdexcept>

namespace hical
{

	void MiddlewarePipeline::use(MiddlewareHandler middleware)
	{
		if (cachedChain_)
		{
			throw std::logic_error("MiddlewarePipeline::use: cannot add middleware after build()");
		}
		auto idx = middlewares_.size();
		middlewares_.push_back(std::move(middleware));
		middlewareNames_.emplace_back("middleware_" + std::to_string(idx));
	}

	void MiddlewarePipeline::use(const std::string& name, MiddlewareHandler middleware)
	{
		if (cachedChain_)
		{
			throw std::logic_error("MiddlewarePipeline::use: cannot add middleware after build()");
		}
		middlewares_.push_back(std::move(middleware));
		middlewareNames_.push_back(name);
	}

	void MiddlewarePipeline::build(MiddlewareNext finalHandler)
	{
		if (cachedChain_)
		{
			throw std::logic_error("MiddlewarePipeline::build: already built");
		}

#ifdef HICAL_ENABLE_MIDDLEWARE_PROFILING
		rebuildTimingStats();
		cachedChain_ = buildChainWithProfiling(middlewares_, std::move(finalHandler), timingStats_);
#else
		cachedChain_ = buildChain(std::move(finalHandler));
#endif
	}

	MiddlewareNext MiddlewarePipeline::buildChain(MiddlewareNext finalHandler) const
	{
		return buildChainFrom(middlewares_, std::move(finalHandler));
	}

	MiddlewareNext MiddlewarePipeline::buildChainFrom(const std::vector<MiddlewareHandler>& middlewares,
													  MiddlewareNext finalHandler)
	{
		if (middlewares.empty())
		{
			return finalHandler;
		}

		MiddlewareNext current = std::move(finalHandler);

		for (int i = static_cast<int>(middlewares.size()) - 1; i >= 0; --i)
		{
			auto mw = middlewares[i];
			current = [mw = std::move(mw), next = std::move(current)](HttpRequest& r) -> Awaitable<HttpResponse>
			{
				co_return co_await mw(r, next);
			};
		}

		return current;
	}

#ifdef HICAL_ENABLE_MIDDLEWARE_PROFILING
	MiddlewareNext MiddlewarePipeline::buildChainWithProfiling(
		const std::vector<MiddlewareHandler>& middlewares,
		MiddlewareNext finalHandler,
		const std::vector<std::shared_ptr<MiddlewareTimingStats>>& stats)
	{
		if (middlewares.empty())
		{
			return finalHandler;
		}

		MiddlewareNext current = std::move(finalHandler);

		for (int i = static_cast<int>(middlewares.size()) - 1; i >= 0; --i)
		{
			auto mw = middlewares[i];
			auto statsPtr = stats[i];
			current = [mw = std::move(mw), next = std::move(current), statsPtr = std::move(statsPtr)](
						  HttpRequest& r) -> Awaitable<HttpResponse>
			{
				auto start = std::chrono::steady_clock::now();
				auto res = co_await mw(r, next);
				auto elapsed = std::chrono::steady_clock::now() - start;
				statsPtr->record(elapsed);
				co_return res;
			};
		}

		return current;
	}

	void MiddlewarePipeline::rebuildTimingStats()
	{
		timingStats_.clear();
		timingStats_.reserve(middlewares_.size());
		for (size_t i = 0; i < middlewares_.size(); ++i)
		{
			auto stats = std::make_shared<MiddlewareTimingStats>();
			stats->name = i < middlewareNames_.size() ? middlewareNames_[i] : "middleware_" + std::to_string(i);
			timingStats_.push_back(std::move(stats));
		}
	}
#endif

	Awaitable<HttpResponse> MiddlewarePipeline::execute(HttpRequest& req)
	{
		if (!cachedChain_)
		{
			throw std::logic_error("MiddlewarePipeline::execute: must call build() first");
		}
		co_return co_await cachedChain_(req);
	}

	Awaitable<HttpResponse> MiddlewarePipeline::execute(HttpRequest& req, MiddlewareNext finalHandler)
	{
		auto chain = buildChain(std::move(finalHandler));
		co_return co_await chain(req);
	}

	MiddlewareNext MiddlewarePipeline::buildFor(MiddlewareNext finalHandler) const
	{
#ifdef HICAL_ENABLE_MIDDLEWARE_PROFILING
		if (!timingStats_.empty())
		{
			return buildChainWithProfiling(middlewares_, std::move(finalHandler), timingStats_);
		}
#endif
		return buildChain(std::move(finalHandler));
	}

	size_t MiddlewarePipeline::size() const
	{
		return middlewares_.size();
	}

#ifdef HICAL_ENABLE_MIDDLEWARE_PROFILING
	std::vector<MiddlewarePipeline::TimingSnapshot> MiddlewarePipeline::getTimingStats() const
	{
		std::vector<TimingSnapshot> result;
		result.reserve(timingStats_.size());
		for (const auto& s : timingStats_)
		{
			auto totalUs = s->totalTimeUs.load(std::memory_order_relaxed);
			auto maxUs = s->maxTimeUs.load(std::memory_order_relaxed);
			auto minUs = s->minTimeUs.load(std::memory_order_relaxed);
			result.push_back(TimingSnapshot {
				.name = s->name,
				.callCount = s->callCount.load(std::memory_order_relaxed),
				.totalTimeMs = static_cast<double>(totalUs) / 1000.0,
				.avgTimeMs = s->avgTimeMs(),
				.maxTimeMs = static_cast<double>(maxUs) / 1000.0,
				.minTimeMs = minUs == std::numeric_limits<int64_t>::max() ? 0.0 : static_cast<double>(minUs) / 1000.0,
			});
		}
		return result;
	}

	void MiddlewarePipeline::resetTimingStats()
	{
		for (auto& s : timingStats_)
		{
			s->callCount.store(0, std::memory_order_relaxed);
			s->totalTimeUs.store(0, std::memory_order_relaxed);
			s->maxTimeUs.store(0, std::memory_order_relaxed);
			s->minTimeUs.store(std::numeric_limits<int64_t>::max(), std::memory_order_relaxed);
		}
	}
#endif

} // namespace hical
