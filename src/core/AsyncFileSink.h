/**
 * @file AsyncFileSink.h
 * @brief 异步双缓冲文件日志 Sink
 */

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
	 * 前端线程 append 到 curBuf_（mutex 保护），后台线程 swap 后批量写盘。
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

		Options opts_;
		LogFile logFile_;

		std::mutex bufMutex_;
		std::string curBuf_;
		std::string flushBuf_;
		std::condition_variable_any cond_;
		std::atomic<uint64_t> dropped_ {0};
		std::deque<std::promise<void>> flushRequests_;
		std::jthread bgThread_; // 必须最后声明：确保先 join 再析构 flushRequests_
	};

} // namespace hical
