/**
 * @file Log.cpp
 * @brief Logger 单例与宏族实现
 */

#include "Log.h"

#include "LogChannel.h"
#include "LogFormatter.h"
#include "LogRecord.h"
#include "LogSink.h"

#include <cstring>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace hical
{

	// ============ thread_local 缓存 ============

	namespace
	{
		struct ThreadIdCache
		{
			bool initialized {false};
			uint64_t threadId {0};
		};

		// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)
		thread_local ThreadIdCache tidTlsCache;

		// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

		uint64_t cachedThreadId()
		{
			auto& cache = tidTlsCache;
			if (!cache.initialized)
			{
				std::ostringstream oss;
				oss << std::this_thread::get_id();
				try
				{
					cache.threadId = std::stoull(oss.str());
				}
				catch (...)
				{
					cache.threadId = std::hash<std::thread::id> {}(std::this_thread::get_id());
				}
				cache.initialized = true;
			}
			return cache.threadId;
		}

		// 默认 StderrSink（当无 Sink 配置时使用）
		StderrSink& defaultStderrSink()
		{
			static StderrSink sSink;
			return sSink;
		}

		// 默认 TextFormatter
		const std::shared_ptr<TextFormatter>& defaultTextFormatter()
		{
			static auto sFormatter = std::make_shared<TextFormatter>();
			return sFormatter;
		}
	} // namespace

	// ============ Logger 实现 ============

	Logger::Logger()
		: sinks_(std::make_shared<const std::vector<std::shared_ptr<LogSink>>>())
		, channels_(std::make_unique<LogChannelRegistry>())
	{
	}

	Logger& Logger::instance()
	{
		static Logger sInstance;
		return sInstance;
	}

	void Logger::setLevel(LogLevel level)
	{
		level_.store(level, std::memory_order_relaxed);
	}

	LogLevel Logger::level() const
	{
		return level_.load(std::memory_order_relaxed);
	}

	void Logger::setFlushLevel(LogLevel level)
	{
		flushLevel_.store(level, std::memory_order_relaxed);
	}

	LogLevel Logger::flushLevel() const
	{
		return flushLevel_.load(std::memory_order_relaxed);
	}

	void Logger::setOutput(std::ostream& os)
	{
		auto newSinks = std::make_shared<std::vector<std::shared_ptr<LogSink>>>();
		newSinks->push_back(std::make_shared<OStreamSink>(os));
		std::lock_guard<std::mutex> lock(mutex_);
		sinks_ = std::move(newSinks);
	}

	void Logger::setOutput(const std::string& filePath)
	{
		auto fileSink = std::make_shared<FileSink>(LogFile::Options {.basePath = filePath});
		auto newSinks = std::make_shared<std::vector<std::shared_ptr<LogSink>>>();
		newSinks->push_back(std::move(fileSink));
		std::lock_guard<std::mutex> lock(mutex_);
		sinks_ = std::move(newSinks);
	}

	void Logger::log(LogLevel lvl, const char* file, int line, std::string_view msg)
	{
		output(lvl, file, line, msg);
	}

	void Logger::addSink(std::shared_ptr<LogSink> sink)
	{
		std::lock_guard<std::mutex> lock(mutex_);
		auto newSinks = std::make_shared<std::vector<std::shared_ptr<LogSink>>>(*sinks_);
		newSinks->push_back(std::move(sink));
		sinks_ = std::move(newSinks);
	}

	void Logger::setSink(std::shared_ptr<LogSink> sink)
	{
		auto newSinks = std::make_shared<std::vector<std::shared_ptr<LogSink>>>();
		newSinks->push_back(std::move(sink));
		std::lock_guard<std::mutex> lock(mutex_);
		sinks_ = std::move(newSinks);
	}

	void Logger::clearSinks()
	{
		std::lock_guard<std::mutex> lock(mutex_);
		sinks_ = std::make_shared<const std::vector<std::shared_ptr<LogSink>>>();
	}

	void Logger::setFormatter(std::shared_ptr<LogFormatter> formatter)
	{
		std::lock_guard<std::mutex> lock(mutex_);
		formatter_ = std::move(formatter);
	}

	LogChannelRegistry& Logger::channels()
	{
		return *channels_;
	}

	void Logger::output(LogLevel lvl, const char* file, int line, std::string_view msg)
	{
		LogRecord record;
		record.level = lvl;
		record.timestamp = std::chrono::system_clock::now();
		record.threadId = cachedThreadId();
		record.file = file;
		record.line = line;
		record.message = msg;

		emit(record);
	}

	void Logger::outputWithFields(LogLevel lvl,
								  const char* file,
								  int line,
								  boost::json::object fields,
								  std::string_view msg)
	{
		LogRecord record;
		record.level = lvl;
		record.timestamp = std::chrono::system_clock::now();
		record.threadId = cachedThreadId();
		record.file = file;
		record.line = line;
		record.message = msg;
		record.fields = std::move(fields);

		emit(record);
	}

	void Logger::emit(const LogRecord& record)
	{
		// COW snapshot：锁内仅拷贝 shared_ptr（1 次 atomic_inc），不拷贝 vector
		std::shared_ptr<LogFormatter> fmtSnap;
		std::shared_ptr<const std::vector<std::shared_ptr<LogSink>>> sinksSnap;
		{
			std::lock_guard<std::mutex> lock(mutex_);
			fmtSnap = formatter_;
			sinksSnap = sinks_;
		}

		// 锁外格式化（纯函数，不访问 Logger 共享状态）
		LogFormatter* formatter = fmtSnap ? fmtSnap.get() : defaultTextFormatter().get();
		auto formattedLine = formatter->format(record);

		bool needFlush =
			record.level >= flushLevel_.load(std::memory_order_relaxed) || record.level == LogLevel::hFatal;

		// 锁外分发到各 Sink（Sink 接口要求实现线程安全）
		if (sinksSnap->empty())
		{
			defaultStderrSink().write(formattedLine);
			if (needFlush)
			{
				defaultStderrSink().flush();
			}
		}
		else
		{
			for (const auto& sink : *sinksSnap)
			{
				if (record.level >= sink->sinkLevel())
				{
					sink->write(formattedLine);
				}
			}
			if (needFlush)
			{
				for (const auto& sink : *sinksSnap)
				{
					sink->flush();
				}
			}
		}

		// Fatal 级别：flush stderr 后 abort
		if (record.level == LogLevel::hFatal)
		{
			fflush(stderr);
			std::abort();
		}
	}

	void Logger::emitTo(const std::string& channelName, const LogRecord& record)
	{
		if (auto ch = channels_->get(channelName))
		{
			ch->emit(record);
		}
	}

} // namespace hical
