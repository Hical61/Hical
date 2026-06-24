/**
 * @file AsioEventLoop.h
 * @brief Boost.Asio 事件循环实现
 */

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
	 * 将 hical::EventLoop 接口映射到 Boost.Asio io_context。
	 * 线程模型：1 Thread : 1 io_context
	 * 每个实例持有独立的 per-thread pmr pool resource
	 */
	class AsioEventLoop : public EventLoop
	{
	public:
		/**
		 * @brief 构造 Asio 事件循环
		 * @param concurrencyHint Asio io_context 并发提示
		 *   - 1（默认）: 单线程模式，reactor 可聚合事件注册减少 epoll_ctl
		 *   - 0: 无限并发（完全线程安全，epoll_ctl 次数更多）
		 *   Hical 的 1:1 线程模型下，传 1 是最优选择
		 */
		explicit AsioEventLoop(int concurrencyHint = 1);
		~AsioEventLoop() override;

		// ============ 生命周期 ============

		void run() override;
		void stop() override;
		bool isRunning() const override;

		/**
		 * @brief 只放掉 work_guard，不强制 stop
		 * 跟 stop() 的区别：stop() 是核弹（立刻中断 run），这个是温柔退出——
		 * 先把 pending op 都 cancel 掉，再调这个，协程自然退出后 run() 就返回了。
		 */
		void releaseWork();

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

		// ============ 连接负载统计 ============

		/// 当前 loop 上挂了多少连接，getNextLoop() 靠这个做负载均衡
		void incrementConnections()
		{
			connectionCount_.fetch_add(1, std::memory_order_relaxed);
		}

		void decrementConnections()
		{
			connectionCount_.fetch_sub(1, std::memory_order_relaxed);
		}

		[[nodiscard]] size_t connectionCount() const
		{
			return connectionCount_.load(std::memory_order_relaxed);
		}

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
		std::atomic<std::thread::id> threadId_;
		std::atomic<bool> running_ {false};
		std::atomic<bool> quit_ {false};
		size_t index_ {0};
		std::atomic<size_t> connectionCount_ {0};

		// 定时器管理（使用独立 AsioTimer 类）
		std::atomic<TimerId> nextTimerId_ {1};
		std::map<TimerId, std::shared_ptr<AsioTimer>> timers_;
		std::mutex timersMutex_;

		// 退出回调
		std::vector<Func> quitCallbacks_;
		std::mutex quitMutex_;
	};

} // namespace hical
