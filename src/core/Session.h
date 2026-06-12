/**
 * @file Session.h
 * @brief 内存 Session 管理（读写锁 + 懒 GC）
 */

#pragma once

#include "Cookie.h"
#include "HttpRequest.h"
#include "HttpResponse.h"
#include "Middleware.h"
#include <any>
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace hical
{

	/**
	 * @brief Session 数据存储（单个会话）
	 * 以字符串键存储任意类型的值（std::any）。
	 * 线程安全：内部持有 mutex，set/get/touch 均在锁内执行，
	 * 可安全地被多个 IO 线程并发访问（Keep-Alive 多路复用场景）。
	 */
	class Session
	{
	public:
		explicit Session(std::string id) : id_(std::move(id))
		{
		}

		/**
		 * @brief 获取 Session ID（只读，构造后不变，无需加锁）
		 * @return Session ID 字符串
		 */
		[[nodiscard]] const std::string& id() const
		{
			return id_;
		}

		/**
		 * @brief 设置 Session 属性
		 * @param key 属性键
		 * @param value 任意类型的值
		 */
		void set(const std::string& key, std::any value)
		{
			std::unique_lock lk(mutex_);
			data_[key] = std::move(value);
			dirty_ = true;
		}

		/**
		 * @brief 获取 Session 属性（类型安全版）
		 * @tparam T 期望类型
		 * @param key 属性键
		 * @return 找到且类型匹配返回值，否则 nullopt
		 */
		template <typename T>
		[[nodiscard]] std::optional<T> get(const std::string& key) const
		{
			std::shared_lock lk(mutex_);
			if (auto it = data_.find(key); it == data_.end())
			{
				return std::nullopt;
			}
			else if (it->second.type() != typeid(T))
			{
				return std::nullopt;
			}
			else
			{
				return std::any_cast<T>(it->second);
			}
		}

		/**
		 * @brief 检查属性是否存在
		 * @param key 属性键
		 * @return true 如果存在
		 */
		[[nodiscard]] bool has(const std::string& key) const
		{
			std::shared_lock lk(mutex_);
			return data_.count(key) > 0;
		}

		/**
		 * @brief 删除指定属性
		 * @param key 属性键
		 */
		void remove(const std::string& key)
		{
			std::unique_lock lk(mutex_);
			data_.erase(key);
		}

		/**
		 * @brief 清空所有属性（不销毁 Session 本身）
		 */
		void clear()
		{
			std::unique_lock lk(mutex_);
			data_.clear();
		}

		/**
		 * @brief 从另一个 Session 迁移所有数据（用于 ID 再生场景）
		 * @param other 源 Session，迁移后 other 的数据被清空
		 * @note 调用者需确保 other 不会被并发访问（通常在 SessionManager 写锁保护下调用）
		 */
		void migrateFrom(Session& other)
		{
			// 先锁自己，再锁 other（固定顺序避免死锁：按 this 指针地址排序）
			if (this < &other)
			{
				std::unique_lock lk1(mutex_);
				std::unique_lock lk2(other.mutex_);
				data_ = std::move(other.data_);
				other.data_.clear();
			}
			else
			{
				std::unique_lock lk1(other.mutex_);
				std::unique_lock lk2(mutex_);
				data_ = std::move(other.data_);
				other.data_.clear();
			}
			dirty_ = true;
		}

		/**
		 * @brief 标记 Session 为已修改（由 set() 自动触发，通常无需手动调用）
		 */
		void markDirty()
		{
			std::unique_lock lk(mutex_);
			dirty_ = true;
		}

		/**
		 * @brief 是否已修改
		 * @return true 如果需要写回 Cookie
		 */
		[[nodiscard]] bool isDirty() const
		{
			std::shared_lock lk(mutex_);
			return dirty_;
		}

		/**
		 * @brief 更新最后访问时间（无锁，原子操作）
		 */
		void touch()
		{
			auto now = std::chrono::steady_clock::now();
			lastAccessNs_.store(now.time_since_epoch().count(), std::memory_order_release);
		}

		/**
		 * @brief 获取最后访问时间（无锁，原子操作）
		 */
		[[nodiscard]] std::chrono::steady_clock::time_point lastAccess() const
		{
			auto ns = lastAccessNs_.load(std::memory_order_acquire);
			return std::chrono::steady_clock::time_point(std::chrono::steady_clock::duration(ns));
		}

	private:
		std::string id_; ///< 构造后不可变，无需加锁
		mutable std::shared_mutex mutex_;
		std::unordered_map<std::string, std::any> data_;
		bool dirty_ = false;
		/// 最后访问时间（纳秒时间戳），无锁原子操作，消除与 SessionManager::mutex_ 的嵌套锁
		std::atomic<int64_t> lastAccessNs_ {std::chrono::steady_clock::now().time_since_epoch().count()};
	};

	/**
	 * @brief Session 配置选项
	 */
	struct SessionOptions
	{
		static constexpr int kDefaultMaxAge = 3600;           ///< 默认有效期 1 小时（秒）
		static constexpr int kDefaultGcInterval = 300;        ///< 默认 GC 间隔 5 分钟（秒）
		static constexpr size_t kDefaultMaxSessions = 100000; ///< 默认最大 Session 数量

		std::string cookieName = "HICAL_SESSION"; ///< Session Cookie 名称
		int maxAge = kDefaultMaxAge;              ///< Session 有效期（秒，默认 1 小时）
		bool httpOnly = true;                     ///< Cookie HttpOnly（防 XSS）
		bool secure = true;                       ///< Cookie Secure（仅 HTTPS，开发环境需显式关闭）
		std::string sameSite = "Lax";             ///< Cookie SameSite 策略
		std::string path = "/";                   ///< Cookie Path
		int gcInterval = kDefaultGcInterval;      ///< 懒 GC 触发间隔（秒，默认 5 分钟）
		size_t maxSessions = kDefaultMaxSessions; ///< 最大 Session 数量（0 = 不限制），防 DoS 内存耗尽
	};

	/**
	 * @brief Session 管理器（内存存储实现）
	 * 线程安全：所有操作加互斥锁。
	 * 生命周期：通常作为全局单例，与服务器共存亡。
	 * 用法：通过 makeSessionMiddleware() 集成到中间件管道。
	 */
	class SessionManager
	{
	public:
		explicit SessionManager(SessionOptions opts = {}) : opts_(std::move(opts))
		{
		}

		/**
		 * @brief 根据 Session ID 查找 Session
		 * @param id Session ID
		 * @return 找到返回 Session 指针，否则 nullptr
		 */
		[[nodiscard]] std::shared_ptr<Session> find(const std::string& id);

		/**
		 * @brief 创建新 Session
		 * @return 新创建的 Session（已写入存储）
		 */
		[[nodiscard]] std::shared_ptr<Session> create();

		/**
		 * @brief 销毁指定 Session（登出场景）
		 * @param id Session ID
		 */
		void destroy(const std::string& id);

		/**
		 * @brief 重新生成 Session ID（防 Session Fixation 攻击）
		 * 保留 Session 内的所有数据，仅更换 ID。旧 ID 立即失效。
		 * 典型场景：用户登录成功后调用此方法，确保攻击者无法利用预设的 Session ID。
		 * @param oldId 当前 Session ID
		 * @return 拥有相同数据但新 ID 的 Session，旧 ID 不存在时返回 nullptr
		 */
		[[nodiscard]] std::shared_ptr<Session> regenerate(const std::string& oldId);

		/**
		 * @brief 清理过期的 Session（应定期调用）
		 * @param force true 时跳过 gcInterval 双重检查，强制执行清理
		 */
		void gc(bool force = true);

		/**
		 * @brief 获取当前活跃 Session 数量
		 * @return Session 数量
		 */
		[[nodiscard]] size_t count();

		/**
		 * @brief 获取配置选项
		 * @return SessionOptions 常量引用
		 */
		[[nodiscard]] const SessionOptions& options() const
		{
			return opts_;
		}

		/**
		 * @brief 请求上下文属性键（Session 存入 HttpRequest attribute 时使用的键名）
		 */
		static constexpr const char* hSessionKey = "hical.session";

	private:
		/**
		 * @brief 生成随机 Session ID（64 位十六进制）
		 * @return Session ID 字符串
		 */
		static std::string generateId();

		SessionOptions opts_;
		mutable std::shared_mutex mutex_;
		std::unordered_map<std::string, std::shared_ptr<Session>> store_;
		/// 上次懒 GC 触发时间（在写锁保护下访问）
		std::chrono::steady_clock::time_point lastGc_ = std::chrono::steady_clock::now();
	};

	/**
	 * @brief 创建 Session 中间件
	 * Session 中间件做三件事：
	 * 1. 从请求 Cookie 中读取 Session ID，查找或创建 Session
	 * 2. 将 Session 对象注入 HttpRequest attribute（键：SessionManager::hSessionKey）
	 * 3. 请求处理完毕后，若 Session 被标记为 dirty，向响应写入 Set-Cookie
	 * 用法：
	 * ```cpp
	 * auto sessionMgr = std::make_shared<hical::SessionManager>();
	 * server.use(hical::makeSessionMiddleware(sessionMgr));
	 * // 在路由处理器中访问 Session
	 * server.router().get("/profile", [](const HttpRequest& req) -> HttpResponse {
	 *     auto session = req.getAttribute<std::shared_ptr<hical::Session>>(
	 *         hical::SessionManager::hSessionKey);
	 *     if (session && (*session)->has("user")) {
	 *         auto user = (*session)->get<std::string>("user");
	 *         return HttpResponse::ok("Hello " + *user);
	 *     }
	 *     return HttpResponse::badRequest("Not logged in");
	 * });
	 * ```
	 * @param manager Session 管理器（shared_ptr，可多个中间件共享同一个 manager）
	 * @return MiddlewareHandler 可传入 server.use() 的中间件
	 */
	inline MiddlewareHandler makeSessionMiddleware(std::shared_ptr<SessionManager> manager)
	{
		return [manager](HttpRequest& req, MiddlewareNext next) -> Awaitable<HttpResponse>
		{
			const auto& opts = manager->options();

			// 1. 从 Cookie 中读取 Session ID
			auto sessionId = req.cookie(opts.cookieName);
			std::shared_ptr<Session> session;

			if (!sessionId.empty())
			{
				session = manager->find(sessionId);
				// find() 内部会检查过期：过期 Session 被删除并返回 nullptr，
				// 随后走 create() 分配新 ID，下方 sessionId != session->id()
				// 会触发 Set-Cookie 将新 ID 写回客户端。
			}
			if (!session)
			{
				// 新建 Session（可能因达到上限返回 nullptr）
				session = manager->create();
				if (!session)
				{
					HttpResponse errRes;
					errRes.setStatus(HttpStatusCode::hServiceUnavailable);
					errRes.setBody("Service Unavailable: session store full", "text/plain");
					co_return errRes;
				}
			}
			session->touch();

			// 2. 注入到请求 attribute
			req.setAttribute(SessionManager::hSessionKey, session);

			// 3. 执行后续中间件/路由
			auto res = co_await next(req);

			// 4. 如果 Session 被写过数据（dirty），刷新 Cookie
			if (session->isDirty() || sessionId != session->id())
			{
				CookieOptions cookieOpts;
				cookieOpts.maxAge = opts.maxAge;
				cookieOpts.httpOnly = opts.httpOnly;
				cookieOpts.secure = opts.secure;
				cookieOpts.sameSite = opts.sameSite;
				cookieOpts.path = opts.path;
				res.setCookie(opts.cookieName, session->id(), cookieOpts);
			}

			co_return res;
		};
	}

} // namespace hical
