/**
 * @file Middleware.h
 * @brief 洋葱模型中间件管线与同步快速路径
 */

#pragma once

#include "HttpRequest.h"
#include "HttpResponse.h"
#include "Coroutine.h"
#include <chrono>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace hical
{

	/**
	 * @brief 中间件 next 回调类型
	 * 调用 next 会继续执行下一个中间件或最终的路由处理器。
	 */
	using MiddlewareNext = std::function<Awaitable<HttpResponse>(HttpRequest&)>;

	/**
	 * @brief 中间件处理器类型
	 * 接收请求和 next 回调，返回协程化的响应。
	 * 中间件可以在调用 next 前后执行逻辑（洋葱模型）。
	 * 示例：
	 * ```cpp
	 * auto logger = [](HttpRequest& req, MiddlewareNext next)
	 *     -> Awaitable<HttpResponse> {
	 *     std::cout << req.path() << std::endl;    // 前置逻辑
	 *     auto res = co_await next(req);            // 调用下一层
	 *     std::cout << res.statusCode() << std::endl; // 后置逻辑
	 *     co_return res;
	 * };
	 * ```
	 */
	using MiddlewareHandler = std::function<Awaitable<HttpResponse>(HttpRequest&, MiddlewareNext)>;

	/**
	 * @brief 同步中间件结果
	 * nullopt = 继续执行 next，有值 = 拦截返回（短路）
	 */
	using SyncMiddlewareResult = std::optional<HttpResponse>;

	/**
	 * @brief 同步前置中间件处理器
	 * 在 next() 之前执行。返回 nullopt 表示继续，返回 HttpResponse 表示拦截。
	 * 不需要协程，无协程帧开销。
	 */
	using SyncBeforeHandler = std::function<SyncMiddlewareResult(HttpRequest&)>;

	/**
	 * @brief 同步后置中间件处理器
	 * 在 next() 返回后执行，可修改响应头（setHeader/setCookie 等）。不需要协程。
	 * @warning 禁止修改响应体（body）。因为 Content-Length 已由 setBody/setJsonBody 中的
	 * prepare_payload() 固定，修改 body 会导致 Content-Length 与实际不匹配，
	 * 在反向代理后可能引发 HTTP 响应走私或客户端截断。
	 * 若需修改响应体，请使用异步 MiddlewareHandler 并在返回前调用 setBody()。
	 */
	using SyncAfterHandler = std::function<void(HttpRequest&, HttpResponse&)>;

	/**
	 * @brief 中间件条目（统一存储）
	 * 支持异步（完整洋葱模型）和同步（前/后分离，极低开销）两种形式。
	 * 连续的 Sync 条目会被合并为单个协程帧执行，大幅降低中间件链开销。
	 */
	struct MiddlewareEntry
	{
		enum class Type
		{
			hAsync,
			hSync
		};

		Type type;
		std::string name;

		// Async：完整洋葱模型（每层一个协程帧）
		MiddlewareHandler asyncHandler;

		// Sync：前/后分离（连续 Sync 共享一个协程帧）
		SyncBeforeHandler before;
		SyncAfterHandler after; // 可为空
	};

#ifdef HICAL_ENABLE_MIDDLEWARE_PROFILING
	/**
	 * @brief 单个中间件的计时统计（线程安全）
	 * 使用 int64_t 微秒存储，保证所有平台上 atomic<int64_t> 都是 lock-free。
	 * 仅在 HICAL_ENABLE_MIDDLEWARE_PROFILING 编译选项开启时可用。
	 * 缓存行优化：每个 atomic 计数器 alignas(64) 独占 cache line，
	 * 消除多 IO 线程并发 record() 时的 false sharing。
	 * name 移至末尾（冷数据，仅查询统计时读取）。
	 */
	struct MiddlewareTimingStats
	{
		// 热数据：每次请求经过中间件都写入，各自独占 cache line
		alignas(64) std::atomic<size_t> callCount {0};
		alignas(64) std::atomic<int64_t> totalTimeUs {0};                                 // 微秒
		alignas(64) std::atomic<int64_t> maxTimeUs {0};                                   // 微秒
		alignas(64) std::atomic<int64_t> minTimeUs {std::numeric_limits<int64_t>::max()}; // 微秒

		// 冷数据：仅查询统计时读取
		std::string name;

		double avgTimeMs() const
		{
			auto count = callCount.load(std::memory_order_relaxed);
			return count > 0 ? static_cast<double>(totalTimeUs.load(std::memory_order_relaxed)) / (1000.0 * count)
							 : 0.0;
		}

		void record(std::chrono::steady_clock::duration elapsed)
		{
			auto us = std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count();
			callCount.fetch_add(1, std::memory_order_relaxed);
			totalTimeUs.fetch_add(us, std::memory_order_relaxed);

			// 更新最大值（CAS）
			auto curMax = maxTimeUs.load(std::memory_order_relaxed);
			while (us > curMax && !maxTimeUs.compare_exchange_weak(curMax, us, std::memory_order_relaxed))
			{
			}

			// 更新最小值（CAS）
			auto curMin = minTimeUs.load(std::memory_order_relaxed);
			while (us < curMin && !minTimeUs.compare_exchange_weak(curMin, us, std::memory_order_relaxed))
			{
			}
		}
	};
#endif

	/**
	 * @brief 中间件管道（洋葱模型）
	 * 按注册顺序依次执行中间件，最后执行最终处理器。
	 * 每个中间件可以决定是否调用 next 继续执行，或直接返回响应（拦截）。
	 */
	class MiddlewarePipeline
	{
	public:
		MiddlewarePipeline() = default;

		/**
		 * @brief 添加中间件
		 * @param middleware 中间件处理器
		 * @throw std::logic_error 当 build() 已调用后再 use() 时抛出
		 * @note 必须在 build() 之前调用
		 */
		void use(MiddlewareHandler middleware);

		/**
		 * @brief 添加命名中间件（启用 profiling 时记录名称用于统计）
		 * @param name 中间件名称
		 * @param middleware 中间件处理器
		 * @throw std::logic_error 当 build() 已调用后再 use() 时抛出
		 */
		void use(const std::string& name, MiddlewareHandler middleware);

		/**
		 * @brief 添加同步前置中间件（无协程帧开销）
		 * @param before 前置处理器（返回 nullopt 继续，返回 HttpResponse 拦截）
		 */
		void use(SyncBeforeHandler before);

		/**
		 * @brief 添加同步前/后中间件（无协程帧开销）
		 * @param before 前置处理器
		 * @param after 后置处理器（可为空）
		 */
		void use(SyncBeforeHandler before, SyncAfterHandler after);

		/**
		 * @brief 添加命名同步中间件
		 * @param name 中间件名称
		 * @param before 前置处理器
		 * @param after 后置处理器（可为空）
		 */
		void use(const std::string& name, SyncBeforeHandler before, SyncAfterHandler after = nullptr);

		/**
		 * @brief 预构建中间件调用链
		 * @param finalHandler 最终处理器（通常是路由分发）
		 * @throw std::logic_error 当重复调用 build() 时抛出
		 * 调用后，execute() 直接使用缓存的调用链，避免每次请求重建。
		 * 调用后不应再 use() 添加中间件。
		 */
		void build(MiddlewareNext finalHandler);

		/**
		 * @brief 执行中间件管道（预构建路径，无需传入 finalHandler）
		 * @param req HTTP 请求
		 * @return 协程化的 HTTP 响应
		 * @throw std::logic_error 当未调用 build() 时抛出
		 */
		[[nodiscard]] Awaitable<HttpResponse> execute(HttpRequest& req);

		/**
		 * @brief 执行中间件管道（始终按传入的 finalHandler 动态构建调用链）
		 * @param req HTTP 请求
		 * @param finalHandler 最终处理器
		 * @return 协程化的 HTTP 响应
		 * @note 此重载始终动态构建链，profiling 开启时不记录统计数据。
		 */
		[[nodiscard]] Awaitable<HttpResponse> execute(HttpRequest& req, MiddlewareNext finalHandler);

		/**
		 * @brief 预构建自定义终端处理器的调用链
		 * @param finalHandler 自定义终端处理器
		 * @return 预构建好的调用链（可缓存后多次调用）
		 * @note profiling 开启时，返回的链共享 build() 时创建的统计对象。
		 */
		[[nodiscard]] MiddlewareNext buildFor(MiddlewareNext finalHandler) const;

		/**
		 * @brief 获取中间件数量
		 * @return 数量
		 */
		[[nodiscard]] size_t size() const;

		/**
		 * @brief 从指定中间件列表构建洋葱调用链（供 RouteGroup 等外部使用）
		 * @param middlewares 中间件列表
		 * @param finalHandler 最终处理器
		 * @return 构建好的调用链
		 */
		[[nodiscard]] static MiddlewareNext buildChainFrom(const std::vector<MiddlewareHandler>& middlewares,
														   MiddlewareNext finalHandler);

		/**
		 * @brief 从 MiddlewareEntry 列表构建优化链
		 * 连续的 Sync 条目合并为单个协程帧执行，大幅减少协程帧堆分配。
		 * @param entries 中间件条目列表
		 * @param finalHandler 最终处理器
		 * @return 构建好的调用链
		 */
		[[nodiscard]] static MiddlewareNext buildOptimizedChain(const std::vector<MiddlewareEntry>& entries,
																MiddlewareNext finalHandler);

#ifdef HICAL_ENABLE_MIDDLEWARE_PROFILING
		/**
		 * @brief 获取各中间件的计时统计快照
		 * @return 各层中间件的统计数据（按注册顺序）
		 */
		struct TimingSnapshot
		{
			std::string name;
			size_t callCount;
			double totalTimeMs;
			double avgTimeMs;
			double maxTimeMs;
			double minTimeMs;
		};

		[[nodiscard]] std::vector<TimingSnapshot> getTimingStats() const;

		/**
		 * @brief 重置所有计时统计
		 */
		void resetTimingStats();
#endif

	private:
		/**
		 * @brief 从中间件列表构建洋葱调用链
		 * @param finalHandler 最终处理器
		 * @return 构建好的调用链
		 */
		MiddlewareNext buildChain(MiddlewareNext finalHandler) const;

#ifdef HICAL_ENABLE_MIDDLEWARE_PROFILING
		/**
		 * @brief 构建带计时统计的洋葱调用链
		 */
		static MiddlewareNext buildChainWithProfiling(const std::vector<MiddlewareHandler>& middlewares,
													  MiddlewareNext finalHandler,
													  const std::vector<std::shared_ptr<MiddlewareTimingStats>>& stats);

		/**
		 * @brief 从 entries_ 中的 Async 条目重建 timingStats_
		 */
		void rebuildTimingStats();
#endif

		std::vector<MiddlewareEntry> entries_;
		MiddlewareNext cachedChain_;

#ifdef HICAL_ENABLE_MIDDLEWARE_PROFILING
		std::vector<std::shared_ptr<MiddlewareTimingStats>> timingStats_;
#endif
	};

} // namespace hical
