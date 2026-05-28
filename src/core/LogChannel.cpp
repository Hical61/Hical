/**
 * @file LogChannel.cpp
 * @brief 日志通道实现
 */

#include "LogChannel.h"

#include <utility>

namespace hical
{

	// ============ LogChannel ============

	LogChannel::LogChannel(std::string name)
		: name_(std::move(name))
		, formatter_(std::make_shared<TextFormatter>())
		, sinks_(std::make_shared<const std::vector<std::shared_ptr<LogSink>>>())
	{
	}

	const std::string& LogChannel::name() const
	{
		return name_;
	}

	void LogChannel::setLevel(LogLevel lvl)
	{
		level_.store(lvl, std::memory_order_relaxed);
	}

	LogLevel LogChannel::level() const
	{
		return level_.load(std::memory_order_relaxed);
	}

	void LogChannel::setFormatter(std::shared_ptr<LogFormatter> formatter)
	{
		if (!formatter)
		{
			return; // 拒绝 nullptr，保持当前 formatter
		}
		std::lock_guard<std::mutex> lock(mutex_);
		formatter_ = std::move(formatter);
	}

	void LogChannel::addSink(std::shared_ptr<LogSink> sink)
	{
		std::lock_guard<std::mutex> lock(mutex_);
		auto newSinks = std::make_shared<std::vector<std::shared_ptr<LogSink>>>(*sinks_);
		newSinks->push_back(std::move(sink));
		sinks_ = std::move(newSinks);
	}

	void LogChannel::clearSinks()
	{
		std::lock_guard<std::mutex> lock(mutex_);
		sinks_ = std::make_shared<const std::vector<std::shared_ptr<LogSink>>>();
	}

	void LogChannel::emit(const LogRecord& record)
	{
		if (record.level < level_.load(std::memory_order_relaxed))
		{
			return;
		}

		// COW snapshot：锁内仅拷贝 shared_ptr，不拷贝 vector
		std::shared_ptr<LogFormatter> fmtSnap;
		std::shared_ptr<const std::vector<std::shared_ptr<LogSink>>> sinksSnap;
		{
			std::lock_guard<std::mutex> lock(mutex_);
			fmtSnap = formatter_;
			sinksSnap = sinks_;
		}

		// 锁外格式化 + 分发（Sink 接口要求实现线程安全）
		auto formatted = fmtSnap->format(record);
		for (const auto& sink : *sinksSnap)
		{
			if (record.level >= sink->sinkLevel())
			{
				sink->write(formatted);
			}
		}
	}

	// ============ LogChannelRegistry ============

	std::shared_ptr<LogChannel> LogChannelRegistry::getOrCreate(const std::string& name)
	{
		{
			std::shared_lock<std::shared_mutex> rlock(mutex_);
			if (auto it = channels_.find(name); it != channels_.end())
			{
				return it->second;
			}
		}
		std::unique_lock<std::shared_mutex> wlock(mutex_);
		// 双重检查
		if (auto it = channels_.find(name); it != channels_.end())
		{
			return it->second;
		}
		auto ch = std::make_shared<LogChannel>(name);
		channels_[name] = ch;
		return ch;
	}

	std::shared_ptr<LogChannel> LogChannelRegistry::get(const std::string& name) const
	{
		std::shared_lock<std::shared_mutex> rlock(mutex_);
		if (auto it = channels_.find(name); it != channels_.end())
		{
			return it->second;
		}
		return nullptr;
	}

	std::vector<std::pair<std::string, LogLevel>> LogChannelRegistry::listChannels() const
	{
		std::shared_lock<std::shared_mutex> rlock(mutex_);
		std::vector<std::pair<std::string, LogLevel>> result;
		result.reserve(channels_.size());
		for (const auto& [name, ch] : channels_)
		{
			result.emplace_back(name, ch->level());
		}
		return result;
	}

} // namespace hical
