#pragma once

#include "../core/Timer.h"
#include <boost/asio.hpp>
#include <atomic>
#include <memory>

namespace hical
{

	class AsioEventLoop;

	/**
 * @brief 基于 Boost.Asio 的定时器实现
 *
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
	};

} // namespace hical
