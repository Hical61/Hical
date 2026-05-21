#include "core/Log.h"
#include "core/LogChannel.h"
#include "core/LogFormatter.h"
#include "core/LogRecord.h"
#include "core/LogSink.h"
#include <gtest/gtest.h>
#include <memory>
#include <mutex>
#include <string>

using namespace hical;

// 简单的内存 Sink 用于测试
class TestSink : public LogSink
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

	void clear()
	{
		std::lock_guard<std::mutex> lock(mutex_);
		data_.clear();
	}

private:
	std::mutex mutex_;
	std::string data_;
};

static LogRecord makeRecord(LogLevel lvl = LogLevel::hInfo, const char* msg = "channel test")
{
	LogRecord r;
	r.level = lvl;
	r.timestamp = std::chrono::system_clock::now();
	r.threadId = 999;
	r.file = "test.cpp";
	r.line = 10;
	r.message = msg;
	return r;
}

// ============ LogChannel 测试 ============

TEST(LogChannelTest, BasicEmit)
{
	auto sink = std::make_shared<TestSink>();
	LogChannel ch("test");
	ch.addSink(sink);

	auto record = makeRecord();
	ch.emit(record);

	EXPECT_NE(sink->data().find("channel test"), std::string::npos);
}

TEST(LogChannelTest, LevelFilter)
{
	auto sink = std::make_shared<TestSink>();
	LogChannel ch("test");
	ch.addSink(sink);
	ch.setLevel(LogLevel::hWarn);

	auto infoRecord = makeRecord(LogLevel::hInfo, "should be filtered");
	ch.emit(infoRecord);
	EXPECT_TRUE(sink->data().empty());

	auto warnRecord = makeRecord(LogLevel::hWarn, "should pass");
	ch.emit(warnRecord);
	EXPECT_NE(sink->data().find("should pass"), std::string::npos);
}

TEST(LogChannelTest, CustomFormatter)
{
	auto sink = std::make_shared<TestSink>();
	LogChannel ch("json-channel");
	ch.addSink(sink);
	ch.setFormatter(std::make_shared<JsonFormatter>());

	auto record = makeRecord();
	record.fields["key"] = "value";
	ch.emit(record);

	auto data = sink->data();
	EXPECT_NE(data.find("\"level\":\"INFO\""), std::string::npos);
	EXPECT_NE(data.find("\"key\":\"value\""), std::string::npos);
}

TEST(LogChannelTest, MultipleSinks)
{
	auto sink1 = std::make_shared<TestSink>();
	auto sink2 = std::make_shared<TestSink>();
	LogChannel ch("multi");
	ch.addSink(sink1);
	ch.addSink(sink2);

	ch.emit(makeRecord());

	EXPECT_NE(sink1->data().find("channel test"), std::string::npos);
	EXPECT_NE(sink2->data().find("channel test"), std::string::npos);
}

TEST(LogChannelTest, ClearSinks)
{
	auto sink = std::make_shared<TestSink>();
	LogChannel ch("test");
	ch.addSink(sink);
	ch.emit(makeRecord(LogLevel::hInfo, "before"));

	ch.clearSinks();
	ch.emit(makeRecord(LogLevel::hInfo, "after"));

	EXPECT_NE(sink->data().find("before"), std::string::npos);
	EXPECT_EQ(sink->data().find("after"), std::string::npos);
}

TEST(LogChannelTest, NameAccessor)
{
	LogChannel ch("my-channel");
	EXPECT_EQ(ch.name(), "my-channel");
}

// ============ LogChannelRegistry 测试 ============

TEST(LogChannelRegistryTest, GetOrCreate)
{
	LogChannelRegistry registry;
	auto ch = registry.getOrCreate("access");
	ASSERT_NE(ch, nullptr);
	EXPECT_EQ(ch->name(), "access");

	// 第二次获取应返回同一对象
	auto ch2 = registry.getOrCreate("access");
	EXPECT_EQ(ch.get(), ch2.get());
}

TEST(LogChannelRegistryTest, GetNonExistent)
{
	LogChannelRegistry registry;
	auto ch = registry.get("nonexistent");
	EXPECT_EQ(ch, nullptr);
}

TEST(LogChannelRegistryTest, ListChannels)
{
	LogChannelRegistry registry;
	registry.getOrCreate("access")->setLevel(LogLevel::hInfo);
	registry.getOrCreate("audit")->setLevel(LogLevel::hWarn);

	auto list = registry.listChannels();
	EXPECT_EQ(list.size(), 2u);

	bool foundAccess = false;
	bool foundAudit = false;
	for (const auto& [name, lvl] : list)
	{
		if (name == "access")
		{
			foundAccess = true;
			EXPECT_EQ(lvl, LogLevel::hInfo);
		}
		if (name == "audit")
		{
			foundAudit = true;
			EXPECT_EQ(lvl, LogLevel::hWarn);
		}
	}
	EXPECT_TRUE(foundAccess);
	EXPECT_TRUE(foundAudit);
}

// ============ 通道宏集成测试 ============

TEST(LogChannelTest, MacroHICAL_LOG_TO)
{
	auto sink = std::make_shared<TestSink>();
	auto ch = Logger::instance().channels().getOrCreate("test_macro");
	ch->addSink(sink);
	ch->setLevel(LogLevel::hTrace);

	HICAL_LOG_TO("test_macro", Info, "macro test port={}", 8080);

	auto data = sink->data();
	EXPECT_NE(data.find("macro test port=8080"), std::string::npos);

	// 清理
	ch->clearSinks();
}

TEST(LogChannelTest, MacroHICAL_LOG_TO_F)
{
	auto sink = std::make_shared<TestSink>();
	auto ch = Logger::instance().channels().getOrCreate("test_macro_f");
	ch->addSink(sink);
	ch->setFormatter(std::make_shared<JsonFormatter>());
	ch->setLevel(LogLevel::hTrace);

	boost::json::object fields;
	fields["status"] = 200;
	HICAL_LOG_TO_F("test_macro_f", Info, fields, "GET /api");

	auto data = sink->data();
	EXPECT_NE(data.find("\"status\":200"), std::string::npos);
	EXPECT_NE(data.find("GET /api"), std::string::npos);

	ch->clearSinks();
}
