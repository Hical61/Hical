#include "MemoryPool.h"

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
		config_ = config;

		// 先清理所有线程本地池（它们引用 globalPool_ 作为上游）
		{
			std::lock_guard<std::mutex> lock(threadPoolsMutex_);
			threadPools_.clear();
		}

		// 重建全局池
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
			uint64_t generation = 0;
		};

		thread_local ThreadCache cache;

		auto currentGen = generation_.load(std::memory_order_acquire);
		if (cache.pool != nullptr && cache.generation == currentGen)
		{
			return cache.pool;
		}

		auto pool = std::make_unique<std::pmr::unsynchronized_pool_resource>(
			std::pmr::pool_options {.max_blocks_per_chunk = config_.threadLocalMaxBlocksPerChunk,
									.largest_required_pool_block = config_.threadLocalLargestPoolBlock},
			&globalPool_);

		auto* poolPtr = pool.get();

		{
			std::lock_guard<std::mutex> lock(threadPoolsMutex_);
			threadPools_.push_back(std::move(pool));
		}

		cache.pool = poolPtr;
		cache.generation = currentGen;
		return poolPtr;
	}

	MemoryPool::Stats MemoryPool::getStats() const
	{
		return Stats {
			.totalAllocations = trackedResource_.totalAllocations(),
			.totalDeallocations = trackedResource_.totalDeallocations(),
			.currentBytesAllocated = trackedResource_.currentBytes(),
			.peakBytesAllocated = trackedResource_.peakBytes(),
		};
	}

	void MemoryPool::resetStats()
	{
		trackedResource_.resetStats();
	}

} // namespace hical
