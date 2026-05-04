#include "core/FixedBuffer.h"
#include <gtest/gtest.h>
#include <string>

using namespace hical;

TEST(FixedBufferTest, EmptyBuffer)
{
	FixedBuffer<64> buf;
	EXPECT_EQ(buf.size(), 0u);
	EXPECT_TRUE(buf.view().empty());
	EXPECT_FALSE(buf.overflowed());
}

TEST(FixedBufferTest, AppendString)
{
	FixedBuffer<64> buf;
	buf.append("hello", 5);
	EXPECT_EQ(buf.view(), "hello");
	EXPECT_EQ(buf.size(), 5u);
	EXPECT_FALSE(buf.overflowed());
}

TEST(FixedBufferTest, AppendStringView)
{
	FixedBuffer<64> buf;
	buf.append(std::string_view("world"));
	EXPECT_EQ(buf.view(), "world");
}

TEST(FixedBufferTest, StreamOperatorStringView)
{
	FixedBuffer<64> buf;
	buf << std::string_view("hello") << std::string_view(" ") << std::string_view("world");
	EXPECT_EQ(buf.view(), "hello world");
}

TEST(FixedBufferTest, StreamOperatorChar)
{
	FixedBuffer<64> buf;
	buf << 'A' << 'B' << 'C';
	EXPECT_EQ(buf.view(), "ABC");
}

TEST(FixedBufferTest, StreamOperatorCString)
{
	FixedBuffer<64> buf;
	buf << "hello";
	EXPECT_EQ(buf.view(), "hello");
}

TEST(FixedBufferTest, StreamOperatorNullCString)
{
	FixedBuffer<64> buf;
	buf << static_cast<const char*>(nullptr);
	EXPECT_EQ(buf.size(), 0u);
}

TEST(FixedBufferTest, StreamOperatorInt)
{
	FixedBuffer<64> buf;
	buf << 42;
	EXPECT_EQ(buf.view(), "42");
}

TEST(FixedBufferTest, StreamOperatorNegativeInt)
{
	FixedBuffer<64> buf;
	buf << -100;
	EXPECT_EQ(buf.view(), "-100");
}

TEST(FixedBufferTest, StreamOperatorUnsigned)
{
	FixedBuffer<64> buf;
	buf << 4294967295u;
	EXPECT_EQ(buf.view(), "4294967295");
}

TEST(FixedBufferTest, StreamOperatorLongLong)
{
	FixedBuffer<64> buf;
	buf << 1234567890123LL;
	EXPECT_EQ(buf.view(), "1234567890123");
}

TEST(FixedBufferTest, StreamOperatorDouble)
{
	FixedBuffer<64> buf;
	buf << 3.14;
	auto sv = buf.view();
	// std::to_chars 的 general 格式可能输出 "3.14" 或 "3.1400000000000001"
	EXPECT_TRUE(sv.starts_with("3.14")) << "Got: " << std::string(sv);
}

TEST(FixedBufferTest, StreamOperatorBool)
{
	FixedBuffer<64> buf;
	buf << true << std::string_view(" ") << false;
	EXPECT_EQ(buf.view(), "true false");
}

TEST(FixedBufferTest, MixedTypes)
{
	FixedBuffer<256> buf;
	buf << std::string_view("port=") << 8080 << std::string_view(" active=") << true;
	EXPECT_EQ(buf.view(), "port=8080 active=true");
}

TEST(FixedBufferTest, OverflowToHeap)
{
	FixedBuffer<16> buf; // 很小的缓冲区
	buf << std::string_view("this is a long string that will overflow");
	EXPECT_TRUE(buf.overflowed());
	EXPECT_EQ(buf.view(), "this is a long string that will overflow");
}

TEST(FixedBufferTest, OverflowAppendContinues)
{
	FixedBuffer<8> buf;
	buf << std::string_view("12345678"); // 刚好满
	EXPECT_FALSE(buf.overflowed());
	buf << std::string_view("9"); // 触发溢出
	EXPECT_TRUE(buf.overflowed());
	EXPECT_EQ(buf.view(), "123456789");
	buf << std::string_view("0");
	EXPECT_EQ(buf.view(), "1234567890");
}

TEST(FixedBufferTest, Clear)
{
	FixedBuffer<64> buf;
	buf << std::string_view("hello");
	buf.clear();
	EXPECT_EQ(buf.size(), 0u);
	EXPECT_FALSE(buf.overflowed());
	EXPECT_TRUE(buf.view().empty());
}

TEST(FixedBufferTest, Capacity)
{
	FixedBuffer<128> buf;
	EXPECT_EQ(buf.capacity(), 128u);
}

TEST(FixedBufferTest, DataPointer)
{
	FixedBuffer<64> buf;
	buf << std::string_view("test");
	EXPECT_NE(buf.data(), nullptr);
	EXPECT_EQ(std::string_view(buf.data(), buf.size()), "test");
}
