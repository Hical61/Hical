#include "core/Log.h"
#include "core/LogFormatter.h"
#include "core/LogRecord.h"
#include <gtest/gtest.h>
#include <boost/json/parse.hpp>
#include <boost/json/value.hpp>
#include <boost/system/error_code.hpp>
#include <string>

using namespace hical;

// 构建一个标准 LogRecord 用于测试
static LogRecord makeTestRecord(LogLevel lvl = LogLevel::hInfo, const char* msg = "test message")
{
	LogRecord r;
	r.level = lvl;
	r.timestamp = std::chrono::system_clock::now();
	r.threadId = 12345;
	r.file = "test_file.cpp";
	r.line = 42;
	r.message = msg;
	return r;
}

// ============ TextFormatter 测试 ============

TEST(TextFormatterTest, BasicFormat)
{
	TextFormatter fmt;
	auto record = makeTestRecord();
	auto result = fmt.format(record);

	EXPECT_NE(result.find("[INFO]"), std::string::npos);
	EXPECT_NE(result.find("[12345]"), std::string::npos);
	EXPECT_NE(result.find("[test_file.cpp:42]"), std::string::npos);
	EXPECT_NE(result.find("test message"), std::string::npos);
	EXPECT_TRUE(result.ends_with("\n"));
}

TEST(TextFormatterTest, ContainsTimestamp)
{
	TextFormatter fmt;
	auto record = makeTestRecord();
	auto result = fmt.format(record);

	// 应包含 [20xx- 格式的时间戳
	EXPECT_NE(result.find("[20"), std::string::npos);
}

TEST(TextFormatterTest, AllLevels)
{
	TextFormatter fmt;
	auto levels =
		{LogLevel::hTrace, LogLevel::hDebug, LogLevel::hInfo, LogLevel::hWarn, LogLevel::hError, LogLevel::hFatal};

	for (auto lvl : levels)
	{
		auto record = makeTestRecord(lvl);
		auto result = fmt.format(record);
		EXPECT_NE(result.find(logLevelToString(lvl)), std::string::npos)
			<< "Level " << logLevelToString(lvl) << " not found in: " << result;
	}
}

TEST(TextFormatterTest, WithTraceId)
{
	TextFormatter fmt;
	auto record = makeTestRecord();
	record.traceId = "abc123def456";
	auto result = fmt.format(record);

	EXPECT_NE(result.find("[abc123def456]"), std::string::npos);
}

TEST(TextFormatterTest, WithoutTraceId)
{
	TextFormatter fmt;
	auto record = makeTestRecord();
	// traceId 为空时不应有额外的 [...] 块
	auto result = fmt.format(record);

	// 计算 [...] 块的数量：应为 timestamp + level + threadId + file:line = 4 个
	size_t count = 0;
	size_t pos = 0;
	while ((pos = result.find('[', pos)) != std::string::npos)
	{
		++count;
		++pos;
	}
	EXPECT_EQ(count, 4u);
}

TEST(TextFormatterTest, ExtractsFilename)
{
	TextFormatter fmt;
	auto record = makeTestRecord();
	record.file = "/long/path/to/file.cpp";
	auto result = fmt.format(record);

	// 应提取 basename
	EXPECT_NE(result.find("[file.cpp:42]"), std::string::npos);
	EXPECT_EQ(result.find("/long/path"), std::string::npos);
}

// ============ JsonFormatter 测试 ============

TEST(JsonFormatterTest, BasicFormat)
{
	JsonFormatter fmt;
	auto record = makeTestRecord();
	auto result = fmt.format(record);

	EXPECT_NE(result.find("\"level\":\"INFO\""), std::string::npos);
	EXPECT_NE(result.find("\"thread_id\":12345"), std::string::npos);
	EXPECT_NE(result.find("\"message\":\"test message\""), std::string::npos);
	EXPECT_NE(result.find("\"file\":\"test_file.cpp\""), std::string::npos);
	EXPECT_NE(result.find("\"line\":42"), std::string::npos);
	EXPECT_TRUE(result.ends_with("\n"));
}

TEST(JsonFormatterTest, ContainsTimestamp)
{
	JsonFormatter fmt;
	auto record = makeTestRecord();
	auto result = fmt.format(record);

	EXPECT_NE(result.find("\"timestamp\":\"20"), std::string::npos);
}

TEST(JsonFormatterTest, WithTraceId)
{
	JsonFormatter fmt;
	auto record = makeTestRecord();
	record.traceId = "trace-abc-123";
	auto result = fmt.format(record);

	EXPECT_NE(result.find("\"trace_id\":\"trace-abc-123\""), std::string::npos);
}

TEST(JsonFormatterTest, WithoutTraceId)
{
	JsonFormatter fmt;
	auto record = makeTestRecord();
	auto result = fmt.format(record);

	// traceId 为空时不应有 trace_id 字段
	EXPECT_EQ(result.find("trace_id"), std::string::npos);
}

TEST(JsonFormatterTest, WithStructuredFields)
{
	JsonFormatter fmt;
	auto record = makeTestRecord();
	record.fields["port"] = 8080;
	record.fields["host"] = "localhost";
	auto result = fmt.format(record);

	EXPECT_NE(result.find("\"port\":8080"), std::string::npos);
	EXPECT_NE(result.find("\"host\":\"localhost\""), std::string::npos);
}

TEST(JsonFormatterTest, ValidJson)
{
	JsonFormatter fmt;
	auto record = makeTestRecord();
	record.fields["key"] = "value";
	auto result = fmt.format(record);

	// 去掉尾部换行后应该是有效 JSON
	auto jsonStr = result.substr(0, result.size() - 1);
	boost::system::error_code ec;
	auto parsed = boost::json::parse(jsonStr, ec);
	EXPECT_FALSE(ec) << "Invalid JSON: " << ec.message() << " in: " << jsonStr;
	EXPECT_TRUE(parsed.is_object());
}
