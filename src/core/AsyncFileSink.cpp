#include "AsyncFileSink.h"

#include <utility>

namespace hical
{

	AsyncFileSink::AsyncFileSink(Options opts) : opts_(std::move(opts)), logFile_(opts_.file)
	{
		curBuf_.reserve(opts_.bufferSize);
		flushBuf_.reserve(opts_.bufferSize);

		bgThread_ = std::jthread(
			[this](std::stop_token stopToken)
			{
				backgroundLoop(std::move(stopToken));
			});
	}

	AsyncFileSink::~AsyncFileSink()
	{
		// jthread 析构自动 request_stop() + join()
		// 析构前先 flush 残余数据
		{
			std::lock_guard<std::mutex> lock(bufMutex_);
			// 用条件变量唤醒后台线程处理剩余数据
			cond_.notify_one();
		}
		// jthread 析构时自动 request_stop + join，后台线程会处理最后一批数据
	}

	void AsyncFileSink::write(std::string_view formattedLine)
	{
		bool shouldNotify = false;
		{
			std::lock_guard<std::mutex> lock(bufMutex_);

			// 背压保护：缓冲区过大时丢弃
			if (curBuf_.size() > opts_.backpressureLimit)
			{
				dropped_.fetch_add(1, std::memory_order_relaxed);
				return;
			}

			auto wasBufEmpty = curBuf_.empty();
			curBuf_.append(formattedLine.data(), formattedLine.size());

			// 从空到非空，或缓冲区接近满时通知后台线程
			shouldNotify = wasBufEmpty || curBuf_.size() >= opts_.bufferSize;
		}

		if (shouldNotify)
		{
			cond_.notify_one();
		}
	}

	void AsyncFileSink::flush()
	{
		// 同步 flush：通过 promise/future 握手，确保后台线程完成写盘
		std::promise<void> p;
		auto f = p.get_future();
		{
			std::lock_guard<std::mutex> lock(bufMutex_);
			flushRequests_.push_back(std::move(p));
			cond_.notify_one();
		}
		f.wait();
	}

	uint64_t AsyncFileSink::droppedCount() const
	{
		return dropped_.load(std::memory_order_relaxed);
	}

	void AsyncFileSink::backgroundLoop(std::stop_token stopToken)
	{
		while (true)
		{
			{
				std::unique_lock<std::mutex> lock(bufMutex_);

				// 等待数据到达、超时、或停止请求
				cond_.wait_for(lock,
							   stopToken,
							   opts_.flushInterval,
							   [this]()
							   {
								   return !curBuf_.empty();
							   });

				if (stopToken.stop_requested() && curBuf_.empty())
				{
					break;
				}

				if (!curBuf_.empty())
				{
					// 双缓冲交换
					curBuf_.swap(flushBuf_);
					curBuf_.clear();
				}
				else
				{
					continue;
				}
			}

			// 锁外批量写盘
			if (!flushBuf_.empty())
			{
				// 插入丢弃统计
				auto dropped = dropped_.exchange(0, std::memory_order_relaxed);
				if (dropped > 0)
				{
					char dropMsg[128];
					auto n = snprintf(dropMsg,
									  sizeof(dropMsg),
									  "[WARN] AsyncFileSink: %llu log lines dropped due to backpressure\n",
									  static_cast<unsigned long long>(dropped));
					logFile_.append(dropMsg, static_cast<size_t>(n));
				}

				logFile_.append(flushBuf_.data(), flushBuf_.size());
				logFile_.flush();
				flushBuf_.clear();
			}

			// 通知所有等待同步 flush 的调用方
			{
				std::lock_guard<std::mutex> lock(bufMutex_);
				for (auto& req : flushRequests_)
				{
					req.set_value();
				}
				flushRequests_.clear();
			}
		}

		// 停止前确保 curBuf_ 残余数据也被写入
		{
			std::lock_guard<std::mutex> lock(bufMutex_);
			if (!curBuf_.empty())
			{
				curBuf_.swap(flushBuf_);
				curBuf_.clear();
			}
		}
		if (!flushBuf_.empty())
		{
			logFile_.append(flushBuf_.data(), flushBuf_.size());
			logFile_.flush();
		}

		// 关闭前通知所有残留的 flush 等待者
		{
			std::lock_guard<std::mutex> lock(bufMutex_);
			for (auto& req : flushRequests_)
			{
				req.set_value();
			}
			flushRequests_.clear();
		}
	}

} // namespace hical
