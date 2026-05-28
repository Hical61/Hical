/**
 * @file Timer.h
 * @brief 定时器抽象接口
 */

#pragma once

#include <chrono>
#include <functional>
#include <memory>

namespace hical
{

	class EventLoop;

	/**
	 * @brief 定时器抽象接口
	 * 独立的定时器类，从 EventLoop 中分离出来。
	 * 支持单次和周期性定时器。
	 */
	class Timer
	{
	public:
		using Callback = std::function<void()>;

		virtual ~Timer() = default;

		/**
		 * @brief 取消定时器
		 */
		virtual void cancel() = 0;

		/**
		 * @brief 定时器是否仍然活跃
		 * @return true 如果定时器未被取消且未过期
		 */
		virtual bool isActive() const = 0;

		/**
		 * @brief 获取定时器所属的事件循环
		 * @return 事件循环指针
		 */
		virtual EventLoop* getLoop() const = 0;

		/**
		 * @brief 是否为周期性定时器
		 * @return true 如果是周期定时器
		 */
		virtual bool isRepeating() const = 0;

		/**
		 * @brief 获取定时器间隔（秒）
		 * @return 间隔时间，单次定时器返回 0
		 */
		virtual double interval() const = 0;
	};

} // namespace hical
