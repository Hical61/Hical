#pragma once

#include "HttpRequest.h"
#include "HttpResponse.h"
#include "Coroutine.h"
#include <chrono>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
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

#ifdef HICAL_ENABLE_MIDDLEWARE_PROFILING
	/**
	 * @brief 单个中间件的计时统计（线程安全）
	 * 使用 int64_t 微秒存储，保证所有平台上 atomic<int64_t> 都是 lock-free。
	 * 仅在 HICAL_ENABLE_MIDDLEWARE_PROFILING 编译选项开启时可用。
	 */
	struct MiddlewareTimingStats
	{
		std::string name;
		std::atomic<size_t> callCount {0};
		std::atomic<int64_t> totalTimeUs {0};                                 // 微秒
		std::atomic<int64_t> maxTimeUs {0};                                   // 微秒
		std::atomic<int64_t> minTimeUs {std::numeric_limits<int64_t>::max()}; // 微秒

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
		Awaitable<HttpResponse> execute(HttpRequest& req);

		/**
		 * @brief 执行中间件管道（始终按传入的 finalHandler 动态构建调用链）
		 * @param req HTTP 请求
		 * @param finalHandler 最终处理器
		 * @return 协程化的 HTTP 响应
		 * @note 此重载始终动态构建链，profiling 开启时不记录统计数据。
		 */
		Awaitable<HttpResponse> execute(HttpRequest& req, MiddlewareNext finalHandler);

		/**
		 * @brief 预构建自定义终端处理器的调用链
		 * @param finalHandler 自定义终端处理器
		 * @return 预构建好的调用链（可缓存后多次调用）
		 * @note profiling 开启时，返回的链共享 build() 时创建的统计对象。
		 */
		MiddlewareNext buildFor(MiddlewareNext finalHandler) const;

		/**
		 * @brief 获取中间件数量
		 * @return 数量
		 */
		size_t size() const;

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

		std::vector<TimingSnapshot> getTimingStats() const;

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

		/**
		 * @brief 从指定中间件列表构建洋葱调用链
		 * @param middlewares 中间件列表
		 * @param finalHandler 最终处理器
		 * @return 构建好的调用链
		 */
		static MiddlewareNext buildChainFrom(const std::vector<MiddlewareHandler>& middlewares,
											 MiddlewareNext finalHandler);

#ifdef HICAL_ENABLE_MIDDLEWARE_PROFILING
		/**
		 * @brief 构建带计时统计的洋葱调用链
		 */
		static MiddlewareNext buildChainWithProfiling(const std::vector<MiddlewareHandler>& middlewares,
													  MiddlewareNext finalHandler,
													  const std::vector<std::shared_ptr<MiddlewareTimingStats>>& stats);

		/**
		 * @brief 从当前 middlewares_ 和 middlewareNames_ 创建新的 timingStats_
		 */
		void rebuildTimingStats();
#endif

		std::vector<MiddlewareHandler> middlewares_;
		std::vector<std::string> middlewareNames_;
		MiddlewareNext cachedChain_;

#ifdef HICAL_ENABLE_MIDDLEWARE_PROFILING
		std::vector<std::shared_ptr<MiddlewareTimingStats>> timingStats_;
#endif
	};

} // namespace hical
