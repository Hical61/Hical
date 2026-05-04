#include "core/LogMiddleware.h"
#include <gtest/gtest.h>
#include <string>

using namespace hical;

// ============ generateTraceId 测试 ============

TEST(LogMiddlewareTest, GenerateTraceIdLength)
{
	auto id = generateTraceId();
	EXPECT_EQ(id.size(), 32u);
}

TEST(LogMiddlewareTest, GenerateTraceIdHexChars)
{
	auto id = generateTraceId();
	for (char c : id)
	{
		bool isHex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
		EXPECT_TRUE(isHex) << "Non-hex char: " << c << " in id: " << id;
	}
}

TEST(LogMiddlewareTest, GenerateTraceIdUnique)
{
	auto id1 = generateTraceId();
	auto id2 = generateTraceId();
	EXPECT_NE(id1, id2) << "Two generated IDs should be different";
}

// 注意：makeLogMiddleware 需要协程运行时（io_context + co_await），
// 完整的中间件集成测试需要 HttpServer 环境。
// 这里只测试独立的工具函数。
