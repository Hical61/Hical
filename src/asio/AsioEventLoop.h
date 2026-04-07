#pragma once

#include "../core/EventLoop.h"
#include "../core/MemoryPool.h"
#include <boost/asio.hpp>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

namespace hical
{

	class AsioTimer;

	/**
 * @brief 基于 Boost.Asio 的事件循环实现
 *
 * 将 hical::EventLoop 接口映射到 Boost.Asio io_context。
 * 线程模型：1 Thread : 1 io_context
 * 每个实例持有独立的 per-thread pmr pool resource
 */
	class AsioEventLoop : public EventLoop
	{
	public:
		AsioEventLoop();
		~AsioEventLoop() override;

		// ============ 生命周期 ============

		void run() override;
		void stop() override;
		bool isRunning() const override;

		// ============ 任务调度 ============

		void dispatch(Func cb) override;
		void post(Func cb) override;

		// ============ 定时器 ============

		TimerId runAfter(double delay, Func cb) override;
		TimerId runEvery(double interval, Func cb) override;
		void cancelTimer(TimerId id) override;

		// ============ 线程属性 ============

		bool isInLoopThread() const override;
		size_t index() const override;
		void setIndex(size_t index) override;

		// ============ 退出钩子 ============

		void runOnQuit(Func cb) override;

		// ============ pmr 支持 ============

		std::pmr::polymorphic_allocator<std::byte> allocator() const override;

		// ============ Asio 特有接口 ============

		/**
     * @brief 获取底层 io_context
     * @return io_context 引用
     */
		boost::asio::io_context& getIoContext()
		{
			return ioContext_;
		}

	private:
		boost::asio::io_context ioContext_;
		std::unique_ptr<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>> workGuard_;
		std::thread::id threadId_;
		std::atomic<bool> running_ {false};
		std::atomic<bool> quit_ {false};
		size_t index_ {0};

		// 定时器管理（使用独立 AsioTimer 类）
		std::atomic<TimerId> nextTimerId_ {1};
		std::map<TimerId, std::shared_ptr<AsioTimer>> timers_;
		std::mutex timersMutex_;

		// 退出回调
		std::vector<Func> quitCallbacks_;
		std::mutex quitMutex_;
	};

} // namespace hical
