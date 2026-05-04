// 此测试文件以 NDEBUG 编译（target_compile_definitions），
// 验证 HICAL_LOG_TRACE 在 Release 下编译期完全消除。

#include "core/Log.h"
#include <gtest/gtest.h>
#include <sstream>

// 确保 NDEBUG 确实被定义
static_assert(
#ifdef NDEBUG
	true
#else
	false
#endif
	,
	"This test must be compiled with NDEBUG defined");

using namespace hical;

TEST(LogNdebugTest, TraceIsCompiledOut)
{
	std::ostringstream ss;
	Logger::instance().setLevel(LogLevel::hTrace);
	Logger::instance().setOutput(ss);

	// 在 NDEBUG 下，TRACE 宏展开为 ((void)0)，不应产生任何输出
	HICAL_LOG_TRACE("this should not appear");
	HICAL_LOG_TRACE("format={}", 42);

	EXPECT_TRUE(ss.str().empty()) << "TRACE should be compiled out under NDEBUG, but got: " << ss.str();

	// 其他级别应正常工作
	HICAL_LOG_DEBUG("debug works");
	EXPECT_NE(ss.str().find("debug works"), std::string::npos);

	// 恢复
	Logger::instance().setOutput(std::cerr);
	Logger::instance().setLevel(LogLevel::hInfo);
}

TEST(LogNdebugTest, TraceIfIsCompiledOut)
{
	std::ostringstream ss;
	Logger::instance().setLevel(LogLevel::hTrace);
	Logger::instance().setOutput(ss);

	HICAL_LOG_TRACE_IF(true, "trace if should not appear");

	EXPECT_TRUE(ss.str().empty()) << "TRACE_IF should be compiled out under NDEBUG";

	Logger::instance().setOutput(std::cerr);
	Logger::instance().setLevel(LogLevel::hInfo);
}

TEST(LogNdebugTest, TraceStreamIsCompiledOut)
{
	std::ostringstream ss;
	Logger::instance().setLevel(LogLevel::hTrace);
	Logger::instance().setOutput(ss);

	HICAL_LOG_TRACE_STREAM << "trace stream should not appear";

	EXPECT_TRUE(ss.str().empty()) << "TRACE_STREAM should be compiled out under NDEBUG";

	Logger::instance().setOutput(std::cerr);
	Logger::instance().setLevel(LogLevel::hInfo);
}
