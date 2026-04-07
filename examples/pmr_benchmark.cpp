#include "core/MemoryPool.h"
#include "core/PmrBuffer.h"
#include <chrono>
#include <iostream>
#include <memory_resource>
#include <thread>
#include <vector>

using namespace hical;

/**
 * @brief 计时辅助
 */
class Timer
{
public:
	Timer() : start_(std::chrono::high_resolution_clock::now())
	{
	}

	double elapsedMs() const
	{
		auto now = std::chrono::high_resolution_clock::now();
		return std::chrono::duration<double, std::milli>(now - start_).count();
	}

private:
	std::chrono::high_resolution_clock::time_point start_;
};

/**
 * @brief 测试 1：单线程分配/释放性能对比
 */
void benchSingleThread(int iterations, size_t allocSize)
{
	std::cout << "\n=== 单线程分配/释放 (" << iterations << " 次, " << allocSize << " 字节) ===\n";

	// 1. 标准 new/delete
	{
		Timer t;
		for (int i = 0; i < iterations; ++i)
		{
			auto* p = new char[allocSize];
			delete[] p;
		}
		std::cout << "  new/delete:       " << t.elapsedMs() << " ms\n";
	}

	// 2. pmr synchronized_pool
	{
		std::pmr::synchronized_pool_resource pool;
		std::pmr::polymorphic_allocator<char> alloc(&pool);

		Timer t;
		for (int i = 0; i < iterations; ++i)
		{
			auto* p = alloc.allocate(allocSize);
			alloc.deallocate(p, allocSize);
		}
		std::cout << "  pmr sync_pool:    " << t.elapsedMs() << " ms\n";
	}

	// 3. pmr unsynchronized_pool（最快，单线程适用）
	{
		std::pmr::unsynchronized_pool_resource pool;
		std::pmr::polymorphic_allocator<char> alloc(&pool);

		Timer t;
		for (int i = 0; i < iterations; ++i)
		{
			auto* p = alloc.allocate(allocSize);
			alloc.deallocate(p, allocSize);
		}
		std::cout << "  pmr unsync_pool:  " << t.elapsedMs() << " ms\n";
	}

	// 4. pmr monotonic（只分配不释放，批量回收）
	{
		size_t bufSize = allocSize * iterations + 1024;
		if (bufSize > 1024 * 1024 * 512)
		{
			bufSize = 1024 * 1024 * 512;
		}
		std::pmr::monotonic_buffer_resource mono(bufSize);
		std::pmr::polymorphic_allocator<char> alloc(&mono);

		Timer t;
		for (int i = 0; i < iterations; ++i)
		{
			auto* p = alloc.allocate(allocSize);
			(void)p; // monotonic 不需要 deallocate
		}
		std::cout << "  pmr monotonic:    " << t.elapsedMs() << " ms\n";
	}

	// 5. hical MemoryPool (thread_local)
	{
		Timer t;
		auto alloc = MemoryPool::instance().threadLocalAllocator();
		for (int i = 0; i < iterations; ++i)
		{
			auto* p = alloc.allocate_bytes(allocSize);
			alloc.deallocate_bytes(p, allocSize);
		}
		std::cout << "  hical threadLocal:" << t.elapsedMs() << " ms\n";
	}
}

/**
 * @brief 测试 2：多线程并发分配性能
 */
void benchMultiThread(int threadsCount, int iterationsPerThread, size_t allocSize)
{
	std::cout << "\n=== 多线程并发分配 (" << threadsCount << " 线程, " << iterationsPerThread << " 次/线程, "
			  << allocSize << " 字节) ===\n";

	// 1. 标准 new/delete
	{
		Timer t;
		std::vector<std::thread> threads;
		for (int n = 0; n < threadsCount; ++n)
		{
			threads.emplace_back(
				[&]()
				{
					for (int i = 0; i < iterationsPerThread; ++i)
					{
						auto* p = new char[allocSize];
						delete[] p;
					}
				});
		}
		for (auto& th : threads)
		{
			th.join();
		}
		std::cout << "  new/delete:       " << t.elapsedMs() << " ms\n";
	}

	// 2. hical MemoryPool（每线程自动获取 thread_local pool）
	{
		MemoryPool::instance().resetStats();
		Timer t;
		std::vector<std::thread> threads;
		for (int n = 0; n < threadsCount; ++n)
		{
			threads.emplace_back(
				[&]()
				{
					auto alloc = MemoryPool::instance().threadLocalAllocator();
					for (int i = 0; i < iterationsPerThread; ++i)
					{
						auto* p = alloc.allocate_bytes(allocSize);
						alloc.deallocate_bytes(p, allocSize);
					}
				});
		}
		for (auto& th : threads)
		{
			th.join();
		}
		double elapsed = t.elapsedMs();
		auto stats = MemoryPool::instance().getStats();

		std::cout << "  hical threadLocal:" << elapsed << " ms\n";
		std::cout << "    全局池分配次数: " << stats.totalAllocations << "\n";
		std::cout << "    全局池释放次数: " << stats.totalDeallocations << "\n";
		std::cout << "    当前已分配:     " << stats.currentBytesAllocated << " bytes\n";
		std::cout << "    峰值分配:       " << stats.peakBytesAllocated << " bytes\n";
	}
}

/**
 * @brief 测试 3：PmrBuffer 性能
 */
void benchPmrBuffer(int iterations, size_t appendSize)
{
	std::cout << "\n=== PmrBuffer append 性能 (" << iterations << " 次, " << appendSize << " 字节) ===\n";

	std::string data(appendSize, 'A');

	// 1. std::string append
	{
		Timer t;
		std::string buf;
		for (int i = 0; i < iterations; ++i)
		{
			buf.append(data);
			if (buf.size() > 1024 * 1024)
			{
				buf.clear();
			}
		}
		std::cout << "  std::string:      " << t.elapsedMs() << " ms\n";
	}

	// 2. PmrBuffer (默认分配器)
	{
		Timer t;
		PmrBuffer buf;
		for (int i = 0; i < iterations; ++i)
		{
			buf.append(data);
			if (buf.readableBytes() > 1024 * 1024)
			{
				buf.retrieveAll();
			}
		}
		std::cout << "  PmrBuffer(默认):  " << t.elapsedMs() << " ms\n";
	}

	// 3. PmrBuffer (hical pool 分配器)
	{
		auto alloc = MemoryPool::instance().threadLocalAllocator();
		Timer t;
		PmrBuffer buf(alloc);
		for (int i = 0; i < iterations; ++i)
		{
			buf.append(data);
			if (buf.readableBytes() > 1024 * 1024)
			{
				buf.retrieveAll();
			}
		}
		std::cout << "  PmrBuffer(pool):  " << t.elapsedMs() << " ms\n";
	}
}

int main()
{
	std::cout << "========== hical pmr 内存池基准测试 ==========\n";
	std::cout << "硬件并发线程数: " << std::thread::hardware_concurrency() << "\n";

	// 小块分配
	benchSingleThread(1000000, 64);
	benchSingleThread(1000000, 256);
	benchSingleThread(500000, 4096);

	// 多线程
	int threads = static_cast<int>(std::thread::hardware_concurrency());
	if (threads < 2)
	{
		threads = 2;
	}
	benchMultiThread(threads, 500000, 256);

	// PmrBuffer
	benchPmrBuffer(1000000, 128);

	std::cout << "\n========== 测试完成 ==========\n";
	return 0;
}
