/**
 * @file LogSink.cpp
 * @brief 日志输出后端实现
 */

#include "LogSink.h"

namespace hical
{

	// ============ StderrSink ============

	void StderrSink::write(std::string_view formattedLine)
	{
		fwrite(formattedLine.data(), 1, formattedLine.size(), stderr);
	}

	void StderrSink::flush()
	{
		fflush(stderr);
	}

	// ============ FileSink ============

	FileSink::FileSink(LogFile::Options opts) : logFile_(std::move(opts))
	{
	}

	void FileSink::write(std::string_view formattedLine)
	{
		std::lock_guard<std::mutex> lock(mutex_);
		logFile_.append(formattedLine.data(), formattedLine.size());
	}

	void FileSink::flush()
	{
		std::lock_guard<std::mutex> lock(mutex_);
		logFile_.flush();
	}

	// ============ OStreamSink ============

	void OStreamSink::write(std::string_view formattedLine)
	{
		std::lock_guard<std::mutex> lock(mutex_);
		os_.write(formattedLine.data(), static_cast<std::streamsize>(formattedLine.size()));
	}

	void OStreamSink::flush()
	{
		std::lock_guard<std::mutex> lock(mutex_);
		os_.flush();
	}

} // namespace hical
