/**
 * @file AsyncFileSink.cpp
 * @brief 异步双缓冲文件 Sink 实现
 */

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
		// 唤醒后台线程把剩余数据写完，jthread 析构自动 stop + join
		{
			std::lock_guard<std::mutex> lock(bufMutex_);
			cond_.notify_one();
		}
	}

	void AsyncFileSink::write(std::string_view formattedLine)
	{
		bool shouldNotify = false;
		{
			std::lock_guard<std::mutex> lock(bufMutex_);

			// 背压，缓冲区太大就丢
			if (curBuf_.size() > opts_.backpressureLimit)
			{
				dropped_.fetch_add(1, std::memory_order_relaxed);
				return;
			}

			auto wasBufEmpty = curBuf_.empty();
			curBuf_.append(formattedLine.data(), formattedLine.size());

			// 有数据了或快满了就通知后台
			shouldNotify = wasBufEmpty || curBuf_.size() >= opts_.bufferSize;
		}

		if (shouldNotify)
		{
			cond_.notify_one();
		}
	}

	void AsyncFileSink::flush()
	{
		// 同步等后台写完盘再返回
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
		std::deque<std::promise<void>> batchRequests;
		while (true)
		{
			{
				std::unique_lock<std::mutex> lock(bufMutex_);

				// 空缓冲区的 flush 也需要唤醒后台线程。
				cond_.wait_for(lock,
							   stopToken,
							   opts_.flushInterval,
							   [this]()
							   {
								   return !curBuf_.empty() || !flushRequests_.empty();
							   });

				if (stopToken.stop_requested() && curBuf_.empty() && flushRequests_.empty())
				{
					break;
				}

				if (curBuf_.empty() && flushRequests_.empty())
				{
					continue;
				}

				// 数据和等待者一起截取，写盘期间新入队的 flush 留给下一批。
				curBuf_.swap(flushBuf_);
				batchRequests.swap(flushRequests_);
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
				flushBuf_.clear();
			}
			logFile_.flush();

			// 本批数据已写出，只唤醒随本批取出的等待者；此处不用持有缓冲区锁。
			for (auto& request : batchRequests)
			{
				request.set_value();
			}
			batchRequests.clear();
		}
	}

} // namespace hical
