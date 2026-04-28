#include "core/MemoryPool.h"
#include "core/PmrBuffer.h"
#include <gtest/gtest.h>
#include <array>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace hical;

// ============ MemoryPool 测试 ============

// 测试单例模式
TEST(MemoryPoolTest, Singleton)
{
	auto& pool1 = MemoryPool::instance();
	auto& pool2 = MemoryPool::instance();
	EXPECT_EQ(&pool1, &pool2);
}

// 测试全局分配器可用
TEST(MemoryPoolTest, GlobalAllocator)
{
	auto allocator = MemoryPool::instance().globalAllocator();
	std::pmr::vector<int> vec(allocator);
	vec.push_back(1);
	vec.push_back(2);
	vec.push_back(3);

	EXPECT_EQ(vec.size(), 3);
	EXPECT_EQ(vec[0], 1);
	EXPECT_EQ(vec[2], 3);
}

// 测试线程本地分配器可用
TEST(MemoryPoolTest, ThreadLocalAllocator)
{
	auto allocator = MemoryPool::instance().threadLocalAllocator();
	std::pmr::vector<char> buffer(allocator);
	buffer.resize(4096);

	EXPECT_EQ(buffer.size(), 4096);
}

// 测试请求级单调池
TEST(MemoryPoolTest, RequestPool)
{
	auto pool = MemoryPool::instance().createRequestPool(1024);
	EXPECT_NE(pool.get(), nullptr);

	std::pmr::polymorphic_allocator<std::byte> allocator(pool.get());
	std::pmr::vector<int> vec(allocator);
	for (int i = 0; i < 100; ++i)
	{
		vec.push_back(i);
	}

	EXPECT_EQ(vec.size(), 100);
	EXPECT_EQ(vec[99], 99);
	// pool 析构时整体释放内存
}

// 测试多线程安全
TEST(MemoryPoolTest, MultiThreadSafety)
{
	const int numThreads = 8;
	const int iterations = 1000;
	std::vector<std::thread> threads;

	for (int i = 0; i < numThreads; ++i)
	{
		threads.emplace_back(
			[iterations]()
			{
				auto allocator = MemoryPool::instance().threadLocalAllocator();
				for (int j = 0; j < iterations; ++j)
				{
					std::pmr::vector<char> buffer(allocator);
					buffer.resize(256);
					buffer[0] = 'x';
				}
			});
	}

	for (auto& t : threads)
	{
		t.join();
	}

	auto stats = MemoryPool::instance().getStats();
	EXPECT_GT(stats.totalAllocations, 0);
}

// 测试统计信息
TEST(MemoryPoolTest, Stats)
{
	MemoryPool::instance().resetStats();

	// 直接通过 trackedResource 分配（绕过 pool 缓存）
	auto& tracked = MemoryPool::instance().trackedResource();
	auto* p = tracked.allocate(256, 8);
	tracked.deallocate(p, 256, 8);

	auto stats = MemoryPool::instance().getStats();
	EXPECT_GE(stats.totalAllocations, 1);
	EXPECT_GE(stats.totalDeallocations, 1);
	EXPECT_GE(stats.peakBytesAllocated, 256);
}

// ============ PmrBuffer 测试 ============

// 测试默认构造
TEST(PmrBufferTest, DefaultConstruction)
{
	PmrBuffer buffer;
	EXPECT_EQ(buffer.readableBytes(), 0);
	EXPECT_GT(buffer.writableBytes(), 0);
}

// 测试使用 pmr 分配器构造
TEST(PmrBufferTest, PmrAllocatorConstruction)
{
	auto allocator = MemoryPool::instance().threadLocalAllocator();
	PmrBuffer buffer(allocator);
	EXPECT_EQ(buffer.readableBytes(), 0);
}

// 测试 append 和 read
TEST(PmrBufferTest, AppendAndRead)
{
	PmrBuffer buffer;

	buffer.append("Hello, ");
	buffer.append("World!");

	EXPECT_EQ(buffer.readableBytes(), 13);

	auto data = buffer.read(7);
	EXPECT_EQ(data, "Hello, ");
	EXPECT_EQ(buffer.readableBytes(), 6);

	auto rest = buffer.readAll();
	EXPECT_EQ(rest, "World!");
	EXPECT_EQ(buffer.readableBytes(), 0);
}

// 测试 peek
TEST(PmrBufferTest, Peek)
{
	PmrBuffer buffer;
	buffer.append("test data");

	EXPECT_EQ(std::string(buffer.peek(), 4), "test");
	// peek 不消费数据
	EXPECT_EQ(buffer.readableBytes(), 9);
}

// 测试 retrieve
TEST(PmrBufferTest, Retrieve)
{
	PmrBuffer buffer;
	buffer.append("Hello, World!");

	buffer.retrieve(7);
	EXPECT_EQ(buffer.readableBytes(), 6);
	EXPECT_EQ(std::string(buffer.peek(), buffer.readableBytes()), "World!");
}

// 测试 retrieveAll
TEST(PmrBufferTest, RetrieveAll)
{
	PmrBuffer buffer;
	buffer.append("test");
	EXPECT_EQ(buffer.readableBytes(), 4);

	buffer.retrieveAll();
	EXPECT_EQ(buffer.readableBytes(), 0);
}

// 测试 findCRLF
TEST(PmrBufferTest, FindCRLF)
{
	PmrBuffer buffer;
	buffer.append("line1\r\nline2");

	auto crlf = buffer.findCRLF();
	EXPECT_NE(crlf, nullptr);
	EXPECT_EQ(crlf - buffer.peek(), 5);
}

// 测试 findCRLF 未找到
TEST(PmrBufferTest, FindCRLFNotFound)
{
	PmrBuffer buffer;
	buffer.append("no crlf here");

	auto crlf = buffer.findCRLF();
	EXPECT_EQ(crlf, nullptr);
}

// 测试 findEOL
TEST(PmrBufferTest, FindEOL)
{
	PmrBuffer buffer;
	buffer.append("line1\nline2");

	auto eol = buffer.findEOL();
	EXPECT_NE(eol, nullptr);
	EXPECT_EQ(eol - buffer.peek(), 5);
}

// 测试确保可写空间
TEST(PmrBufferTest, EnsureWritableBytes)
{
	PmrBuffer buffer({}, 16); // 初始大小 16

	buffer.ensureWritableBytes(1024);
	EXPECT_GE(buffer.writableBytes(), 1024);
}

// 测试大量数据
TEST(PmrBufferTest, LargeData)
{
	PmrBuffer buffer;
	std::string largeData(100000, 'x');
	buffer.append(largeData);

	EXPECT_EQ(buffer.readableBytes(), 100000);
	auto result = buffer.readAll();
	EXPECT_EQ(result, largeData);
}

// 测试 swap
TEST(PmrBufferTest, Swap)
{
	PmrBuffer buf1;
	PmrBuffer buf2;

	buf1.append("aaa");
	buf2.append("bbbbb");

	buf1.swap(buf2);

	EXPECT_EQ(buf1.readableBytes(), 5);
	EXPECT_EQ(buf2.readableBytes(), 3);
}

// 测试 append 另一个 PmrBuffer
TEST(PmrBufferTest, AppendBuffer)
{
	PmrBuffer buf1;
	PmrBuffer buf2;

	buf1.append("Hello, ");
	buf2.append("World!");

	buf1.append(buf2);
	EXPECT_EQ(buf1.readableBytes(), 13);
	EXPECT_EQ(std::string(buf1.peek(), buf1.readableBytes()), "Hello, World!");
}

// 测试空间回收（数据移动到前面）
TEST(PmrBufferTest, SpaceReclaim)
{
	PmrBuffer buffer({}, 32);

	// 先写入再读出，制造 readIndex 前移
	buffer.append("12345678901234567890"); // 20 字节
	buffer.retrieve(18);                   // 消费 18 字节，剩余 2 字节

	// 此时 readIndex 前面有大量空闲空间
	// 追加新数据应该触发数据前移而非扩容
	buffer.append("new data");
	EXPECT_EQ(buffer.readableBytes(), 10);
	EXPECT_EQ(std::string(buffer.peek(), 2), "90");
}

// 测试 hasWritten
TEST(PmrBufferTest, HasWritten)
{
	PmrBuffer buffer;
	buffer.ensureWritableBytes(10);

	// 手动写入数据
	char* writePos = buffer.beginWrite();
	std::memcpy(writePos, "test", 4);
	buffer.hasWritten(4);

	EXPECT_EQ(buffer.readableBytes(), 4);
	EXPECT_EQ(std::string(buffer.peek(), 4), "test");
}

// 测试 swap 不同分配器抛异常
TEST(PmrBufferTest, SwapDifferentAllocatorThrows)
{
	std::array<std::byte, 4096> storage1;
	std::array<std::byte, 4096> storage2;
	std::pmr::monotonic_buffer_resource res1(storage1.data(), storage1.size(), std::pmr::null_memory_resource());
	std::pmr::monotonic_buffer_resource res2(storage2.data(), storage2.size(), std::pmr::null_memory_resource());

	PmrBuffer buf1(std::pmr::polymorphic_allocator<std::byte>(&res1), 64);
	PmrBuffer buf2(std::pmr::polymorphic_allocator<std::byte>(&res2), 64);

	buf1.append("aaa");
	buf2.append("bbb");

	EXPECT_THROW(buf1.swap(buf2), std::logic_error);
}

// 测试 ensureWritableBytes 在不可扩容的分配器上抛 std::bad_alloc
TEST(PmrBufferTest, EnsureWritableBytesThrowsOnAllocationFailure)
{
	std::array<std::byte, 128> storage;
	std::pmr::monotonic_buffer_resource res(storage.data(), storage.size(), std::pmr::null_memory_resource());

	PmrBuffer buffer(std::pmr::polymorphic_allocator<std::byte>(&res), 32);

	// 请求远超可用空间的容量，上游 null_memory_resource 会抛 std::bad_alloc
	EXPECT_THROW(buffer.ensureWritableBytes(65536), std::bad_alloc);
}

// ============ MemoryPool GC 测试 ============

// 测试 getStats 返回扩展字段
TEST(MemoryPoolGcTest, StatsIncludeThreadPoolCount)
{
	auto& pool = MemoryPool::instance();

	// 触发 thread-local 池创建
	auto alloc = pool.threadLocalAllocator();
	(void)alloc;

	auto stats = pool.getStats();
	EXPECT_GE(stats.threadPoolCount, 1);
}

// 测试 gc() 不崩溃，gcCycles 递增
TEST(MemoryPoolGcTest, GcIncrementsCycles)
{
	auto& pool = MemoryPool::instance();

	// 确保有 thread-local 池
	auto alloc = pool.threadLocalAllocator();
	(void)alloc;

	auto beforeStats = pool.getStats();
	pool.gc(std::chrono::seconds(0)); // maxIdle=0 意味着所有池都视为过期

	auto afterStats = pool.getStats();
	EXPECT_GT(afterStats.gcCycles, beforeStats.gcCycles);
}

// 测试 gc() 在多线程场景下不崩溃
TEST(MemoryPoolGcTest, GcThreadSafety)
{
	auto& pool = MemoryPool::instance();

	std::vector<std::thread> threads;
	for (int i = 0; i < 4; ++i)
	{
		threads.emplace_back(
			[&pool]()
			{
				// 创建 thread-local 池并分配一些内存
				auto alloc = pool.threadLocalAllocator();
				std::pmr::vector<int> vec(alloc);
				for (int j = 0; j < 100; ++j)
				{
					vec.push_back(j);
				}
			});
	}

	for (auto& t : threads)
	{
		t.join();
	}

	// 线程已退出，GC 应该能安全回收
	EXPECT_NO_THROW(pool.gc(std::chrono::seconds(0)));

	auto stats = pool.getStats();
	EXPECT_GE(stats.threadPoolCount, 4);
}
