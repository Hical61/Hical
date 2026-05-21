#include "core/LogFile.h"
#include <gtest/gtest.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace hical;

namespace fs = std::filesystem;

class LogFileTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		// 每个测试用独立的子目录，避免并行测试互相干扰
		testDir_ = fs::temp_directory_path()
				   / ("hical_logfile_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
		fs::create_directories(testDir_);
	}

	void TearDown() override
	{
		std::error_code ec;
		fs::remove_all(testDir_, ec);
	}

	fs::path testDir_;
};

TEST_F(LogFileTest, BasicWrite)
{
	auto logPath = (testDir_ / "test.log").string();
	LogFile lf(LogFile::Options {.basePath = logPath});

	std::string msg = "hello world\n";
	lf.append(msg.data(), msg.size());
	lf.flush();

	std::ifstream ifs(logPath);
	std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
	EXPECT_EQ(content, "hello world\n");
}

TEST_F(LogFileTest, WrittenBytesTracking)
{
	auto logPath = (testDir_ / "test.log").string();
	LogFile lf(LogFile::Options {.basePath = logPath});

	EXPECT_EQ(lf.writtenBytes(), 0u);

	std::string msg = "12345\n";
	lf.append(msg.data(), msg.size());
	EXPECT_EQ(lf.writtenBytes(), 6u);

	lf.append(msg.data(), msg.size());
	EXPECT_EQ(lf.writtenBytes(), 12u);
}

TEST_F(LogFileTest, RotationOnSize)
{
	auto logPath = (testDir_ / "test.log").string();
	// 很小的 maxFileSize 触发频繁轮转
	LogFile lf(LogFile::Options {.basePath = logPath, .maxFileSize = 20, .maxFiles = 10});

	std::string msg = "line of text here\n"; // 18 bytes
	lf.append(msg.data(), msg.size());
	// 第二次写入超过 20 字节限制，触发轮转
	lf.append(msg.data(), msg.size());
	lf.flush();

	// 应该有轮转文件
	int logFileCount = 0;
	for (const auto& entry : fs::directory_iterator(testDir_))
	{
		if (entry.path().extension() == ".log")
		{
			++logFileCount;
		}
	}
	// 至少 2 个文件：当前 + 1 个轮转
	EXPECT_GE(logFileCount, 2);
}

TEST_F(LogFileTest, MaxFilesLimit)
{
	auto logPath = (testDir_ / "test.log").string();
	LogFile lf(LogFile::Options {.basePath = logPath, .maxFileSize = 10, .maxFiles = 3});

	// 写入很多次触发多次轮转
	for (int i = 0; i < 20; ++i)
	{
		std::string msg = "data line " + std::to_string(i) + "\n";
		lf.append(msg.data(), msg.size());
	}
	lf.flush();

	// 统计总 .log 文件数
	int logFileCount = 0;
	for (const auto& entry : fs::directory_iterator(testDir_))
	{
		if (entry.path().extension() == ".log")
		{
			++logFileCount;
		}
	}
	// 不应超过 maxFiles + 1（当前文件 + maxFiles 个轮转文件）
	EXPECT_LE(logFileCount, 4);
}

TEST_F(LogFileTest, RotatedFileNaming)
{
	auto logPath = (testDir_ / "app.log").string();
	LogFile lf(LogFile::Options {.basePath = logPath, .maxFileSize = 10, .maxFiles = 10});

	std::string msg = "trigger rotation!\n";
	lf.append(msg.data(), msg.size());
	lf.append(msg.data(), msg.size());
	lf.flush();

	// 检查轮转文件命名格式
	for (const auto& entry : fs::directory_iterator(testDir_))
	{
		auto name = entry.path().filename().string();
		if (name == "app.log")
		{
			continue; // 跳过当前文件
		}
		// 格式：app.YYMMDD-HHMMSS.NNNNNN.log
		EXPECT_TRUE(name.starts_with("app.")) << "Bad name: " << name;
		EXPECT_TRUE(name.ends_with(".log")) << "Bad name: " << name;
		// 应包含时间戳和序列号
		EXPECT_GT(name.size(), std::string("app.YYMMDD-HHMMSS.NNNNNN.log").size() - 2) << "Name too short: " << name;
	}
}

TEST_F(LogFileTest, AppendAfterRotation)
{
	auto logPath = (testDir_ / "test.log").string();
	LogFile lf(LogFile::Options {.basePath = logPath, .maxFileSize = 20, .maxFiles = 5});

	// 多次写入，跨越多次轮转
	for (int i = 0; i < 10; ++i)
	{
		std::string msg = "line " + std::to_string(i) + " content\n";
		lf.append(msg.data(), msg.size());
	}
	lf.flush();

	// 当前文件应存在
	EXPECT_TRUE(fs::exists(logPath));
}

TEST_F(LogFileTest, CreateDirectoryIfNeeded)
{
	auto nestedDir = testDir_ / "sub" / "dir";
	auto logPath = (nestedDir / "test.log").string();

	LogFile lf(LogFile::Options {.basePath = logPath});

	std::string msg = "nested dir test\n";
	lf.append(msg.data(), msg.size());
	lf.flush();

	EXPECT_TRUE(fs::exists(logPath));
}
