#include "AsyncFileSink.h"

#include <utility>

namespace hical
{

	AsyncFileSink::AsyncFileSink(Options opts) : m_opts(std::move(opts)), m_logFile(m_opts.file)
	{
		m_curBuf.reserve(m_opts.bufferSize);
		m_flushBuf.reserve(m_opts.bufferSize);

		m_bgThread = std::jthread(
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
			std::lock_guard<std::mutex> lock(m_bufMutex);
			// 用条件变量唤醒后台线程处理剩余数据
			m_cond.notify_one();
		}
		// jthread 析构时自动 request_stop + join，后台线程会处理最后一批数据
	}

	void AsyncFileSink::write(std::string_view formattedLine)
	{
		bool shouldNotify = false;
		{
			std::lock_guard<std::mutex> lock(m_bufMutex);

			// 背压保护：缓冲区过大时丢弃
			if (m_curBuf.size() > m_opts.backpressureLimit)
			{
				m_dropped.fetch_add(1, std::memory_order_relaxed);
				return;
			}

			auto wasBufEmpty = m_curBuf.empty();
			m_curBuf.append(formattedLine.data(), formattedLine.size());

			// 从空到非空，或缓冲区接近满时通知后台线程
			shouldNotify = wasBufEmpty || m_curBuf.size() >= m_opts.bufferSize;
		}

		if (shouldNotify)
		{
			m_cond.notify_one();
		}
	}

	void AsyncFileSink::flush()
	{
		// 同步 flush：通过 promise/future 握手，确保后台线程完成写盘
		std::promise<void> p;
		auto f = p.get_future();
		{
			std::lock_guard<std::mutex> lock(m_bufMutex);
			m_flushRequests.push_back(std::move(p));
			m_cond.notify_one();
		}
		f.wait();
	}

	uint64_t AsyncFileSink::droppedCount() const
	{
		return m_dropped.load(std::memory_order_relaxed);
	}

	void AsyncFileSink::backgroundLoop(std::stop_token stopToken)
	{
		while (true)
		{
			{
				std::unique_lock<std::mutex> lock(m_bufMutex);

				// 等待数据到达、超时、或停止请求
				auto hasData = m_cond.wait_for(lock,
											   stopToken,
											   m_opts.flushInterval,
											   [this]()
											   {
												   return !m_curBuf.empty();
											   });

				if (stopToken.stop_requested() && m_curBuf.empty())
				{
					break;
				}

				if (!m_curBuf.empty())
				{
					// 双缓冲交换
					m_curBuf.swap(m_flushBuf);
					m_curBuf.clear();
				}
				else
				{
					continue;
				}
			}

			// 锁外批量写盘
			if (!m_flushBuf.empty())
			{
				// 插入丢弃统计
				auto dropped = m_dropped.exchange(0, std::memory_order_relaxed);
				if (dropped > 0)
				{
					char dropMsg[128];
					auto n = snprintf(dropMsg,
									  sizeof(dropMsg),
									  "[WARN] AsyncFileSink: %llu log lines dropped due to backpressure\n",
									  static_cast<unsigned long long>(dropped));
					m_logFile.append(dropMsg, static_cast<size_t>(n));
				}

				m_logFile.append(m_flushBuf.data(), m_flushBuf.size());
				m_logFile.flush();
				m_flushBuf.clear();
			}

			// 通知所有等待同步 flush 的调用方
			{
				std::lock_guard<std::mutex> lock(m_bufMutex);
				for (auto& req : m_flushRequests)
				{
					req.set_value();
				}
				m_flushRequests.clear();
			}
		}

		// 停止前确保 m_curBuf 残余数据也被写入
		{
			std::lock_guard<std::mutex> lock(m_bufMutex);
			if (!m_curBuf.empty())
			{
				m_curBuf.swap(m_flushBuf);
				m_curBuf.clear();
			}
		}
		if (!m_flushBuf.empty())
		{
			m_logFile.append(m_flushBuf.data(), m_flushBuf.size());
			m_logFile.flush();
		}

		// 关闭前通知所有残留的 flush 等待者
		{
			std::lock_guard<std::mutex> lock(m_bufMutex);
			for (auto& req : m_flushRequests)
			{
				req.set_value();
			}
			m_flushRequests.clear();
		}
	}

} // namespace hical
