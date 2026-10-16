/**
 * @file Middleware.cpp
 * @brief 中间件管线构建与执行实现
 */

#include "Middleware.h"
#include "RequestDispatch.h"
#include "Log.h"
#include <cstddef>
#include <optional>
#include <stdexcept>

namespace hical
{

	namespace
	{
		/**
		 * @brief 链上的 body 门：命中 needsBody 声明之前先把请求体读进来
		 * 门不捕获任何 per-request 状态，是个能缓存进 cachedChain_ 的静态节点；真正「怎么读」
		 * 由每请求写在请求内部槽上的 readBody 决定（handleSession 挂上去的，见任务 3）。
		 * 槽没写或没挂 readBody 就直接放行——WS/SSE 那条链的 req 本来就不塞槽，走到这儿无害。
		 * 读失败（readBody 返回有值）就把那个错误响应原样抛出去短路，后面的中间件和 handler 都不跑。
		 */
		Awaitable<HttpResponse> bodyGate(HttpRequest& req, MiddlewareNext next)
		{
			auto* ctx = detail::RequestDispatchContext::get(req);
			if (ctx != nullptr && ctx->readBody != nullptr)
			{
				if (auto err = co_await ctx->readBody(ctx->bodyObj); err.has_value())
				{
					co_return std::move(*err);
				}
			}
			co_return co_await next(req);
		}

		/**
		 * @brief 在第一个 needsBody 条目前插一道 body 门，产出新条目列表
		 * 门尽量靠后：读 body 是慢操作，能让前面认证/限流先拦掉的无效请求就不该白读。
		 * 多个 needsBody 也只插一道——body 读一次就够，多插会把它读重。
		 * 一个都没声明时返回 nullopt，调用方沿用原列表，链结构与加这个特性之前逐字节相同。
		 * @param entries 原始条目列表
		 * @return 需要插门时返回插好门的新列表，否则 nullopt
		 */
		std::optional<std::vector<MiddlewareEntry>> withBodyGate(const std::vector<MiddlewareEntry>& entries)
		{
			size_t firstNeedsBody = entries.size();
			for (size_t i = 0; i < entries.size(); ++i)
			{
				if (entries[i].needsBody)
				{
					firstNeedsBody = i;
					break;
				}
			}

			if (firstNeedsBody == entries.size())
			{
				return std::nullopt;
			}

			auto at = entries.begin() + static_cast<std::ptrdiff_t>(firstNeedsBody);

			std::vector<MiddlewareEntry> gated;
			gated.reserve(entries.size() + 1);
			gated.insert(gated.end(), entries.begin(), at);

			// 门是 hAsync：它前后哪边是连续 Sync 都会被这道门截断，合并不到一起。
			// 声明 needsBody 就得认这个代价，不声明的管线压根不走这里。
			MiddlewareEntry gate;
			gate.type = MiddlewareEntry::Type::hAsync;
			gate.name = "body_gate";
			gate.asyncHandler = bodyGate;
			gated.push_back(std::move(gate));

			gated.insert(gated.end(), at, entries.end());
			return gated;
		}

#ifdef HICAL_ENABLE_MIDDLEWARE_PROFILING
		/**
		 * @brief entries 里有没有声明 needsBody 的条目（profiling 分支判断用）
		 */
		bool hasNeedsBody(const std::vector<MiddlewareEntry>& entries)
		{
			for (const auto& e : entries)
			{
				if (e.needsBody)
				{
					return true;
				}
			}
			return false;
		}
#endif
	} // namespace

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

	void MiddlewarePipeline::use(SyncAfterHandler after)
	{
		if (cachedChain_)
		{
			throw std::logic_error("MiddlewarePipeline::use: cannot add middleware after build()");
		}
		auto name = "sync_middleware_" + std::to_string(entries_.size());

		MiddlewareEntry entry;
		entry.type = MiddlewareEntry::Type::hSync;
		entry.name = std::move(name);
		entry.after = std::move(after);
		entries_.push_back(std::move(entry));
	}

	void MiddlewarePipeline::use(const std::string& name, SyncAfterHandler after)
	{
		if (cachedChain_)
		{
			throw std::logic_error("MiddlewarePipeline::use: cannot add middleware after build()");
		}

		MiddlewareEntry entry;
		entry.type = MiddlewareEntry::Type::hSync;
		entry.name = name;
		entry.after = std::move(after);
		entries_.push_back(std::move(entry));
	}

	void MiddlewarePipeline::useBody(MiddlewareHandler middleware)
	{
		if (cachedChain_)
		{
			throw std::logic_error("MiddlewarePipeline::useBody: cannot add middleware after build()");
		}
		auto name = "middleware_" + std::to_string(entries_.size());

		MiddlewareEntry entry;
		entry.type = MiddlewareEntry::Type::hAsync;
		entry.name = std::move(name);
		entry.asyncHandler = std::move(middleware);
		entry.needsBody = true;
		entries_.push_back(std::move(entry));
	}

	void MiddlewarePipeline::useBody(const std::string& name, MiddlewareHandler middleware)
	{
		if (cachedChain_)
		{
			throw std::logic_error("MiddlewarePipeline::useBody: cannot add middleware after build()");
		}

		MiddlewareEntry entry;
		entry.type = MiddlewareEntry::Type::hAsync;
		entry.name = name;
		entry.asyncHandler = std::move(middleware);
		entry.needsBody = true;
		entries_.push_back(std::move(entry));
	}

	void MiddlewarePipeline::useBody(SyncBeforeHandler before, SyncAfterHandler after)
	{
		if (cachedChain_)
		{
			throw std::logic_error("MiddlewarePipeline::useBody: cannot add middleware after build()");
		}
		auto name = "sync_middleware_" + std::to_string(entries_.size());

		MiddlewareEntry entry;
		entry.type = MiddlewareEntry::Type::hSync;
		entry.name = std::move(name);
		entry.before = std::move(before);
		entry.after = std::move(after);
		entry.needsBody = true;
		entries_.push_back(std::move(entry));
	}

	void MiddlewarePipeline::useBody(const std::string& name, SyncBeforeHandler before, SyncAfterHandler after)
	{
		if (cachedChain_)
		{
			throw std::logic_error("MiddlewarePipeline::useBody: cannot add middleware after build()");
		}

		MiddlewareEntry entry;
		entry.type = MiddlewareEntry::Type::hSync;
		entry.name = name;
		entry.before = std::move(before);
		entry.after = std::move(after);
		entry.needsBody = true;
		entries_.push_back(std::move(entry));
	}

	void MiddlewarePipeline::build(MiddlewareNext finalHandler)
	{
		if (cachedChain_)
		{
			throw std::logic_error("MiddlewarePipeline::build: already built");
		}

#ifdef HICAL_ENABLE_MIDDLEWARE_PROFILING
		if (hasSyncEntries() || hasNeedsBody(entries_))
		{
			// profiling 链是按 vector<MiddlewareHandler> 搭的，而 hSync 条目压根没有 handler 可挂，
			// body 门又得靠普通链插——两者都挂不上这条链。Sync 中间件在 profiling 链上是「不执行」
			// 不是「不计时」（认证、限流这类会静默 fail-open）；body 门缺失则会让 needsBody 中间件
			// 拿到空 body。宁可少统计也不能少执行/少读 body，整条管线退回普通链。
			HICAL_LOG_WARN("MiddlewarePipeline: 本管线含 Sync 或 needsBody 中间件，profiling 链处理不了它们"
						   "（前者丢执行，后者漏 body 门），已回退到不丢条目的普通链，本管线 profiling 关闭");
			// 统计对象留着只会变成一串 callCount 永远为 0 的假行，一同清掉
			timingStats_.clear();
			cachedChain_ = buildOptimizedChain(entries_, std::move(finalHandler));
			return;
		}

		rebuildTimingStats();
		// Profiling 模式：从 entries_ 提取 async handlers（这里已确保没有 Sync 条目，不会丢东西）
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

#ifdef HICAL_ENABLE_MIDDLEWARE_PROFILING
	bool MiddlewarePipeline::hasSyncEntries() const
	{
		for (const auto& e : entries_)
		{
			if (e.type == MiddlewareEntry::Type::hSync)
			{
				return true;
			}
		}
		return false;
	}
#endif

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
		// 声明了 needsBody 就先插一道 body 门。没声明的管线 chainEntries 就是原列表，
		// 后面走的分支、合并方式全都不受影响，链逐字节和加这个特性之前一样。
		auto gated = withBodyGate(entries);
		const std::vector<MiddlewareEntry>& chainEntries = gated.has_value() ? *gated : entries;

		if (chainEntries.empty())
		{
			return finalHandler;
		}

		// 如果全是 Async，退化为 buildChainFrom
		bool hasSync = false;
		for (const auto& e : chainEntries)
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
			asyncHandlers.reserve(chainEntries.size());
			for (const auto& e : chainEntries)
			{
				asyncHandlers.push_back(e.asyncHandler);
			}
			return buildChainFrom(asyncHandlers, std::move(finalHandler));
		}

		// 逆序构建优化链：连续 Sync 条目合并为单个协程 lambda
		MiddlewareNext current = std::move(finalHandler);

		int i = static_cast<int>(chainEntries.size()) - 1;
		while (i >= 0)
		{
			if (chainEntries[i].type == MiddlewareEntry::Type::hAsync)
			{
				// Async 条目：独立协程 lambda（不变）
				auto mw = chainEntries[i].asyncHandler;
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
				while (i >= 0 && chainEntries[i].type == MiddlewareEntry::Type::hSync)
				{
					--i;
				}
				int syncStart = i + 1; // inclusive

				// 提取这组 Sync 条目的 before/after（按注册顺序）
				std::vector<SyncBeforeHandler> befores;
				std::vector<SyncAfterHandler> afters;
				for (int j = syncStart; j <= syncEnd; ++j)
				{
					if (chainEntries[j].before)
					{
						befores.push_back(chainEntries[j].before);
					}
					if (chainEntries[j].after)
					{
						afters.push_back(chainEntries[j].after);
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
		// 这里直接返回缓存的 awaitable，别再包一层 co_return co_await——
		// 包一层只为转发，白白多一个协程帧（分配 + 挂起/恢复 + 析构）。
		// 注意：函数从协程变成普通函数后，上面那个抛异常从「恢复时抛」变成「调用时抛」，
		// 调用点（HttpSessionImpl::processRequest）的 try 两种都接得住，语义不变。
		return cachedChain_(req);
	}

	Awaitable<HttpResponse> MiddlewarePipeline::execute(HttpRequest& req, MiddlewareNext finalHandler)
	{
		// 走优化链，Sync 条目也一起进链——以前这里只挑 hAsync，Sync 中间件在任何模式下都被吞掉
		auto chain = buildOptimizedChain(entries_, std::move(finalHandler));
		co_return co_await chain(req);
	}

	MiddlewareNext MiddlewarePipeline::buildFor(MiddlewareNext finalHandler) const
	{
#ifdef HICAL_ENABLE_MIDDLEWARE_PROFILING
		// build() 已经把「含 Sync / needsBody 就清空统计」这步做掉了，这里只是兜底：
		// buildFor() 是 const 公开接口，不保证一定在 build() 之后调用，别让 profiling 链
		// 有任何机会拿到带 Sync 或 needsBody 的 entries_（前者丢执行，后者漏 body 门）。
		if (!timingStats_.empty() && !hasSyncEntries() && !hasNeedsBody(entries_))
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
