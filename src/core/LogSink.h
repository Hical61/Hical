/**
 * @file LogSink.h
 * @brief 可插拔日志输出后端接口
 */

#pragma once

#include "Log.h"
#include "LogFile.h"

#include <atomic>
#include <cstdio>
#include <memory>
#include <mutex>
#include <ostream>
#include <string_view>

namespace hical
{

	/**
	 * @brief 日志输出后端抽象接口
	 * Sink 接收已格式化的完整日志行（含时间戳、级别、线程ID、文件:行号、消息、换行符）。
	 * 每个 Sink 有独立的 sinkLevel 过滤。实现必须保证线程安全。
	 */
	class LogSink
	{
	public:
		virtual ~LogSink() = default;

		/**
		 * @brief 写入一行已格式化的日志
		 * @param formattedLine 完整日志行（含换行符）
		 */
		virtual void write(std::string_view formattedLine) = 0;

		/**
		 * @brief 刷新缓冲区到目标
		 */
		virtual void flush() = 0;

		void setLevel(LogLevel lvl)
		{
			sinkLevel_.store(lvl, std::memory_order_relaxed);
		}

		[[nodiscard]] LogLevel sinkLevel() const
		{
			return sinkLevel_.load(std::memory_order_relaxed);
		}

	private:
		std::atomic<LogLevel> sinkLevel_ {LogLevel::hTrace};
	};

	/**
	 * @brief 标准错误输出 Sink
	 * 线程安全（依赖 stdio 内部锁）。
	 */
	class StderrSink : public LogSink
	{
	public:
		void write(std::string_view formattedLine) override;
		void flush() override;
	};

	/**
	 * @brief 同步文件 Sink（带轮转）
	 * 内部持有 LogFile + mutex，线程安全。
	 */
	class FileSink : public LogSink
	{
	public:
		explicit FileSink(LogFile::Options opts);

		void write(std::string_view formattedLine) override;
		void flush() override;

	private:
		std::mutex mutex_;
		LogFile logFile_;
	};

	/**
	 * @brief ostream 包装 Sink（内部使用，为 setOutput(ostream&) 提供兼容桥接）
	 */
	class OStreamSink : public LogSink
	{
	public:
		explicit OStreamSink(std::ostream& os) : os_(os)
		{
		}

		void write(std::string_view formattedLine) override;
		void flush() override;

	private:
		std::ostream& os_;
		std::mutex mutex_;
	};

} // namespace hical
