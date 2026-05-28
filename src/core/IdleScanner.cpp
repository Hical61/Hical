/**
 * @file IdleScanner.cpp
 * @brief 空闲连接扫描器实现
 */

#include "core/IdleScanner.h"

namespace hical
{

	// 每个 io_context 线程绑一个 scanner
	static thread_local IdleScanner* tls_scanner = nullptr;

	IdleScanner* currentThreadIdleScanner()
	{
		return tls_scanner;
	}

	IdleScanner::IdleScanner(boost::asio::any_io_executor executor, int64_t timeoutMs)
		: timer_(std::in_place, executor), executor_(std::move(executor)), timeoutMs_(timeoutMs)
	{
		// 哨兵自指 = 空链表
		sentinel_.prev = &sentinel_;
		sentinel_.next = &sentinel_;
	}

	void IdleScanner::registerEntry(Entry& entry)
	{
		// 插到链表头（sentinel.next 前面）
		entry.next = sentinel_.next;
		entry.prev = &sentinel_;
		sentinel_.next->prev = &entry;
		sentinel_.next = &entry;
		++count_;
	}

	void IdleScanner::unregisterEntry(Entry& entry)
	{
		// 从链表里摘掉
		entry.prev->next = entry.next;
		entry.next->prev = entry.prev;
		entry.prev = nullptr;
		entry.next = nullptr;
		--count_;
	}

	Awaitable<void> IdleScanner::run()
	{
		tls_scanner = this;

		auto intervalMs = (std::max)(int64_t {1000}, timeoutMs_ / 4);

		while (running_.load(std::memory_order_relaxed) && timer_.has_value())
		{
			timer_->expires_after(std::chrono::milliseconds(intervalMs));
			boost::system::error_code ec;
			co_await timer_->async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, ec));

			if (ec || !running_.load(std::memory_order_relaxed))
			{
				break;
			}

			auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
						   std::chrono::steady_clock::now().time_since_epoch())
						   .count();

			Entry* curr = sentinel_.next;
			while (curr != &sentinel_)
			{
				Entry* next = curr->next;
				auto elapsed = now - curr->lastActiveMs.load(std::memory_order_relaxed);

				if (elapsed >= timeoutMs_ && curr->socket)
				{
					boost::system::error_code closeEc;
					curr->socket->close(closeEc);
				}

				curr = next;
			}
		}

		tls_scanner = nullptr;
	}

	void IdleScanner::stop()
	{
		running_.store(false, std::memory_order_relaxed);
		if (timer_.has_value())
		{
			boost::asio::post(timer_->get_executor(),
							  [this]()
							  {
								  if (timer_.has_value())
								  {
									  timer_->cancel();
								  }
							  });
		}
	}

	void IdleScanner::shutdown()
	{
		running_.store(false, std::memory_order_relaxed);
		// 趁 io_context 还活着，把 timer 干掉，切断对 timer_service 的引用。
		// 不然等 io_context 析构把 service 删了，timer 析构时就踩野内存了
		if (timer_.has_value())
		{
			timer_->cancel();
			timer_.reset();
		}
	}

	void IdleScanner::closeAll()
	{
		Entry* curr = sentinel_.next;
		while (curr != &sentinel_)
		{
			Entry* next = curr->next;
			if (curr->socket && curr->socket->is_open())
			{
				boost::system::error_code ec;
				curr->socket->close(ec);
			}
			curr = next;
		}
	}

	boost::asio::any_io_executor IdleScanner::getExecutor() const
	{
		return executor_;
	}

} // namespace hical
