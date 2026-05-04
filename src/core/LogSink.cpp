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

	FileSink::FileSink(LogFile::Options opts) : m_logFile(std::move(opts))
	{
	}

	void FileSink::write(std::string_view formattedLine)
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_logFile.append(formattedLine.data(), formattedLine.size());
	}

	void FileSink::flush()
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_logFile.flush();
	}

	// ============ OStreamSink ============

	void OStreamSink::write(std::string_view formattedLine)
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_os.write(formattedLine.data(), static_cast<std::streamsize>(formattedLine.size()));
	}

	void OStreamSink::flush()
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_os.flush();
	}

} // namespace hical
