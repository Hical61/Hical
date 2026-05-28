/**
 * @file DbConnectionPool.h
 * @brief 协程连接池（稳态定时器信号量）
 */

#pragma once

#include "DbConfig.h"
#include "DbConnection.h"
#include "core/Coroutine.h"
#include <atomic>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

namespace hical::db
{

	/**
	 * @brief 数据库连接工厂函数类型
	 * 由具体后端（MysqlConnection 等）提供，用于创建新连接。
	 * @param ioCtx io_context 引用
	 * @param config 数据库配置
	 * @return 协程化的连接创建
	 */
	using DbConnectionFactory =
		std::function<Awaitable<std::shared_ptr<DbConnection>>(boost::asio::io_context&, const DbConfig&)>;

	/**
	 * @brief 协程化数据库连接池
	 * 设计要点：
	 * - 不使用 condition_variable（会阻塞 io_context 线程）
	 * - 使用 steady_timer 作为协程信号量：池满时 acquire() 挂起协程等待，
	 *   release() 时 cancel() timer 唤醒等待者
	 * - LIFO 复用策略：优先返回最近归还的连接（缓存友好）
	 * - 空闲连接定期回收（超过 idleTimeout 的连接被销毁）
	 * - 连接归还时自动回滚残留事务
	 */
	class DbConnectionPool : public std::enable_shared_from_this<DbConnectionPool>
	{
	public:
		/**
		 * @param ioCtx io_context 引用（连接池生命周期内必须有效）
		 * @param config 数据库配置
		 * @param factory 连接创建工厂函数
		 */
		explicit DbConnectionPool(boost::asio::io_context& ioCtx, DbConfig config, DbConnectionFactory factory);

		~DbConnectionPool();

		/**
		 * @brief 异步初始化连接池
		 * 预创建 minConnections 个连接，启动空闲回收定时器。
		 * 必须在 server.start() 之前调用。
		 */
		Awaitable<void> init();

		/**
		 * @brief 异步获取连接
		 * 1. 有空闲连接 → ping 检活后直接返回
		 * 2. 未达上限 → 创建新连接
		 * 3. 池满 → 协程挂起等待归还（超时抛 std::runtime_error）
		 * @return 可用的数据库连接
		 * @throws std::runtime_error 获取超时或连接池已关闭
		 */
		[[nodiscard]] Awaitable<std::shared_ptr<DbConnection>> acquire();

		/**
		 * @brief 归还连接到池
		 * 自动检测残留事务并回滚。
		 * 如果有等待者，直接转交连接（不入池）。
		 * @param conn 要归还的连接
		 */
		void release(std::shared_ptr<DbConnection> conn);

		/**
		 * @brief 关闭连接池
		 * 唤醒所有等待者（令其获取失败），清理空闲连接。
		 */
		Awaitable<void> shutdown();

		// ============ 统计 ============

		/// 正在被业务使用的连接数
		[[nodiscard]] size_t activeCount() const;

		/// 空闲连接数
		[[nodiscard]] size_t idleCount() const;

		/// 等待获取连接的协程数
		[[nodiscard]] size_t waitingCount() const;

		/// 总连接数（活跃 + 空闲）
		[[nodiscard]] size_t totalCount() const;

		// ============ 常量键 ============

		/// 请求属性键：连接池本身
		static constexpr const char* hPoolKey = "hical.db.pool";

		/// 请求属性键：当前请求的数据库连接
		static constexpr const char* hConnKey = "hical.db.conn";

	private:
		/// 尝试唤醒一个等待者，将连接转交给它
		void wakeOneWaiter(std::shared_ptr<DbConnection> conn);

		/// 启动空闲连接回收定时器
		void startIdleChecker();

		/// 空闲连接回收协程
		Awaitable<void> idleCheckLoop();

		/// 启动后台健康检查定时器
		void startHealthChecker();

		/// 后台健康检查协程：定期 ping 空闲连接，剔除死连接并补充
		Awaitable<void> healthCheckLoop();

		boost::asio::io_context& ioCtx_;
		DbConfig config_;
		DbConnectionFactory factory_;

		mutable std::mutex mutex_;

		/// 空闲连接栈（LIFO）
		std::vector<std::shared_ptr<DbConnection>> idle_;

		/// 当前被分发出去的连接数
		size_t activeCount_ = 0;

		/// 等待获取连接的协程队列
		struct Waiter
		{
			std::shared_ptr<boost::asio::steady_timer> timer;
			/// 堆分配的结果槽：消除协程栈帧销毁后的悬垂指针风险
			std::shared_ptr<std::shared_ptr<DbConnection>> result;
		};

		std::deque<Waiter> waiters_;

		/// 连接池是否已关闭
		std::atomic<bool> shutdown_ {false};

		/// 是否已初始化
		bool initialized_ = false;

		/// 后台循环使用的 timer（shutdown 时 cancel 以立即唤醒退出）
		std::shared_ptr<boost::asio::steady_timer> idleCheckTimer_;
		std::shared_ptr<boost::asio::steady_timer> healthCheckTimer_;
	};

} // namespace hical::db
