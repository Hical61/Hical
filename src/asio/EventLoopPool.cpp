/**
 * @file EventLoopPool.cpp
 * @brief 事件循环池实现
 */

#include "EventLoopPool.h"

#ifdef __linux__
	#include <pthread.h>
	#include <sched.h>
#endif

namespace hical
{

	EventLoopPool::EventLoopPool(size_t numThreads)
	{
		for (size_t i = 0; i < numThreads; ++i)
		{
			auto loop = std::make_unique<AsioEventLoop>();
			loop->setIndex(i);
			loops_.push_back(std::move(loop));
		}
	}

	EventLoopPool::~EventLoopPool()
	{
		if (running_.load())
		{
			stop();
		}
	}

	void EventLoopPool::start()
	{
		if (running_.exchange(true))
		{
			return; // 已经启动
		}

		for (size_t i = 0; i < loops_.size(); ++i)
		{
			auto* ptr = loops_[i].get();
			threads_.emplace_back(
				[ptr, i]()
				{
#ifdef __linux__
					// 绑核，别让内核随便迁移线程搞 TLB flush
					cpu_set_t cpuset;
					CPU_ZERO(&cpuset);
					CPU_SET(i % std::thread::hardware_concurrency(), &cpuset);
					pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
#else
					(void)i;
#endif
					ptr->run();
				});
		}
	}

	void EventLoopPool::stop()
	{
		if (!running_.exchange(false))
		{
			return; // 已经停止
		}

		// 不用 io_context::stop() 强杀了——放掉 work_guard 让 worker 干完手头活自己退
		for (auto& loop : loops_)
		{
			loop->releaseWork();
		}

		for (auto& thread : threads_)
		{
			if (thread.joinable())
			{
				thread.join();
			}
		}

		threads_.clear();
	}

	void EventLoopPool::releaseWork()
	{
		for (auto& loop : loops_)
		{
			loop->releaseWork();
		}
	}

	AsioEventLoop* EventLoopPool::getNextLoop()
	{
		if (loops_.empty())
		{
			return nullptr;
		}

		size_t index = nextIndex_.fetch_add(1) % loops_.size();
		return loops_[index].get();
	}

	std::vector<AsioEventLoop*> EventLoopPool::getAllLoops()
	{
		std::vector<AsioEventLoop*> result;
		result.reserve(loops_.size());
		for (auto& loop : loops_)
		{
			result.push_back(loop.get());
		}
		return result;
	}

	size_t EventLoopPool::size() const
	{
		return loops_.size();
	}

	bool EventLoopPool::isRunning() const
	{
		return running_.load();
	}

} // namespace hical
