#include "AsioEventLoop.h"
#include "AsioTimer.h"

namespace hical
{

	AsioEventLoop::AsioEventLoop()
		: workGuard_(std::make_unique<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>>(
			  boost::asio::make_work_guard(ioContext_)))
	{
	}

	AsioEventLoop::~AsioEventLoop()
	{
		if (running_.load())
		{
			stop();
		}
	}

	// ============ 生命周期 ============

	void AsioEventLoop::run()
	{
		threadId_.store(std::this_thread::get_id(), std::memory_order_release);
		running_.store(true);
		quit_.store(false);

		// 运行事件循环（阻塞）
		ioContext_.run();

		// 执行退出回调
		{
			std::lock_guard<std::mutex> lock(quitMutex_);
			for (auto& cb : quitCallbacks_)
			{
				cb();
			}
			quitCallbacks_.clear();
		}

		running_.store(false);
	}

	void AsioEventLoop::stop()
	{
		// 确保只有第一个调用者执行实际停止操作，防止并发 reset 竞争
		if (quit_.exchange(true))
		{
			return;
		}
		workGuard_.reset();
		ioContext_.stop();
	}

	bool AsioEventLoop::isRunning() const
	{
		return running_.load() && !quit_.load();
	}

	// ============ 任务调度 ============

	void AsioEventLoop::dispatch(Func cb)
	{
		if (isInLoopThread())
		{
			cb();
		}
		else
		{
			post(std::move(cb));
		}
	}

	void AsioEventLoop::post(Func cb)
	{
		boost::asio::post(ioContext_, std::move(cb));
	}

	// ============ 定时器 ============

	TimerId AsioEventLoop::runAfter(double delay, Func cb)
	{
		TimerId id = nextTimerId_.fetch_add(1);

		auto timer = std::make_shared<AsioTimer>(this, delay, std::move(cb));

		// 设置自动清理回调：单次定时器触发后自动从 map 中移除，防止内存泄漏
		timer->setCleanup(id,
						  [this](uint64_t timerId)
						  {
							  std::lock_guard<std::mutex> lock(timersMutex_);
							  timers_.erase(timerId);
						  });

		{
			std::lock_guard<std::mutex> lock(timersMutex_);
			timers_[id] = timer;
		}

		timer->start();
		return id;
	}

	TimerId AsioEventLoop::runEvery(double interval, Func cb)
	{
		TimerId id = nextTimerId_.fetch_add(1);

		auto timer = std::make_shared<AsioTimer>(this, interval, std::move(cb), true);

		{
			std::lock_guard<std::mutex> lock(timersMutex_);
			timers_[id] = timer;
		}

		timer->start();
		return id;
	}

	void AsioEventLoop::cancelTimer(TimerId id)
	{
		std::lock_guard<std::mutex> lock(timersMutex_);
		auto it = timers_.find(id);
		if (it != timers_.end())
		{
			it->second->cancel();
			timers_.erase(it);
		}
	}

	// ============ 线程属性 ============

	bool AsioEventLoop::isInLoopThread() const
	{
		return threadId_.load(std::memory_order_acquire) == std::this_thread::get_id();
	}

	size_t AsioEventLoop::index() const
	{
		return index_;
	}

	void AsioEventLoop::setIndex(size_t index)
	{
		index_ = index;
	}

	// ============ 退出钩子 ============

	void AsioEventLoop::runOnQuit(Func cb)
	{
		std::lock_guard<std::mutex> lock(quitMutex_);
		quitCallbacks_.push_back(std::move(cb));
	}

	// ============ pmr 支持 ============

	std::pmr::polymorphic_allocator<std::byte> AsioEventLoop::allocator() const
	{
		return MemoryPool::instance().threadLocalAllocator();
	}

} // namespace hical
