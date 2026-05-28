/**
 * @file AsioTimer.h
 * @brief Boost.Asio 定时器实现
 */

#pragma once

#include "../core/Timer.h"
#include <boost/asio.hpp>
#include <atomic>
#include <functional>
#include <memory>

namespace hical
{

	class AsioEventLoop;

	/**
	 * @brief 基于 Boost.Asio 的定时器实现
	 * 独立的定时器类，支持单次和周期性定时。
	 */
	class AsioTimer
		: public Timer
		, public std::enable_shared_from_this<AsioTimer>
	{
	public:
		/**
		 * @brief 构造单次定时器
		 * @param loop 所属事件循环
		 * @param delay 延迟时间（秒）
		 * @param cb 回调函数
		 */
		AsioTimer(AsioEventLoop* loop, double delay, Callback cb);

		/**
		 * @brief 构造周期定时器
		 * @param loop 所属事件循环
		 * @param interval 间隔时间（秒）
		 * @param cb 回调函数
		 * @param repeating 是否周期执行
		 */
		AsioTimer(AsioEventLoop* loop, double interval, Callback cb, bool repeating);

		~AsioTimer() override;

		// ============ Timer 接口实现 ============

		void cancel() override;
		bool isActive() const override;
		EventLoop* getLoop() const override;
		bool isRepeating() const override;
		double interval() const override;

		// ============ 内部接口 ============

		/**
		 * @brief 启动定时器（由 EventLoop 调用）
		 */
		void start();

		/**
		 * @brief 设置定时器 ID 和完成时的清理回调
		 * @param id 定时器 ID
		 * @param cleanup 触发后的清理回调（用于从 EventLoop 的 map 中移除自身）
		 */
		void setCleanup(uint64_t id, std::function<void(uint64_t)> cleanup);

	private:
		void scheduleOnce();
		void scheduleRepeating();
		void handleTimeout(const boost::system::error_code& ec);

		AsioEventLoop* loop_;
		boost::asio::steady_timer timer_;
		Callback callback_;
		double interval_; // 秒
		bool repeating_;
		std::atomic<bool> cancelled_ {false};

		// 单次定时器自动清理：触发后通知 EventLoop 移除自身，防止 timers_ map 泄漏
		uint64_t timerId_ {0};
		std::function<void(uint64_t)> cleanupCallback_;
	};

} // namespace hical
