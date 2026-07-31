/**
 * @file EventLoopPool.cpp
 * @brief 事件循环池实现
 */

#include "EventLoopPool.h"
#include "core/NumaTopology.h"

#ifdef __linux__
	#include <pthread.h>
	#include <sched.h>
#endif

namespace hical
{

	EventLoopPool::EventLoopPool(size_t numThreads, int concurrencyHint)
	{
		for (size_t i = 0; i < numThreads; ++i)
		{
			auto loop = std::make_unique<AsioEventLoop>(concurrencyHint);
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

		// 主线程调一次 detect（幂等），把 isNuma 结果传进 lambda 避免跨线程引用
		auto& topo = NumaTopology::instance();
		topo.detect();
		bool numa = topo.isNuma();

		for (size_t i = 0; i < loops_.size(); ++i)
		{
			auto* ptr = loops_[i].get();
			threads_.emplace_back(
				[ptr, i, numa]()
				{
#ifdef __linux__
					cpu_set_t cpuset;
					CPU_ZERO(&cpuset);

					if (numa)
					{
						// NUMA 感知绑核：线程均匀分到各 NUMA 节点，节点内轮询绑 CPU
						// 比如 2 节点各 8 核，8 线程 → 每节点 4 线程，分别绑 node0 CPU 0-3、node1 CPU 0-3
						const auto& nodes = NumaTopology::instance().nodes();
						size_t nodeCount = nodes.size();
						size_t nodeIdx = i % nodeCount;
						const auto& node = nodes[nodeIdx];
						if (node.cpuCount_ > 0)
						{
							int cpuIdx = static_cast<int>((i / nodeCount) % static_cast<size_t>(node.cpuCount_));
							CPU_SET(node.cpuList_[cpuIdx], &cpuset);
						}
					}
					else
					{
						// 非 NUMA：简单轮询绑核，和以前完全一样
						CPU_SET(i % std::thread::hardware_concurrency(), &cpuset);
					}
					pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
#else
					(void)i;
					(void)numa;
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

		// 最少连接数策略：选当前活跃连接最少的 loop
		AsioEventLoop* best = loops_[0].get();
		size_t minCount = best->connectionCount();
		for (size_t i = 1; i < loops_.size(); ++i)
		{
			size_t count = loops_[i]->connectionCount();
			if (count < minCount)
			{
				minCount = count;
				best = loops_[i].get();
			}
		}
		return best;
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
