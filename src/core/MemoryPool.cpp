#include "MemoryPool.h"
#include <cassert>
#include <stdexcept>

namespace hical
{

	MemoryPool::MemoryPool()
		: trackedResource_(std::pmr::new_delete_resource())
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
		// 安全检查：configure() 必须在服务器启动前、单线程环境中调用。
		// 如果有通过 createRequestPool() 创建的 monotonic_buffer_resource 仍在使用中，
		// 析构旧 globalPool_ 会导致 use-after-free。
		assert(generation_.load(std::memory_order_relaxed) == 0
			   || !"configure() should only be called before server starts");

		config_ = config;

		// 先清理所有线程本地池（它们引用 globalPool_ 作为上游）
		{
			std::lock_guard<std::mutex> lock(threadPoolsMutex_);
			threadPools_.clear();
		}

		// 重建全局池（placement new 原地重建）
		// 安全前提：此时无其他线程通过 globalAllocator()/threadLocalAllocator() 分配内存，
		// 且无通过 createRequestPool() 创建的存活 monotonic_buffer_resource。
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
		return std::make_unique<std::pmr::monotonic_buffer_resource>(initialSize, &globalPool_);
	}

	std::pmr::unsynchronized_pool_resource* MemoryPool::getOrCreateThreadPool()
	{
		// 代际感知的 thread_local 缓存：configure() 后自动失效重建
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
			// 检查 GC 是否标记了此池需要释放（延迟释放策略：由拥有线程自行执行 release，
			// 避免跨线程操作 unsynchronized_pool_resource 导致 UB）
			if (cache.entry->needsRelease.exchange(false, std::memory_order_acquire))
			{
				cache.pool->release();
			}
			// 更新活跃时间戳（无锁）
			cache.entry->lastAllocTime.store(std::chrono::steady_clock::now(), std::memory_order_relaxed);
			return cache.pool;
		}

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
					// 标记需要释放，由拥有线程在下次分配时安全执行 release()。
					// 不直接调用 release()，因为 unsynchronized_pool_resource 不是线程安全的。
					entry->needsRelease.store(true, std::memory_order_release);
					++reclaimed;
				}
			}
		}

		gcCycles_.fetch_add(1, std::memory_order_relaxed);
		gcReclaimedPools_.fetch_add(reclaimed, std::memory_order_relaxed);
	}

} // namespace hical
