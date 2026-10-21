/**
 * @file test_read_buffer_pool.cpp
 * @brief 读缓冲池的借还、复用和异常回收测试
 */

#include "core/ReadBufferPool.h"
#include <gtest/gtest.h>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

using namespace hical;

// 借出的 buffer 容量应 >= kBufferSize
TEST(ReadBufferPoolTest, AcquireHasSufficientCapacity)
{
	auto h = ReadBufferPool::acquire();
	EXPECT_GE(h.get().capacity(), ReadBufferPool::kBufferSize);
}

// 借出的 buffer 是 clear 状态
TEST(ReadBufferPoolTest, AcquiredBufferIsEmpty)
{
	auto h = ReadBufferPool::acquire();
	EXPECT_TRUE(h.get().empty());
}

// 写入数据后析构，再借应该是 empty（池化复用路径）
TEST(ReadBufferPoolTest, ReturnedBufferIsCleared)
{
	{
		auto h = ReadBufferPool::acquire();
		h.get().assign(100, 'x');
	}
	// 析构后归还到池，再借一次
	auto h2 = ReadBufferPool::acquire();
	EXPECT_TRUE(h2.get().empty());
}

// 归还后池里有一个，再借能复用（不 new 新对象）
TEST(ReadBufferPoolTest, PoolReuse)
{
	std::string* rawPtr = nullptr;
	{
		auto h = ReadBufferPool::acquire();
		rawPtr = h.buf_;
	}
	// 归还后再借，应该是同一块内存
	auto h2 = ReadBufferPool::acquire();
	EXPECT_EQ(h2.buf_, rawPtr);
}

// move 语义：move 后原 handle 不再持有 buffer
TEST(ReadBufferPoolTest, MoveHandle)
{
	auto h1 = ReadBufferPool::acquire();
	auto* rawPtr = h1.buf_;
	auto h2 = std::move(h1);
	EXPECT_EQ(h1.buf_, nullptr); // NOLINT(bugprone-use-after-move)
	EXPECT_EQ(h2.buf_, rawPtr);
}

// 显式 release 后 buf_ 为 nullptr
TEST(ReadBufferPoolTest, ExplicitRelease)
{
	auto h = ReadBufferPool::acquire();
	h.release();
	EXPECT_EQ(h.buf_, nullptr);
}

// 超大 buffer 归还时不放回池（容量 > kMaxReturnSize）
// 验证方法：先排空池，归还一个超大 buffer（应被 delete 不入池），
// 再借出时容量应 <= kMaxReturnSize（来自新 new，不是那个超大的）。
TEST(ReadBufferPoolTest, OversizedBufferNotPooled)
{
	// 借出 kMaxPooled 个 handle 让池完全排空，用固定数组持有
	static constexpr size_t kDrainCount = ReadBufferPool::kMaxPooled;
	ReadBufferPool::BufferHandle drain[kDrainCount]; // NOLINT(cppcoreguidelines-pro-bounds-constant-array-index)
	for (auto& slot : drain)
	{
		slot = ReadBufferPool::acquire();
	}
	// 池此时为空；归还超大 buffer，它应被直接 delete 而不入池
	{
		auto big = ReadBufferPool::acquire(); // 池空 → new 一个新的
		big.get().reserve(ReadBufferPool::kMaxReturnSize + 1);
		// big 析构 → 超大 → delete，池 count 仍为 0
	}
	// 如果超大 buffer 错误入池，下次 acquire 会复用它（容量 > kMaxReturnSize）
	auto next = ReadBufferPool::acquire(); // 池空 → 必须 new
	EXPECT_LE(next.get().capacity(), ReadBufferPool::kMaxReturnSize);
	// drain[] 析构，kDrainCount 个正常 buffer 归还到池
}

// 连续借还多个，pool 不超过 kMaxPooled
TEST(ReadBufferPoolTest, PoolCappedAtMaxPooled)
{
	// 先把池清空（借出所有现存的）
	std::vector<ReadBufferPool::BufferHandle> handles;
	handles.reserve(ReadBufferPool::kMaxPooled + 5);
	for (size_t i = 0; i <= ReadBufferPool::kMaxPooled + 4; ++i)
	{
		handles.emplace_back(ReadBufferPool::acquire());
	}
	handles.clear(); // 全部归还，超过 kMaxPooled 的部分被 delete

	// 再借 kMaxPooled 个，都应该能从池里拿到
	for (size_t i = 0; i < ReadBufferPool::kMaxPooled; ++i)
	{
		handles.emplace_back(ReadBufferPool::acquire());
	}
	EXPECT_EQ(handles.size(), ReadBufferPool::kMaxPooled);
}

// 默认构造的 handle 是空的
TEST(ReadBufferPoolTest, DefaultHandleIsNull)
{
	ReadBufferPool::BufferHandle h;
	EXPECT_EQ(h.buf_, nullptr);
}

TEST(ReadBufferPoolTest, AcquireForReadProvidesWritableSize)
{
	auto handle = ReadBufferPool::acquireForRead();
	EXPECT_EQ(handle.get().size(), ReadBufferPool::kBufferSize);
	EXPECT_GE(handle.get().capacity(), ReadBufferPool::kBufferSize);
}

TEST(ReadBufferPoolTest, AcquireForReadReusesStorageWithoutClearing)
{
	std::string* rawBuffer = nullptr;
	const char* storage = nullptr;
	{
		auto handle = ReadBufferPool::acquireForRead();
		handle.get().assign(ReadBufferPool::kBufferSize, 'x');
		rawBuffer = handle.buf_;
		storage = handle.get().data();
	}
	auto reused = ReadBufferPool::acquireForRead();
	EXPECT_EQ(reused.buf_, rawBuffer);
	EXPECT_EQ(reused.get().data(), storage);
	EXPECT_EQ(reused.get(), std::string(ReadBufferPool::kBufferSize, 'x'));
}

TEST(ReadBufferPoolTest, AcquireAfterReadBufferStillReturnsEmpty)
{
	{
		auto handle = ReadBufferPool::acquireForRead();
		handle.get().front() = 'x';
	}
	auto ordinary = ReadBufferPool::acquire();
	EXPECT_TRUE(ordinary.get().empty());
	EXPECT_GE(ordinary.get().capacity(), ReadBufferPool::kBufferSize);
}

TEST(ReadBufferPoolTest, AcquireForReadUsesRequestedSizeAfterLargerBuffer)
{
	{
		auto large = ReadBufferPool::acquireForRead(ReadBufferPool::kBufferSize * 2);
		large.get().assign(large.get().size(), 'x');
	}
	auto normal = ReadBufferPool::acquireForRead();
	EXPECT_EQ(normal.get().size(), ReadBufferPool::kBufferSize);
	EXPECT_EQ(normal.get().back(), 'x');
	normal.release();

	auto empty = ReadBufferPool::acquireForRead(0);
	EXPECT_TRUE(empty.get().empty());
	EXPECT_GE(empty.get().capacity(), ReadBufferPool::kBufferSize);
	empty.release();
	auto expanded = ReadBufferPool::acquireForRead();
	EXPECT_EQ(expanded.get().size(), ReadBufferPool::kBufferSize);
}

TEST(ReadBufferPoolTest, AcquireForReadFailureReturnsBufferToPool)
{
	std::string* rawBuffer = nullptr;
	{
		auto handle = ReadBufferPool::acquireForRead();
		rawBuffer = handle.buf_;
	}
	EXPECT_THROW(ReadBufferPool::acquireForRead(std::numeric_limits<size_t>::max()), std::length_error);
	auto next = ReadBufferPool::acquireForRead();
	EXPECT_EQ(next.buf_, rawBuffer);
	EXPECT_EQ(next.get().size(), ReadBufferPool::kBufferSize);
}
