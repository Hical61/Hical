#include "MemoryPool.h"

namespace hical
{

MemoryPool::MemoryPool()
    : trackedResource_(std::pmr::new_delete_resource()),
      globalPool_(std::pmr::pool_options{
                      .max_blocks_per_chunk = config_.globalMaxBlocksPerChunk,
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
    // 重建全局池
    globalPool_.~synchronized_pool_resource();
    new (&globalPool_) std::pmr::synchronized_pool_resource(
        std::pmr::pool_options{
            .max_blocks_per_chunk = config_.globalMaxBlocksPerChunk,
            .largest_required_pool_block = config_.globalLargestPoolBlock},
        &trackedResource_);
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

std::unique_ptr<std::pmr::monotonic_buffer_resource>
MemoryPool::createRequestPool(size_t initialSize)
{
    if (initialSize == 0)
    {
        initialSize = config_.requestPoolInitialSize;
    }
    return std::make_unique<std::pmr::monotonic_buffer_resource>(
        initialSize, &globalPool_);
}

std::pmr::unsynchronized_pool_resource* MemoryPool::getOrCreateThreadPool()
{
    // thread_local 裸指针做缓存，避免每次加锁查找
    // MemoryPool 持有 unique_ptr 所有权，保证在 globalPool_ 之前析构
    thread_local std::pmr::unsynchronized_pool_resource* cachedPool = nullptr;

    if (cachedPool != nullptr)
    {
        return cachedPool;
    }

    auto pool = std::make_unique<std::pmr::unsynchronized_pool_resource>(
        std::pmr::pool_options{
            .max_blocks_per_chunk = config_.threadLocalMaxBlocksPerChunk,
            .largest_required_pool_block = config_.threadLocalLargestPoolBlock},
        &globalPool_);

    auto* poolPtr = pool.get();

    {
        std::lock_guard<std::mutex> lock(threadPoolsMutex_);
        threadPools_.push_back(std::move(pool));
    }

    cachedPool = poolPtr;
    return poolPtr;
}

MemoryPool::Stats MemoryPool::getStats() const
{
    return Stats{
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

}  // namespace hical
