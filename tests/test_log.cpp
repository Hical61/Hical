#include "core/Log.h"
#include "core/LogSink.h"
#include <gtest/gtest.h>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

using namespace hical;

// ============ 测试夹具：每个用例前重置 Logger 状态 ============

class LogTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		// 重置为默认状态：级别 hInfo，输出到 devNull，flush 阈值 hError
		Logger::instance().setLevel(LogLevel::hInfo);
		Logger::instance().setFlushLevel(LogLevel::hError);
		Logger::instance().setOutput(devNull_);
	}

	void TearDown() override
	{
		// 恢复为 stderr，避免 fixture 析构后 Logger 持有悬空指针
		Logger::instance().setOutput(std::cerr);
		Logger::instance().setLevel(LogLevel::hInfo);
		Logger::instance().setFlushLevel(LogLevel::hError);
	}

	std::ostringstream ss_;
	// 用 /dev/null 等效替代 stderr，避免测试输出噪音
	std::ofstream devNull_;
};

// ============ 基础属性测试 ============

TEST_F(LogTest, DefaultLevelIsInfo)
{
	Logger::instance().setLevel(LogLevel::hInfo);
	EXPECT_EQ(Logger::instance().level(), LogLevel::hInfo);
}

TEST_F(LogTest, SetLevel)
{
	Logger::instance().setLevel(LogLevel::hDebug);
	EXPECT_EQ(Logger::instance().level(), LogLevel::hDebug);

	Logger::instance().setLevel(LogLevel::hError);
	EXPECT_EQ(Logger::instance().level(), LogLevel::hError);
}

TEST_F(LogTest, SetLevelTrace)
{
	Logger::instance().setLevel(LogLevel::hTrace);
	EXPECT_EQ(Logger::instance().level(), LogLevel::hTrace);
}

TEST_F(LogTest, SetLevelFatal)
{
	Logger::instance().setLevel(LogLevel::hFatal);
	EXPECT_EQ(Logger::instance().level(), LogLevel::hFatal);
}

// ============ flush 级别测试 ============

TEST_F(LogTest, DefaultFlushLevelIsError)
{
	EXPECT_EQ(Logger::instance().flushLevel(), LogLevel::hError);
}

TEST_F(LogTest, SetFlushLevel)
{
	Logger::instance().setFlushLevel(LogLevel::hWarn);
	EXPECT_EQ(Logger::instance().flushLevel(), LogLevel::hWarn);

	Logger::instance().setFlushLevel(LogLevel::hFatal);
	EXPECT_EQ(Logger::instance().flushLevel(), LogLevel::hFatal);
}

// ============ 级别过滤测试 ============

TEST_F(LogTest, LevelFilterDebug)
{
	Logger::instance().setLevel(LogLevel::hWarn);
	Logger::instance().setOutput(ss_);

	// hWarn 级别下，DEBUG 和 INFO 不应输出
	HICAL_LOG_DEBUG("should be filtered");
	HICAL_LOG_INFO("should also be filtered");
	HICAL_LOG_WARN("should appear");

	std::string out = ss_.str();
	EXPECT_EQ(out.find("should be filtered"), std::string::npos);
	EXPECT_EQ(out.find("should also be filtered"), std::string::npos);
	EXPECT_NE(out.find("should appear"), std::string::npos);
}

TEST_F(LogTest, TraceLevelFilter)
{
	Logger::instance().setLevel(LogLevel::hDebug);
	Logger::instance().setOutput(ss_);

	// hDebug 级别下，TRACE 不应输出
	HICAL_LOG_TRACE("trace should be filtered");
	HICAL_LOG_DEBUG("debug should appear");

	std::string out = ss_.str();
#ifndef NDEBUG
	// Debug 构建：TRACE 宏有效但被级别过滤
	EXPECT_EQ(out.find("trace should be filtered"), std::string::npos);
#endif
	EXPECT_NE(out.find("debug should appear"), std::string::npos);
}

TEST_F(LogTest, TraceAppearsAtTraceLevel)
{
	Logger::instance().setLevel(LogLevel::hTrace);
	Logger::instance().setOutput(ss_);

	HICAL_LOG_TRACE("trace visible");

	std::string out = ss_.str();
#ifndef NDEBUG
	EXPECT_NE(out.find("trace visible"), std::string::npos);
	EXPECT_NE(out.find("[TRACE]"), std::string::npos);
#else
	// Release 构建：TRACE 宏编译期消除，不应有输出
	EXPECT_TRUE(out.empty());
#endif
}

// ============ 输出格式测试 ============

TEST_F(LogTest, OutputToStream)
{
	Logger::instance().setLevel(LogLevel::hDebug);
	Logger::instance().setOutput(ss_);

	HICAL_LOG_INFO("hello world");

	std::string out = ss_.str();
	EXPECT_NE(out.find("[INFO]"), std::string::npos);
	// 文件名部分（宏展开后文件名为 test_log.cpp）
	EXPECT_NE(out.find("test_log.cpp:"), std::string::npos);
	EXPECT_NE(out.find("hello world"), std::string::npos);
}

TEST_F(LogTest, OutputContainsTimestamp)
{
	Logger::instance().setLevel(LogLevel::hDebug);
	Logger::instance().setOutput(ss_);

	HICAL_LOG_INFO("ts check");

	std::string out = ss_.str();
	// 时间戳以 [20 开头（21 世纪年份）
	EXPECT_NE(out.find("[20"), std::string::npos);
}

TEST_F(LogTest, OutputContainsThreadId)
{
	Logger::instance().setLevel(LogLevel::hDebug);
	Logger::instance().setOutput(ss_);

	HICAL_LOG_INFO("tid check");

	std::string out = ss_.str();
	// 格式：[timestamp] [LEVEL] [threadID] [file:line] message
	// 应该有 3 个 ] [ 分隔块在 LEVEL 和 file 之间
	// 验证线程ID字段存在：在 [INFO] 之后、[test_log.cpp 之前应有一个 [...] 块
	auto infoPos = out.find("[INFO]");
	auto filePos = out.find("[test_log.cpp:");
	ASSERT_NE(infoPos, std::string::npos);
	ASSERT_NE(filePos, std::string::npos);
	// [INFO] 和 [test_log.cpp: 之间应该有 [threadID]
	auto between = out.substr(infoPos + 6, filePos - infoPos - 6);
	// 应包含 [...]
	EXPECT_NE(between.find('['), std::string::npos);
	EXPECT_NE(between.find(']'), std::string::npos);
}

// ============ 所有级别输出测试 ============

TEST_F(LogTest, AllLevels)
{
	Logger::instance().setLevel(LogLevel::hTrace);
	Logger::instance().setOutput(ss_);

	HICAL_LOG_TRACE("msg trace");
	HICAL_LOG_DEBUG("msg debug");
	HICAL_LOG_INFO("msg info");
	HICAL_LOG_WARN("msg warn");
	HICAL_LOG_ERROR("msg error");

	std::string out = ss_.str();
#ifndef NDEBUG
	EXPECT_NE(out.find("TRACE"), std::string::npos);
#endif
	EXPECT_NE(out.find("DEBUG"), std::string::npos);
	EXPECT_NE(out.find("INFO"), std::string::npos);
	EXPECT_NE(out.find("WARN"), std::string::npos);
	EXPECT_NE(out.find("ERROR"), std::string::npos);
}

// ============ std::format 风格测试 ============

TEST_F(LogTest, FormatStyleBasic)
{
	Logger::instance().setLevel(LogLevel::hDebug);
	Logger::instance().setOutput(ss_);

	HICAL_LOG_INFO("port={}", 8080);

	std::string out = ss_.str();
	EXPECT_NE(out.find("port=8080"), std::string::npos);
}

TEST_F(LogTest, FormatStyleMultipleArgs)
{
	Logger::instance().setLevel(LogLevel::hDebug);
	Logger::instance().setOutput(ss_);

	HICAL_LOG_INFO("host={} port={}", "localhost", 8080);

	std::string out = ss_.str();
	EXPECT_NE(out.find("host=localhost port=8080"), std::string::npos);
}

TEST_F(LogTest, FormatStyleNoArgs)
{
	Logger::instance().setLevel(LogLevel::hDebug);
	Logger::instance().setOutput(ss_);

	// 无额外参数：std::format("hello") 直接返回 "hello"
	HICAL_LOG_INFO("hello");

	std::string out = ss_.str();
	EXPECT_NE(out.find("hello"), std::string::npos);
}

TEST_F(LogTest, FormatStyleWithTypes)
{
	Logger::instance().setLevel(LogLevel::hDebug);
	Logger::instance().setOutput(ss_);

	HICAL_LOG_DEBUG("int={} double={:.2f} bool={}", 42, 3.14, true);

	std::string out = ss_.str();
	EXPECT_NE(out.find("int=42"), std::string::npos);
	EXPECT_NE(out.find("double=3.14"), std::string::npos);
	EXPECT_NE(out.find("bool=true"), std::string::npos);
}

// ============ 条件日志宏测试 ============

TEST_F(LogTest, ConditionalMacroTrue)
{
	Logger::instance().setLevel(LogLevel::hDebug);
	Logger::instance().setOutput(ss_);

	HICAL_LOG_INFO_IF(true, "condition met");

	std::string out = ss_.str();
	EXPECT_NE(out.find("condition met"), std::string::npos);
}

TEST_F(LogTest, ConditionalMacroFalse)
{
	Logger::instance().setLevel(LogLevel::hDebug);
	Logger::instance().setOutput(ss_);

	HICAL_LOG_INFO_IF(false, "should not appear");

	std::string out = ss_.str();
	EXPECT_EQ(out.find("should not appear"), std::string::npos);
}

TEST_F(LogTest, ConditionalMacroWithFormat)
{
	Logger::instance().setLevel(LogLevel::hDebug);
	Logger::instance().setOutput(ss_);

	int x = 100;
	HICAL_LOG_WARN_IF(x > 50, "x={} exceeded threshold", x);

	std::string out = ss_.str();
	EXPECT_NE(out.find("x=100 exceeded threshold"), std::string::npos);
}

// ============ 流式日志宏测试 ============

TEST_F(LogTest, StreamMacroBasic)
{
	Logger::instance().setLevel(LogLevel::hDebug);
	Logger::instance().setOutput(ss_);

	HICAL_LOG_INFO_STREAM << "port=" << 8080;

	std::string out = ss_.str();
	EXPECT_NE(out.find("port=8080"), std::string::npos);
	EXPECT_NE(out.find("[INFO]"), std::string::npos);
}

TEST_F(LogTest, StreamMacroFiltered)
{
	Logger::instance().setLevel(LogLevel::hWarn);
	Logger::instance().setOutput(ss_);

	HICAL_LOG_DEBUG_STREAM << "filtered out";

	std::string out = ss_.str();
	EXPECT_TRUE(out.empty());
}

TEST_F(LogTest, StreamMacroMultipleValues)
{
	Logger::instance().setLevel(LogLevel::hDebug);
	Logger::instance().setOutput(ss_);

	HICAL_LOG_ERROR_STREAM << "code=" << 500 << " msg=" << "server error";

	std::string out = ss_.str();
	EXPECT_NE(out.find("code=500 msg=server error"), std::string::npos);
	EXPECT_NE(out.find("[ERROR]"), std::string::npos);
}

// ============ flush 行为测试 ============

TEST_F(LogTest, NoFlushBelowThreshold)
{
	// 默认 flushLevel = hError
	// Info/Warn 不应触发 flush（通过 stringstream 的 rdbuf 间接验证）
	Logger::instance().setLevel(LogLevel::hDebug);
	Logger::instance().setOutput(ss_);

	HICAL_LOG_INFO("no flush");
	HICAL_LOG_WARN("no flush either");

	// 无法直接验证 flush 行为，但至少消息应该在缓冲区
	std::string out = ss_.str();
	EXPECT_NE(out.find("no flush"), std::string::npos);
}

TEST_F(LogTest, FlushLevelCustom)
{
	Logger::instance().setFlushLevel(LogLevel::hWarn);
	EXPECT_EQ(Logger::instance().flushLevel(), LogLevel::hWarn);

	Logger::instance().setFlushLevel(LogLevel::hInfo);
	EXPECT_EQ(Logger::instance().flushLevel(), LogLevel::hInfo);
}

// ============ Fatal 测试 ============

TEST_F(LogTest, FatalAbortsProcess)
{
	// Fatal 级别应触发 abort()
	EXPECT_DEATH(
		{
			Logger::instance().setLevel(LogLevel::hTrace);
			Logger::instance().setOutput(std::cerr);
			HICAL_LOG_FATAL("fatal error");
		},
		"");
}

TEST_F(LogTest, FatalStreamAbortsProcess)
{
	EXPECT_DEATH(
		{
			Logger::instance().setLevel(LogLevel::hTrace);
			Logger::instance().setOutput(std::cerr);
			HICAL_LOG_FATAL_STREAM << "fatal stream";
		},
		"");
}

// ============ logLevelToString 测试 ============

TEST_F(LogTest, LogLevelToStringAllLevels)
{
	EXPECT_STREQ(logLevelToString(LogLevel::hTrace), "TRACE");
	EXPECT_STREQ(logLevelToString(LogLevel::hDebug), "DEBUG");
	EXPECT_STREQ(logLevelToString(LogLevel::hInfo), "INFO");
	EXPECT_STREQ(logLevelToString(LogLevel::hWarn), "WARN");
	EXPECT_STREQ(logLevelToString(LogLevel::hError), "ERROR");
	EXPECT_STREQ(logLevelToString(LogLevel::hFatal), "FATAL");
}

// ============ 文件输出测试 ============

TEST_F(LogTest, FileOutput)
{
	auto tmpPath = std::filesystem::temp_directory_path() / "hical_test_log.txt";
	// 确保测试前文件不存在（清理旧数据）
	std::filesystem::remove(tmpPath);

	Logger::instance().setLevel(LogLevel::hDebug);
	Logger::instance().setOutput(tmpPath.string());

	HICAL_LOG_INFO("file output test");

	// 强制 flush：写一条 Error 触发 flush
	HICAL_LOG_ERROR("flush trigger");

	// 读回文件内容验证
	std::ifstream ifs(tmpPath);
	ASSERT_TRUE(ifs.is_open());
	std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());

	EXPECT_NE(content.find("[INFO]"), std::string::npos);
	EXPECT_NE(content.find("file output test"), std::string::npos);
	ifs.close();

	// 恢复 Logger 输出以释放文件句柄（Windows 要求关闭后才能删除）
	Logger::instance().setOutput(devNull_);
	std::filesystem::remove(tmpPath);
}

// ============ 多线程安全测试 ============

TEST_F(LogTest, MultiThreadSafety)
{
	Logger::instance().setLevel(LogLevel::hDebug);
	Logger::instance().setOutput(ss_);

	constexpr int kThreads = 4;
	constexpr int kLogsPerThread = 100;

	std::vector<std::thread> threads;
	threads.reserve(kThreads);

	for (int t = 0; t < kThreads; ++t)
	{
		threads.emplace_back(
			[t]()
			{
				for (int i = 0; i < kLogsPerThread; ++i)
				{
					HICAL_LOG_INFO("thread={} iter={}", t, i);
				}
			});
	}

	for (auto& th : threads)
	{
		th.join();
	}

	std::string out = ss_.str();
	// 验证所有线程的日志都有输出（至少找到每个线程的第一条）
	for (int t = 0; t < kThreads; ++t)
	{
		auto needle = std::format("thread={} iter=0", t);
		EXPECT_NE(out.find(needle), std::string::npos) << "Missing logs from thread " << t;
	}
}

// ============ Sink API 测试 ============

// 简单的内存 Sink，收集所有写入的内容
class MemorySink : public LogSink
{
public:
	void write(std::string_view formattedLine) override
	{
		std::lock_guard<std::mutex> lock(mutex_);
		data_.append(formattedLine.data(), formattedLine.size());
	}

	void flush() override
	{
	}

	std::string data()
	{
		std::lock_guard<std::mutex> lock(mutex_);
		return data_;
	}

private:
	std::mutex mutex_;
	std::string data_;
};

TEST_F(LogTest, AddSinkReceivesLogs)
{
	auto sink = std::make_shared<MemorySink>();
	Logger::instance().setLevel(LogLevel::hDebug);
	Logger::instance().clearSinks();
	Logger::instance().addSink(sink);

	HICAL_LOG_INFO("sink test");

	auto data = sink->data();
	EXPECT_NE(data.find("sink test"), std::string::npos);
	EXPECT_NE(data.find("[INFO]"), std::string::npos);
}

TEST_F(LogTest, MultipleSinksReceiveLogs)
{
	auto sink1 = std::make_shared<MemorySink>();
	auto sink2 = std::make_shared<MemorySink>();
	Logger::instance().setLevel(LogLevel::hDebug);
	Logger::instance().clearSinks();
	Logger::instance().addSink(sink1);
	Logger::instance().addSink(sink2);

	HICAL_LOG_WARN("dual sink");

	EXPECT_NE(sink1->data().find("dual sink"), std::string::npos);
	EXPECT_NE(sink2->data().find("dual sink"), std::string::npos);
}

TEST_F(LogTest, ClearSinksStopsOutput)
{
	auto sink = std::make_shared<MemorySink>();
	Logger::instance().setLevel(LogLevel::hDebug);
	Logger::instance().clearSinks();
	Logger::instance().addSink(sink);

	HICAL_LOG_INFO("before clear");
	Logger::instance().clearSinks();
	HICAL_LOG_INFO("after clear");

	auto data = sink->data();
	EXPECT_NE(data.find("before clear"), std::string::npos);
	EXPECT_EQ(data.find("after clear"), std::string::npos);
}

TEST_F(LogTest, SetSinkReplacesExisting)
{
	auto sink1 = std::make_shared<MemorySink>();
	auto sink2 = std::make_shared<MemorySink>();
	Logger::instance().setLevel(LogLevel::hDebug);
	Logger::instance().setSink(sink1);

	HICAL_LOG_INFO("to sink1");

	Logger::instance().setSink(sink2);

	HICAL_LOG_INFO("to sink2");

	EXPECT_NE(sink1->data().find("to sink1"), std::string::npos);
	EXPECT_EQ(sink1->data().find("to sink2"), std::string::npos);
	EXPECT_NE(sink2->data().find("to sink2"), std::string::npos);
}

TEST_F(LogTest, SinkLevelFilter)
{
	auto sink = std::make_shared<MemorySink>();
	sink->setLevel(LogLevel::hError); // Sink 只接受 Error+
	Logger::instance().setLevel(LogLevel::hDebug);
	Logger::instance().clearSinks();
	Logger::instance().addSink(sink);

	HICAL_LOG_INFO("info should be filtered by sink");
	HICAL_LOG_ERROR("error should pass");

	auto data = sink->data();
	EXPECT_EQ(data.find("info should be filtered"), std::string::npos);
	EXPECT_NE(data.find("error should pass"), std::string::npos);
}

TEST_F(LogTest, SetOutputOstreamCompatibility)
{
	// setOutput(ostream&) 应该通过内部 OStreamSink 桥接正常工作
	Logger::instance().setLevel(LogLevel::hDebug);
	Logger::instance().setOutput(ss_);

	HICAL_LOG_INFO("ostream compat");

	std::string out = ss_.str();
	EXPECT_NE(out.find("ostream compat"), std::string::npos);
}
