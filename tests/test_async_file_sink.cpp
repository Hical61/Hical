#include "core/AsyncFileSink.h"
#include "core/LogSink.h"
#include <gtest/gtest.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

using namespace hical;

namespace fs = std::filesystem;

class AsyncFileSinkTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		m_testDir = fs::temp_directory_path()
					/ ("hical_async_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
		fs::create_directories(m_testDir);
	}

	void TearDown() override
	{
		std::error_code ec;
		fs::remove_all(m_testDir, ec);
	}

	std::string readFile(const fs::path& path)
	{
		std::ifstream ifs(path);
		return {std::istreambuf_iterator<char>(ifs), std::istreambuf_iterator<char>()};
	}

	fs::path m_testDir;
};

TEST_F(AsyncFileSinkTest, BasicWrite)
{
	auto logPath = (m_testDir / "async.log").string();
	{
		AsyncFileSink sink(
			AsyncFileSink::Options {.file = {.basePath = logPath}, .flushInterval = std::chrono::milliseconds(100)});

		sink.write("[INFO] hello async\n");
		sink.flush();
		// 给后台线程一点时间处理
		std::this_thread::sleep_for(std::chrono::milliseconds(300));
	} // 析构时 jthread stop+join，确保 flush

	auto content = readFile(logPath);
	EXPECT_NE(content.find("hello async"), std::string::npos) << "Content: " << content;
}

TEST_F(AsyncFileSinkTest, MultipleWrites)
{
	auto logPath = (m_testDir / "multi.log").string();
	{
		AsyncFileSink sink(
			AsyncFileSink::Options {.file = {.basePath = logPath}, .flushInterval = std::chrono::milliseconds(100)});

		for (int i = 0; i < 100; ++i)
		{
			auto msg = "[INFO] line " + std::to_string(i) + "\n";
			sink.write(msg);
		}
		// 等后台线程处理
		std::this_thread::sleep_for(std::chrono::milliseconds(500));
	}

	auto content = readFile(logPath);
	// 检查第一行和最后一行
	EXPECT_NE(content.find("line 0"), std::string::npos);
	EXPECT_NE(content.find("line 99"), std::string::npos);
}

TEST_F(AsyncFileSinkTest, MultiThreadWrite)
{
	auto logPath = (m_testDir / "mt.log").string();
	{
		AsyncFileSink sink(
			AsyncFileSink::Options {.file = {.basePath = logPath}, .flushInterval = std::chrono::milliseconds(100)});

		constexpr int kThreads = 4;
		constexpr int kLinesPerThread = 50;

		std::vector<std::thread> threads;
		threads.reserve(kThreads);

		for (int t = 0; t < kThreads; ++t)
		{
			threads.emplace_back(
				[&sink, t]()
				{
					for (int i = 0; i < kLinesPerThread; ++i)
					{
						auto msg = "[INFO] t" + std::to_string(t) + " i" + std::to_string(i) + "\n";
						sink.write(msg);
					}
				});
		}
		for (auto& th : threads)
		{
			th.join();
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(500));
	}

	auto content = readFile(logPath);
	// 验证所有线程都有输出
	for (int t = 0; t < 4; ++t)
	{
		auto needle = "t" + std::to_string(t) + " i0";
		EXPECT_NE(content.find(needle), std::string::npos) << "Missing thread " << t;
	}
}

TEST_F(AsyncFileSinkTest, GracefulShutdown)
{
	auto logPath = (m_testDir / "shutdown.log").string();
	{
		AsyncFileSink sink(AsyncFileSink::Options {
			.file = {.basePath = logPath},
			.flushInterval = std::chrono::milliseconds(5000) // 长 interval
		});

		sink.write("[INFO] before shutdown\n");
		// 不手动 flush，依赖析构时的 jthread stop+join
	}

	auto content = readFile(logPath);
	EXPECT_NE(content.find("before shutdown"), std::string::npos)
		<< "Data should be flushed on shutdown. Content: " << content;
}

TEST_F(AsyncFileSinkTest, DroppedCountInitiallyZero)
{
	auto logPath = (m_testDir / "drop.log").string();
	AsyncFileSink sink(
		AsyncFileSink::Options {.file = {.basePath = logPath}, .flushInterval = std::chrono::milliseconds(100)});
	EXPECT_EQ(sink.droppedCount(), 0u);
}

TEST_F(AsyncFileSinkTest, FileRotationWithAsync)
{
	auto logPath = (m_testDir / "rotate.log").string();
	{
		AsyncFileSink sink(AsyncFileSink::Options {.file = {.basePath = logPath, .maxFileSize = 100, .maxFiles = 3},
												   .flushInterval = std::chrono::milliseconds(50)});

		// 写入足够多的数据触发轮转
		for (int i = 0; i < 50; ++i)
		{
			auto msg = "[INFO] rotation test line " + std::to_string(i) + " with extra padding data\n";
			sink.write(msg);
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(300));
	}

	// 检查有轮转文件
	int logFileCount = 0;
	for (const auto& entry : fs::directory_iterator(m_testDir))
	{
		if (entry.path().extension() == ".log")
		{
			++logFileCount;
		}
	}
	EXPECT_GE(logFileCount, 2) << "Should have rotated files";
}

TEST_F(AsyncFileSinkTest, SinkLevelFilter)
{
	auto logPath = (m_testDir / "level.log").string();
	{
		AsyncFileSink sink(
			AsyncFileSink::Options {.file = {.basePath = logPath}, .flushInterval = std::chrono::milliseconds(100)});
		sink.setLevel(LogLevel::hError);
		EXPECT_EQ(sink.sinkLevel(), LogLevel::hError);
	}
}
