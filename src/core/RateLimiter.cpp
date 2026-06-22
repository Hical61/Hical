/**
 * @file RateLimiter.cpp
 * @brief Token Bucket 速率限制中间件实现
 */

#include "core/RateLimiter.h"
#include <algorithm>
#include <cmath>
#include <string>

namespace hical
{

	// ============== RateLimiter 实现 ==============

	RateLimiter::RateLimiter(RateLimiterOptions opts) : opts_(std::move(opts))
	{
		store_.lastGc = std::chrono::steady_clock::now();

		// 默认 key 提取：从请求属性取 remote_addr
		if (!opts_.keyExtractor)
		{
			opts_.keyExtractor = [](const HttpRequest& req) -> std::string
			{
				// 优先从请求属性取（连接层注入）
				auto attr = req.getAttribute<std::string>(kRemoteAddrKey);
				if (attr && !attr->empty())
				{
					return *attr;
				}
				// 代理场景的 X-Forwarded-For
				auto forwarded = req.header("X-Forwarded-For");
				if (!forwarded.empty())
				{
					// 取第一个 IP（最接近客户端）
					auto pos = forwarded.find(',');
					return pos != std::string_view::npos ? std::string(forwarded.substr(0, pos))
														 : std::string(forwarded);
				}
				// 兜底：连接到同一个无 remote_addr 注入的服务器时，所有请求走同一个桶
				return "global";
			};
		}
	}

	bool RateLimiter::check(const std::string& key, std::chrono::steady_clock::time_point now)
	{
		auto nowNs = now.time_since_epoch().count();

		// 惰性 GC：每 1024 次触发一次过期桶清理
		// gc() 内部有 gcIntervalMs 双重检查，不会每次都扫全表
		{
			static thread_local uint32_t gcCounter = 0;
			if (++gcCounter % 1024 == 0)
			{
				gc(now);
			}
		}

		// 1. 查找或创建桶（读锁优先）
		{
			std::shared_lock lock(store_.mutex);
			auto it = store_.buckets.find(key);
			if (it != store_.buckets.end())
			{
				auto& bucket = *it->second;
				std::lock_guard bkLock(bucket.mutex);

				auto elapsed = (nowNs - bucket.lastAccessNs.load(std::memory_order_relaxed)) / 1e9;
				double rate = opts_.config.rate;
				double burst = opts_.config.burst;

				// 按时间间隔 refill token
				bucket.tokens = std::min(bucket.tokens + elapsed * rate, burst);
				bucket.lastAccessNs.store(nowNs, std::memory_order_relaxed);

				if (bucket.tokens >= 1.0)
				{
					bucket.tokens -= 1.0;
					return true;
				}
				return false;
			}
		}

		// 2. 未找到，创建新桶（写锁）
		//    先检查 map 容量
		{
			std::unique_lock lock(store_.mutex);

			// 双重检查（释放读锁后可能已被创建）
			auto it = store_.buckets.find(key);
			if (it != store_.buckets.end())
			{
				auto& bucket = *it->second;
				std::lock_guard bkLock(bucket.mutex);

				auto elapsed = (nowNs - bucket.lastAccessNs.load(std::memory_order_relaxed)) / 1e9;
				bucket.tokens = std::min(bucket.tokens + elapsed * opts_.config.rate, opts_.config.burst);
				bucket.lastAccessNs.store(nowNs, std::memory_order_relaxed);

				if (bucket.tokens >= 1.0)
				{
					bucket.tokens -= 1.0;
					return true;
				}
				return false;
			}

			// 达到上限时拒绝新条目（防 DoS）
			if (store_.buckets.size() >= opts_.maxEntries)
			{
				return false;
			}

			auto newBucket = std::make_unique<detail::RateLimitBucket>();
			newBucket->tokens = std::max(opts_.config.burst - 1.0, 0.0); // 新桶初始满，消费当前这一个
			newBucket->lastAccessNs.store(nowNs, std::memory_order_relaxed);
			store_.buckets.emplace(key, std::move(newBucket));
		}

		// 3. 新桶创建后，当前请求已消费一个 token
		return true;
	}

	size_t RateLimiter::bucketCount() const
	{
		std::shared_lock lock(store_.mutex);
		return store_.buckets.size();
	}

	void RateLimiter::gc(std::chrono::steady_clock::time_point now)
	{
		// 检查是否需要 GC（双重检查：调用方已锁，但内部仍要检查间隔）
		constexpr auto kIdleThreshold = std::chrono::seconds(300); // 5 分钟无访问即过期

		std::unique_lock lock(store_.mutex);

		auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - store_.lastGc);
		if (elapsed.count() < opts_.gcIntervalMs)
		{
			return;
		}
		store_.lastGc = now;

		// 遍历删除过期桶
		for (auto it = store_.buckets.begin(); it != store_.buckets.end();)
		{
			// lastAccessNs 是 atomic，GC 在 map 写锁下无锁读取，
			// check() 用 relaxed store 更新，差几微秒不影响 GC 过期判定
			auto lastAccess = std::chrono::steady_clock::time_point {
				std::chrono::steady_clock::duration(it->second->lastAccessNs.load(std::memory_order_relaxed))};
			if ((now - lastAccess) > kIdleThreshold)
			{
				it = store_.buckets.erase(it);
			}
			else
			{
				++it;
			}
		}
	}

	// ============== 中间件工厂 ==============

	SyncBeforeHandler makeRateLimiterMiddleware(RateLimiterOptions opts)
	{
		auto limiter = std::make_shared<RateLimiter>(std::move(opts));

		return [limiter](HttpRequest& req) -> SyncMiddlewareResult
		{
			auto now = std::chrono::steady_clock::now();

			// 提取 key
			auto key = limiter->options().keyExtractor(req);

			// 检查限流
			if (limiter->check(key, now))
			{
				// 通过
				return std::nullopt;
			}

			// 被限流：返回 429
			HttpResponse res;
			res.setStatus(HttpStatusCode::hTooManyRequests);
			res.setBody("429 Too Many Requests", "text/plain");

			// Retry-After：等 1 个 token 的时间（秒）
			double rate = limiter->config().rate;
			if (rate > 0)
			{
				int retryAfterSec = static_cast<int>(std::ceil(1.0 / rate));
				res.setHeader("Retry-After", std::to_string(retryAfterSec));
			}

			// 限流响应头
			double burst = limiter->config().burst;
			double remaining = 0.0; // 被限流时剩余 < 1
			int resetSec = static_cast<int>(std::ceil(burst / rate));

			res.setHeader("X-RateLimit-Limit", std::to_string(static_cast<int>(rate)));
			res.setHeader("X-RateLimit-Remaining", std::to_string(static_cast<int>(remaining)));
			res.setHeader("X-RateLimit-Reset", std::to_string(resetSec));

			return res;
		};
	}

} // namespace hical
