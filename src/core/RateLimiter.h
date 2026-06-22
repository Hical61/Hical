/**
 * @file RateLimiter.h
 * @brief Token Bucket 速率限制中间件
 */

#pragma once

#include "HttpRequest.h"
#include "HttpResponse.h"
#include "Middleware.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>

namespace hical
{

	/**
	 * @brief 速率限制配置
	 */
	struct RateLimitConfig
	{
		double rate = 10.0;  ///< 每秒恢复的 token 数（平均速率）
		double burst = 20.0; ///< 桶容量（允许的瞬时爆发量）
	};

	/**
	 * @brief 速率限制中间件选项
	 */
	struct RateLimiterOptions
	{
		RateLimitConfig config; ///< 速率限制参数

		/**
		 * @brief Key 提取函数
		 * 默认从请求属性 "hical.remote_addr" 获取客户端 IP。
		 * 可自定义为按路由、按用户 ID、按 API key 等维度限流。
		 */
		std::function<std::string(const HttpRequest&)> keyExtractor = nullptr;

		bool rateLimitHeaders = true; ///< 是否附加 X-RateLimit-* 响应头
		size_t maxEntries = 100000;   ///< 最大条目数，防 DoS 内存耗尽
		int gcIntervalMs = 60000;     ///< 懒清理间隔（毫秒）
	};

	/**
	 * @brief 默认 remote_addr 属性键名
	 * 连接层（HttpSessionImpl）应将客户端 IP 以此键存入请求属性。
	 */
	inline constexpr const char* kRemoteAddrKey = "hical.remote_addr";

	namespace detail
	{

		/**
		 * @brief 单个 Token Bucket 桶
		 * thread-safe：每个桶独立 mutex，减少锁竞争。
		 * 只在 check() 时按需计算 refill，不设后台定时器。
		 */
		struct RateLimitBucket
		{
			mutable std::mutex mutex;

			/// 当前可用 token 数（最多 burst 个）
			double tokens;

			/// 上次 check() 的时间点（steady_clock::time_point 的 time_since_epoch().count()）
			double lastCheckNs;

			/// 最近一次访问时间（epoch 纳秒，用于 GC 判断）
			/// atomic 允许 GC（写锁持有 map）无锁读取，消除 TSAN race
			std::atomic<int64_t> lastAccessNs {0};
		};

		/**
		 * @brief 桶容器及 GC 状态（内聚在 RateLimiter 中）
		 */
		struct BucketStore
		{
			mutable std::shared_mutex mutex; ///< 保护 map 读写
			std::unordered_map<std::string, std::unique_ptr<RateLimitBucket>> buckets;

			/// 上次 GC 触发时间点
			std::chrono::steady_clock::time_point lastGc;
		};

	} // namespace detail

	/**
	 * @brief Token Bucket 速率限制器
	 * 按 key 维护 Token Bucket，支持：

	 * - 按 key 独立限流
	 * - 瞬时爆发（burst）
	 * - 线程安全（per-bucket mutex + shared_mutex for map）
	 * - 过期条目惰性清理（定期 GC）
	 */
	class RateLimiter
	{
	public:
		/**
		 * @param opts 速率限制选项
		 */
		explicit RateLimiter(RateLimiterOptions opts);

		/**
		 * @brief 检查指定 key 是否允许通过
		 * @param key 限流 key（如 IP、用户 ID、路由）
		 * @param now 当前时间点
		 * @return true 允许通过，false 被限流
		 */
		[[nodiscard]] bool check(const std::string& key, std::chrono::steady_clock::time_point now);

		/**
		 * @brief 获取配置
		 */
		[[nodiscard]] const RateLimitConfig& config() const
		{
			return opts_.config;
		}

		/**
		 * @brief 获取选项
		 */
		[[nodiscard]] const RateLimiterOptions& options() const
		{
			return opts_;
		}

		/**
		 * @brief 获取当前桶总数（近似值，仅统计用途）
		 */
		[[nodiscard]] size_t bucketCount() const;

	private:
		/**
		 * @brief 惰性清理过期桶
		 * @param now 当前时间点
		 */
		void gc(std::chrono::steady_clock::time_point now);

		RateLimiterOptions opts_;
		detail::BucketStore store_;
	};

	/**
	 * @brief 创建速率限制中间件（SyncBeforeHandler）
	 * 在请求进入路由前检查速率限制：
	 * - 未超限 → 返回 nullopt，继续执行后续中间件
	 * - 超限   → 返回 429 Too Many Requests + Retry-After 头
	 * 用法：
	 * ```cpp
	 * // 按 IP 限流：每秒 10 请求，burst 20
	 * server.use(makeRateLimiterMiddleware({
	 *     .config = {10.0, 20.0}
	 * }));
	 * // 按路由限流：每秒 100 请求，带限流响应头
	 * server.use(makeRateLimiterMiddleware({
	 *     .config = {100.0, 200.0},
	 *     .keyExtractor = [](const HttpRequest& req) {
	 *         return std::string(req.path());
	 *     }
	 * }));
	 * ```
	 * @param opts 速率限制选项
	 * @return SyncBeforeHandler
	 */
	[[nodiscard]] SyncBeforeHandler makeRateLimiterMiddleware(RateLimiterOptions opts = {});

} // namespace hical
