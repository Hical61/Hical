#include "LogChannel.h"

#include <utility>

namespace hical
{

	// ============ LogChannel ============

	LogChannel::LogChannel(std::string name)
		: m_name(std::move(name))
		, m_formatter(std::make_shared<TextFormatter>())
		, m_sinks(std::make_shared<const std::vector<std::shared_ptr<LogSink>>>())
	{
	}

	const std::string& LogChannel::name() const
	{
		return m_name;
	}

	void LogChannel::setLevel(LogLevel lvl)
	{
		m_level.store(lvl, std::memory_order_relaxed);
	}

	LogLevel LogChannel::level() const
	{
		return m_level.load(std::memory_order_relaxed);
	}

	void LogChannel::setFormatter(std::shared_ptr<LogFormatter> formatter)
	{
		if (!formatter)
		{
			return; // 拒绝 nullptr，保持当前 formatter
		}
		std::lock_guard<std::mutex> lock(m_mutex);
		m_formatter = std::move(formatter);
	}

	void LogChannel::addSink(std::shared_ptr<LogSink> sink)
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		auto newSinks = std::make_shared<std::vector<std::shared_ptr<LogSink>>>(*m_sinks);
		newSinks->push_back(std::move(sink));
		m_sinks = std::move(newSinks);
	}

	void LogChannel::clearSinks()
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_sinks = std::make_shared<const std::vector<std::shared_ptr<LogSink>>>();
	}

	void LogChannel::emit(const LogRecord& record)
	{
		if (record.level < m_level.load(std::memory_order_relaxed))
		{
			return;
		}

		// COW snapshot：锁内仅拷贝 shared_ptr，不拷贝 vector
		std::shared_ptr<LogFormatter> fmtSnap;
		std::shared_ptr<const std::vector<std::shared_ptr<LogSink>>> sinksSnap;
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			fmtSnap = m_formatter;
			sinksSnap = m_sinks;
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
			std::shared_lock<std::shared_mutex> rlock(m_mutex);
			auto it = m_channels.find(name);
			if (it != m_channels.end())
			{
				return it->second;
			}
		}
		std::unique_lock<std::shared_mutex> wlock(m_mutex);
		// 双重检查
		auto it = m_channels.find(name);
		if (it != m_channels.end())
		{
			return it->second;
		}
		auto ch = std::make_shared<LogChannel>(name);
		m_channels[name] = ch;
		return ch;
	}

	std::shared_ptr<LogChannel> LogChannelRegistry::get(const std::string& name) const
	{
		std::shared_lock<std::shared_mutex> rlock(m_mutex);
		auto it = m_channels.find(name);
		if (it != m_channels.end())
		{
			return it->second;
		}
		return nullptr;
	}

	std::vector<std::pair<std::string, LogLevel>> LogChannelRegistry::listChannels() const
	{
		std::shared_lock<std::shared_mutex> rlock(m_mutex);
		std::vector<std::pair<std::string, LogLevel>> result;
		result.reserve(m_channels.size());
		for (const auto& [name, ch] : m_channels)
		{
			result.emplace_back(name, ch->level());
		}
		return result;
	}

} // namespace hical
