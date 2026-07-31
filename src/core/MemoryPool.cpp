/**
 * @file MemoryPool.cpp
 * @brief 三级 PMR 内存池实现
 */

#include "MemoryPool.h"
#ifdef HICAL_USE_MIMALLOC
	#include "MimallocResource.h"
#endif
#ifdef HICAL_HAS_NUMA
	#include <numa.h>
	#include <sched.h>
#endif
#include <cassert>
#include <stdexcept>

namespace hical
{

	MemoryPool::MemoryPool()
		: trackedResource_(
#ifdef HICAL_USE_MIMALLOC
			  &mimallocResource()
#else
			  std::pmr::new_delete_resource()
#endif
				  )
		, globalPool_(std::pmr::pool_options {.max_blocks_per_chunk = config_.globalMaxBlocksPerChunk,
											  .largest_required_pool_block = config_.globalLargestPoolBlock},
					  &trackedResource_)
	{
	}

	MemoryPool::~MemoryPool()
	{
		// 先清理所有线程本地池（它们引用 globalPool_ 作为上游）
		std::lock_guard<std::mutex> lock(threadPoolsMutex_);
		threadPools_.clear();
	}

	MemoryPool& MemoryPool::instance()
	{
		static MemoryPool instance;
		return instance;
	}

	void MemoryPool::configure(const PoolConfig& config)
	{
		// configure() 得在启动前、单线程时调，不然 globalPool_ 重建会炸
		assert(generation_.load(std::memory_order_relaxed) == 0
			   || !"configure() should only be called before server starts");

		config_ = config;

		// 先清理所有线程本地池（它们引用 globalPool_ 作为上游）
		{
			std::lock_guard<std::mutex> lock(threadPoolsMutex_);
			threadPools_.clear();
		}

		// placement new 重建全局池
		globalPool_.~synchronized_pool_resource();
		new (&globalPool_) std::pmr::synchronized_pool_resource(
			std::pmr::pool_options {.max_blocks_per_chunk = config_.globalMaxBlocksPerChunk,
									.largest_required_pool_block = config_.globalLargestPoolBlock},
			&trackedResource_);

		// 递增代际计数器，使所有 thread_local 缓存失效
		generation_.fetch_add(1, std::memory_order_release);
	}

	std::pmr::polymorphic_allocator<std::byte> MemoryPool::globalAllocator()
	{
		return std::pmr::polymorphic_allocator<std::byte>(&globalPool_);
	}

	std::pmr::polymorphic_allocator<std::byte> MemoryPool::threadLocalAllocator()
	{
		auto* pool = getOrCreateThreadPool();
		return std::pmr::polymorphic_allocator<std::byte>(pool);
	}

	std::pmr::unsynchronized_pool_resource* MemoryPool::threadLocalPool()
	{
		return getOrCreateThreadPool();
	}

	std::unique_ptr<std::pmr::monotonic_buffer_resource> MemoryPool::createRequestPool(size_t initialSize)
	{
		if (initialSize == 0)
		{
			initialSize = config_.requestPoolInitialSize;
		}
		// upstream 使用调用线程的 thread-local 无锁池，避免扩容时走全局有锁池
		// 安全前提：monotonic buffer 必须在同一线程上创建和销毁（协程保证）
		return std::make_unique<std::pmr::monotonic_buffer_resource>(initialSize, threadLocalPool());
	}

	std::pmr::unsynchronized_pool_resource* MemoryPool::getOrCreateThreadPool()
	{
		// thread_local 缓存，generation 变了就重建
		struct ThreadCache
		{
			std::pmr::unsynchronized_pool_resource* pool = nullptr;
			ThreadPoolEntry* entry = nullptr;
			uint64_t generation = 0;
		};

		thread_local ThreadCache cache;

		auto currentGen = generation_.load(std::memory_order_acquire);
		if (cache.pool != nullptr && cache.generation == currentGen)
		{
			// GC 标记了就 release（只能由拥有线程做，跨线程 UB）
			if (cache.entry->needsRelease.exchange(false, std::memory_order_acquire))
			{
				cache.pool->release();
			}
			// 更新活跃时间戳（无锁）
			cache.entry->lastAllocTime.store(std::chrono::steady_clock::now(), std::memory_order_relaxed);
			return cache.pool;
		}

#ifdef HICAL_HAS_NUMA
		// 设为当前线程所在 NUMA 节点的内存亲和，让后续 PMR 分配优先走本地内存
		int cpu = sched_getcpu();
		int node = numa_node_of_cpu(cpu);
		if (node >= 0)
		{
			numa_set_preferred(node);
		}
#endif
		auto pool = std::make_unique<std::pmr::unsynchronized_pool_resource>(
			std::pmr::pool_options {.max_blocks_per_chunk = config_.threadLocalMaxBlocksPerChunk,
									.largest_required_pool_block = config_.threadLocalLargestPoolBlock},
			&globalPool_);

		auto* poolPtr = pool.get();
		auto entry = std::make_unique<ThreadPoolEntry>(std::move(pool));
		auto* entryPtr = entry.get();

		{
			std::lock_guard<std::mutex> lock(threadPoolsMutex_);
			threadPools_.push_back(std::move(entry));
		}

		cache.pool = poolPtr;
		cache.entry = entryPtr;
		cache.generation = currentGen;
		return poolPtr;
	}

	MemoryPool::Stats MemoryPool::getStats() const
	{
		size_t poolCount = 0;
		{
			std::lock_guard<std::mutex> lock(threadPoolsMutex_);
			poolCount = threadPools_.size();
		}

		return Stats {
			.totalAllocations = trackedResource_.totalAllocations(),
			.totalDeallocations = trackedResource_.totalDeallocations(),
			.currentBytesAllocated = trackedResource_.currentBytes(),
			.peakBytesAllocated = trackedResource_.peakBytes(),
			.threadPoolCount = poolCount,
			.gcCycles = gcCycles_.load(std::memory_order_relaxed),
			.gcReclaimedPools = gcReclaimedPools_.load(std::memory_order_relaxed),
		};
	}

	void MemoryPool::resetStats()
	{
		trackedResource_.resetStats();
		gcCycles_.store(0, std::memory_order_relaxed);
		gcReclaimedPools_.store(0, std::memory_order_relaxed);
	}

	void MemoryPool::gc(std::chrono::seconds maxIdleSeconds)
	{
		auto now = std::chrono::steady_clock::now();
		size_t reclaimed = 0;

		{
			std::lock_guard<std::mutex> lock(threadPoolsMutex_);
			for (auto& entry : threadPools_)
			{
				auto lastActive = entry->lastAllocTime.load(std::memory_order_relaxed);
				if (now - lastActive > maxIdleSeconds)
				{
					// 标记一下，让拥有线程自己 release
					entry->needsRelease.store(true, std::memory_order_release);
					++reclaimed;
				}
			}
		}

		gcCycles_.fetch_add(1, std::memory_order_relaxed);
		gcReclaimedPools_.fetch_add(reclaimed, std::memory_order_relaxed);
	}

} // namespace hical
