/**
 * @file EventLoop.h
 * @brief 事件循环抽象接口
 */

#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory_resource>

namespace hical
{

	using Func = std::function<void()>;
	using TimerId = uint64_t;

	enum : TimerId
	{
		hInvalidTimerId = 0
	};

	/**
	 * @brief 事件循环抽象接口（hical 风格）
	 * 定义了事件循环的核心操作，采用现代 C++ 命名风格。
	 * 与旧版 IEventLoop 的区别：
	 * - loop() -> run()
	 * - quit() -> stop()
	 * - queueInLoop() -> post()
	 * - runInLoop() -> dispatch()
	 * - 新增 pmr 分配器支持
	 */
	class EventLoop
	{
	public:
		virtual ~EventLoop() = default;

		// ============ 生命周期 ============

		/**
		 * @brief 启动事件循环（阻塞直到退出）
		 */
		virtual void run() = 0;

		/**
		 * @brief 停止事件循环
		 */
		virtual void stop() = 0;

		/**
		 * @brief 事件循环是否正在运行
		 * @return true 如果正在运行
		 */
		virtual bool isRunning() const = 0;

		// ============ 任务调度 ============

		/**
		 * @brief 在事件循环线程中执行任务（智能调度）
		 * @param cb 要执行的回调
		 * @note 如果当前线程就是事件循环线程，则直接执行；否则投递到队列
		 */
		virtual void dispatch(Func cb) = 0;

		/**
		 * @brief 将任务投递到事件循环队列（总是异步）
		 * @param cb 要执行的回调
		 * @note 无论当前是否在事件循环线程，都投递到队列异步执行
		 */
		virtual void post(Func cb) = 0;

		// ============ 定时器 ============

		/**
		 * @brief 延迟执行任务
		 * @param delay 延迟时间（秒）
		 * @param cb 要执行的回调
		 * @return 定时器 ID，可用于取消
		 */
		virtual TimerId runAfter(double delay, Func cb) = 0;

		/**
		 * @brief 延迟执行任务（chrono 版本）
		 * @param delay 延迟时间
		 * @param cb 要执行的回调
		 * @return 定时器 ID
		 */
		TimerId runAfter(const std::chrono::duration<double>& delay, Func cb)
		{
			return runAfter(delay.count(), std::move(cb));
		}

		/**
		 * @brief 周期性执行任务
		 * @param interval 间隔时间（秒）
		 * @param cb 要执行的回调
		 * @return 定时器 ID，可用于取消
		 */
		virtual TimerId runEvery(double interval, Func cb) = 0;

		/**
		 * @brief 周期性执行任务（chrono 版本）
		 * @param interval 间隔时间
		 * @param cb 要执行的回调
		 * @return 定时器 ID
		 */
		TimerId runEvery(const std::chrono::duration<double>& interval, Func cb)
		{
			return runEvery(interval.count(), std::move(cb));
		}

		/**
		 * @brief 取消定时器
		 * @param id 定时器 ID
		 */
		virtual void cancelTimer(TimerId id) = 0;

		// ============ 线程属性 ============

		/**
		 * @brief 当前线程是否为事件循环所属线程
		 * @return true 如果在事件循环线程中
		 */
		virtual bool isInLoopThread() const = 0;

		/**
		 * @brief 获取事件循环索引（用于多线程池场景）
		 * @return 索引值
		 */
		virtual size_t index() const = 0;

		/**
		 * @brief 设置事件循环索引
		 * @param index 索引值
		 */
		virtual void setIndex(size_t index) = 0;

		// ============ 退出钩子 ============

		/**
		 * @brief 注册退出时执行的回调
		 * @param cb 要执行的回调
		 */
		virtual void runOnQuit(Func cb) = 0;

		// ============ pmr 支持 ============

		/**
		 * @brief 获取事件循环关联的 pmr 分配器
		 * @return pmr 分配器（通常是线程本地池）
		 */
		virtual std::pmr::polymorphic_allocator<std::byte> allocator() const = 0;
	};

} // namespace hical
