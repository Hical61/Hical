#include "core/ReadBufferPool.h"
#include <gtest/gtest.h>
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
TEST(ReadBufferPoolTest, OversizedBufferNotPooled)
{
	std::string* rawPtr = nullptr;
	{
		auto h = ReadBufferPool::acquire();
		h.get().reserve(ReadBufferPool::kMaxReturnSize + 1);
		rawPtr = h.buf_;
	}
	// 超大 buffer 被 delete，再借应该是新对象
	auto h2 = ReadBufferPool::acquire();
	EXPECT_NE(h2.buf_, rawPtr);
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
