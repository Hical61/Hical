#pragma once

#include "LogFile.h"
#include "LogSink.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <future>
#include <mutex>
#include <string>
#include <thread>

namespace hical
{

	/**
	 * @brief 异步文件日志 Sink（双缓冲 + 后台线程）
	 * 前端线程 append 到 m_curBuf（mutex 保护），后台线程 swap 后批量写盘。
	 * 使用 std::jthread + stop_token 实现优雅关闭。
	 * 背压保护：积压超限时丢弃日志并记录丢弃数量。
	 */
	class AsyncFileSink : public LogSink
	{
	public:
		struct Options
		{
			LogFile::Options file;
			size_t bufferSize = 4 * 1024 * 1024;
			size_t backpressureLimit = 8 * 1024 * 1024;
			std::chrono::milliseconds flushInterval {1000};
		};

		explicit AsyncFileSink(Options opts);
		~AsyncFileSink() override;

		AsyncFileSink(const AsyncFileSink&) = delete;
		AsyncFileSink& operator=(const AsyncFileSink&) = delete;
		AsyncFileSink(AsyncFileSink&&) = delete;
		AsyncFileSink& operator=(AsyncFileSink&&) = delete;

		void write(std::string_view formattedLine) override;
		void flush() override;

		/**
		 * @brief 获取因背压而丢弃的日志条数（原子读取）
		 */
		[[nodiscard]] uint64_t droppedCount() const;

	private:
		void backgroundLoop(std::stop_token stopToken);

		Options m_opts;
		LogFile m_logFile;

		std::mutex m_bufMutex;
		std::string m_curBuf;
		std::string m_flushBuf;
		std::condition_variable_any m_cond;
		std::jthread m_bgThread;
		std::atomic<uint64_t> m_dropped {0};
		std::deque<std::promise<void>> m_flushRequests; // flush() 同步握手队列
	};

} // namespace hical
