#pragma once

#include <atomic>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <vector>

namespace hical
{

	/**
 * @brief 池参数配置
 */
	struct PoolConfig
	{
		// 全局同步池参数
		size_t globalMaxBlocksPerChunk = 128;
		size_t globalLargestPoolBlock = 1024 * 1024; // 1MB

		// 线程本地池参数
		size_t threadLocalMaxBlocksPerChunk = 64;
		size_t threadLocalLargestPoolBlock = 512 * 1024; // 512KB

		// 请求级池默认初始大小
		size_t requestPoolInitialSize = 4096;
	};

	/**
 * @brief 追踪型内存资源包装器
 *
 * 包装一个上游 memory_resource，在 allocate/deallocate 时
 * 做原子计数统计，零额外开销（仅原子操作）。
 */
	class TrackedResource : public std::pmr::memory_resource
	{
	public:
		/**
     * @brief 构造追踪型资源
     * @param upstream 上游内存资源
     */
		explicit TrackedResource(std::pmr::memory_resource* upstream) : upstream_(upstream)
		{
		}

		size_t totalAllocations() const
		{
			return totalAllocations_.load(std::memory_order_relaxed);
		}

		size_t totalDeallocations() const
		{
			return totalDeallocations_.load(std::memory_order_relaxed);
		}

		size_t currentBytes() const
		{
			return currentBytes_.load(std::memory_order_relaxed);
		}

		size_t peakBytes() const
		{
			return peakBytes_.load(std::memory_order_relaxed);
		}

		/**
     * @brief 重置统计数据
     */
		void resetStats()
		{
			totalAllocations_.store(0, std::memory_order_relaxed);
			totalDeallocations_.store(0, std::memory_order_relaxed);
			currentBytes_.store(0, std::memory_order_relaxed);
			peakBytes_.store(0, std::memory_order_relaxed);
		}

	protected:
		void* do_allocate(size_t bytes, size_t alignment) override
		{
			void* p = upstream_->allocate(bytes, alignment);
			totalAllocations_.fetch_add(1, std::memory_order_relaxed);
			auto current = currentBytes_.fetch_add(bytes, std::memory_order_relaxed) + bytes;
			// 更新峰值（无锁 CAS）
			auto peak = peakBytes_.load(std::memory_order_relaxed);
			while (current > peak && !peakBytes_.compare_exchange_weak(peak, current, std::memory_order_relaxed))
			{
			}
			return p;
		}

		void do_deallocate(void* p, size_t bytes, size_t alignment) override
		{
			upstream_->deallocate(p, bytes, alignment);
			totalDeallocations_.fetch_add(1, std::memory_order_relaxed);
			currentBytes_.fetch_sub(bytes, std::memory_order_relaxed);
		}

		bool do_is_equal(const memory_resource& other) const noexcept override
		{
			return this == &other;
		}

	private:
		std::pmr::memory_resource* upstream_;
		std::atomic<size_t> totalAllocations_ {0};
		std::atomic<size_t> totalDeallocations_ {0};
		std::atomic<size_t> currentBytes_ {0};
		std::atomic<size_t> peakBytes_ {0};
	};

	/**
 * @brief 全局 pmr 内存池管理器
 *
 * 提供三层内存分配策略：
 * 1. 全局同步池（synchronized_pool_resource）— 跨线程共享，带追踪统计
 * 2. 线程本地池（unsynchronized_pool_resource）— thread_local 无锁访问
 * 3. 请求级单调池（monotonic_buffer_resource）— 请求结束后整体释放
 *
 * @note 线程模型
 * 线程本地池为每个调用 threadLocalAllocator() 的线程创建独立的 unsynchronized_pool_resource。
 * 这些池对象由 MemoryPool 持有所有权（通过 threadPools_ vector），线程退出后池仍在内存中。
 * 对于固定线程池（如 Web 服务器的 IO 线程池），这不是问题。
 * 但在频繁创建/销毁短命线程的场景下，threadPools_ 会单调增长。
 * 如有此类需求，可通过 configure() 重置或在服务器启动前固定线程池大小。
 */
	class MemoryPool
	{
	public:
		/**
     * @brief 获取全局单例
     * @return 内存池管理器引用
     */
		static MemoryPool& instance();

		/**
     * @brief 使用指定配置初始化（必须在首次使用前调用，否则使用默认配置）
     * @param config 池配置
     *
     * @warning 线程安全约束：此方法必须在服务器启动前、单线程环境中调用。
     * 如果在多线程运行期间调用，会导致 use-after-free（globalPool_ 被重建时
     * 其他线程可能正在通过 globalAllocator()/threadLocalAllocator() 分配内存）。
     * 同样，调用时不应有通过 threadLocalAllocator() 分配的存活对象。
     */
		void configure(const PoolConfig& config);

		/**
     * @brief 获取全局同步池（线程安全，跨线程共享）
     * @return pmr 分配器
     */
		std::pmr::polymorphic_allocator<std::byte> globalAllocator();

		/**
     * @brief 获取当前线程的本地池（thread_local 无锁高性能）
     * @return pmr 分配器
     */
		std::pmr::polymorphic_allocator<std::byte> threadLocalAllocator();

		/**
     * @brief 创建请求级单调池（用于 HTTP 请求生命周期）
     * @param initialSize 初始缓冲区大小（字节，0 则使用配置默认值）
     * @return 单调池资源（需手动管理生命周期）
     */
		std::unique_ptr<std::pmr::monotonic_buffer_resource> createRequestPool(size_t initialSize = 0);

		/**
     * @brief 内存池统计信息
     */
		struct Stats
		{
			size_t totalAllocations {0};      // 总分配次数
			size_t totalDeallocations {0};    // 总释放次数
			size_t currentBytesAllocated {0}; // 当前已分配字节数
			size_t peakBytesAllocated {0};    // 峰值字节数
		};

		/**
     * @brief 获取全局池统计信息
     * @return Stats 结构
     */
		Stats getStats() const;

		/**
     * @brief 重置统计数据
     */
		void resetStats();

		/**
     * @brief 获取追踪资源的直接引用（用于高级诊断）
     * @return TrackedResource 引用
     */
		TrackedResource& trackedResource()
		{
			return trackedResource_;
		}

		/**
     * @brief 获取当前配置
     * @return 配置引用
     */
		const PoolConfig& config() const
		{
			return config_;
		}

		// 禁止拷贝和移动
		MemoryPool(const MemoryPool&) = delete;
		MemoryPool& operator=(const MemoryPool&) = delete;

	private:
		MemoryPool();
		~MemoryPool();

		PoolConfig config_;

		// 追踪层（包装 new_delete_resource 作为最终上游）
		TrackedResource trackedResource_;

		// 全局同步池（上游为 trackedResource_）
		std::pmr::synchronized_pool_resource globalPool_;

		// 获取当前线程的本地池（thread_local 缓存 + mutex 注册）
		std::pmr::unsynchronized_pool_resource* getOrCreateThreadPool();

		// MemoryPool 持有所有线程本地池的所有权（避免 thread_local 析构顺序问题）
		mutable std::mutex threadPoolsMutex_;
		std::vector<std::unique_ptr<std::pmr::unsynchronized_pool_resource>> threadPools_;

		// 代际计数器：configure() 时递增，使 thread_local 缓存自动失效
		std::atomic<uint64_t> generation_ {0};
	};

} // namespace hical
