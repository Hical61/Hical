/**
 * @file Middleware.cpp
 * @brief 中间件管线构建与执行实现
 */

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
		auto name = "middleware_" + std::to_string(entries_.size());

		MiddlewareEntry entry;
		entry.type = MiddlewareEntry::Type::hAsync;
		entry.name = std::move(name);
		entry.asyncHandler = std::move(middleware);
		entries_.push_back(std::move(entry));
	}

	void MiddlewarePipeline::use(const std::string& name, MiddlewareHandler middleware)
	{
		if (cachedChain_)
		{
			throw std::logic_error("MiddlewarePipeline::use: cannot add middleware after build()");
		}

		MiddlewareEntry entry;
		entry.type = MiddlewareEntry::Type::hAsync;
		entry.name = name;
		entry.asyncHandler = std::move(middleware);
		entries_.push_back(std::move(entry));
	}

	void MiddlewarePipeline::use(SyncBeforeHandler before)
	{
		if (cachedChain_)
		{
			throw std::logic_error("MiddlewarePipeline::use: cannot add middleware after build()");
		}
		auto name = "sync_middleware_" + std::to_string(entries_.size());

		MiddlewareEntry entry;
		entry.type = MiddlewareEntry::Type::hSync;
		entry.name = std::move(name);
		entry.before = std::move(before);
		entries_.push_back(std::move(entry));
	}

	void MiddlewarePipeline::use(SyncBeforeHandler before, SyncAfterHandler after)
	{
		if (cachedChain_)
		{
			throw std::logic_error("MiddlewarePipeline::use: cannot add middleware after build()");
		}
		auto name = "sync_middleware_" + std::to_string(entries_.size());

		MiddlewareEntry entry;
		entry.type = MiddlewareEntry::Type::hSync;
		entry.name = std::move(name);
		entry.before = std::move(before);
		entry.after = std::move(after);
		entries_.push_back(std::move(entry));
	}

	void MiddlewarePipeline::use(const std::string& name, SyncBeforeHandler before, SyncAfterHandler after)
	{
		if (cachedChain_)
		{
			throw std::logic_error("MiddlewarePipeline::use: cannot add middleware after build()");
		}

		MiddlewareEntry entry;
		entry.type = MiddlewareEntry::Type::hSync;
		entry.name = name;
		entry.before = std::move(before);
		entry.after = std::move(after);
		entries_.push_back(std::move(entry));
	}

	void MiddlewarePipeline::build(MiddlewareNext finalHandler)
	{
		if (cachedChain_)
		{
			throw std::logic_error("MiddlewarePipeline::build: already built");
		}

#ifdef HICAL_ENABLE_MIDDLEWARE_PROFILING
		rebuildTimingStats();
		// Profiling 模式：从 entries_ 提取 async handlers（Sync 中间件视为无 handler，跳过计时）
		std::vector<MiddlewareHandler> asyncHandlers;
		asyncHandlers.reserve(entries_.size());
		for (const auto& e : entries_)
		{
			if (e.type == MiddlewareEntry::Type::hAsync)
			{
				asyncHandlers.push_back(e.asyncHandler);
			}
		}
		cachedChain_ = buildChainWithProfiling(asyncHandlers, std::move(finalHandler), timingStats_);
#else
		// 使用优化链：连续 Sync 中间件合并为单个协程帧
		cachedChain_ = buildOptimizedChain(entries_, std::move(finalHandler));
#endif
	}

	MiddlewareNext MiddlewarePipeline::buildChain(MiddlewareNext finalHandler) const
	{
		// 从 entries_ 提取 async handlers（兼容旧接口）
		std::vector<MiddlewareHandler> asyncHandlers;
		asyncHandlers.reserve(entries_.size());
		for (const auto& e : entries_)
		{
			if (e.type == MiddlewareEntry::Type::hAsync)
			{
				asyncHandlers.push_back(e.asyncHandler);
			}
		}
		return buildChainFrom(asyncHandlers, std::move(finalHandler));
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
				return mw(r, next);
			};
		}

		return current;
	}

	MiddlewareNext MiddlewarePipeline::buildOptimizedChain(const std::vector<MiddlewareEntry>& entries,
														   MiddlewareNext finalHandler)
	{
		if (entries.empty())
		{
			return finalHandler;
		}

		// 如果全是 Async，退化为 buildChainFrom
		bool hasSync = false;
		for (const auto& e : entries)
		{
			if (e.type == MiddlewareEntry::Type::hSync)
			{
				hasSync = true;
				break;
			}
		}

		if (!hasSync)
		{
			std::vector<MiddlewareHandler> asyncHandlers;
			asyncHandlers.reserve(entries.size());
			for (const auto& e : entries)
			{
				asyncHandlers.push_back(e.asyncHandler);
			}
			return buildChainFrom(asyncHandlers, std::move(finalHandler));
		}

		// 逆序构建优化链：连续 Sync 条目合并为单个协程 lambda
		MiddlewareNext current = std::move(finalHandler);

		int i = static_cast<int>(entries.size()) - 1;
		while (i >= 0)
		{
			if (entries[i].type == MiddlewareEntry::Type::hAsync)
			{
				// Async 条目：独立协程 lambda（不变）
				auto mw = entries[i].asyncHandler;
				current = [mw = std::move(mw), next = std::move(current)](HttpRequest& r) -> Awaitable<HttpResponse>
				{
					return mw(r, next);
				};
				--i;
			}
			else
			{
				// 收集连续 Sync 条目
				int syncEnd = i; // inclusive
				while (i >= 0 && entries[i].type == MiddlewareEntry::Type::hSync)
				{
					--i;
				}
				int syncStart = i + 1; // inclusive

				// 提取这组 Sync 条目的 before/after（按注册顺序）
				std::vector<SyncBeforeHandler> befores;
				std::vector<SyncAfterHandler> afters;
				for (int j = syncStart; j <= syncEnd; ++j)
				{
					if (entries[j].before)
					{
						befores.push_back(entries[j].before);
					}
					if (entries[j].after)
					{
						afters.push_back(entries[j].after);
					}
				}

				// 合并为单个协程 lambda：1 个协程帧覆盖 N 个 Sync 中间件
				current = [befores = std::move(befores), afters = std::move(afters), next = std::move(current)](
							  HttpRequest& r) -> Awaitable<HttpResponse>
				{
					// 前置：按注册顺序执行（外层中间件先执行）
					for (const auto& before : befores)
					{
						auto intercepted = before(r);
						if (intercepted)
						{
							co_return std::move(*intercepted);
						}
					}

					// 唯一的协程挂起点
					auto res = co_await next(r);

					// 后置：按注册逆序执行（洋葱模型语义：后注册的 after 先退出）
					for (int k = static_cast<int>(afters.size()) - 1; k >= 0; --k)
					{
						afters[k](r, res);
					}

					co_return res;
				};
			}
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
		// 仅为 Async 条目创建 profiling 统计（Sync 中间件不独立计时）
		for (const auto& e : entries_)
		{
			if (e.type == MiddlewareEntry::Type::hAsync)
			{
				auto stats = std::make_shared<MiddlewareTimingStats>();
				stats->name = e.name;
				timingStats_.push_back(std::move(stats));
			}
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
			std::vector<MiddlewareHandler> asyncHandlers;
			asyncHandlers.reserve(entries_.size());
			for (const auto& e : entries_)
			{
				if (e.type == MiddlewareEntry::Type::hAsync)
				{
					asyncHandlers.push_back(e.asyncHandler);
				}
			}
			return buildChainWithProfiling(asyncHandlers, std::move(finalHandler), timingStats_);
		}
#endif
		return buildOptimizedChain(entries_, std::move(finalHandler));
	}

	size_t MiddlewarePipeline::size() const
	{
		return entries_.size();
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
