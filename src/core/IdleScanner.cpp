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
		: timer_(std::move(executor)), timeoutMs_(timeoutMs)
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
		// 设 thread_local，让同线程的 handleSession 能找到这个 scanner
		tls_scanner = this;

		// 扫描间隔：max(1s, timeout/4)，跟 TcpServer::idleCheckLoop 同策略
		auto intervalMs = (std::max)(int64_t {1000}, timeoutMs_ / 4);

		while (running_.load(std::memory_order_relaxed))
		{
			timer_.expires_after(std::chrono::milliseconds(intervalMs));
			boost::system::error_code ec;
			co_await timer_.async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, ec));

			if (ec || !running_.load(std::memory_order_relaxed))
			{
				break;
			}

			// 遍历链表，关掉超时的连接
			auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
						   std::chrono::steady_clock::now().time_since_epoch())
						   .count();

			Entry* curr = sentinel_.next;
			while (curr != &sentinel_)
			{
				Entry* next = curr->next; // 先存好，close 后 entry 可能被 unregister 摘掉
				auto elapsed = now - curr->lastActiveMs.load(std::memory_order_relaxed);

				if (elapsed >= timeoutMs_ && curr->socket)
				{
					boost::system::error_code closeEc;
					curr->socket->close(closeEc);
				}

				curr = next;
			}
		}

		// 清掉 thread_local 指针
		tls_scanner = nullptr;
	}

	void IdleScanner::stop()
	{
		running_.store(false, std::memory_order_relaxed);
		// cancel timer 让 async_wait 收到 operation_aborted 退出协程
		boost::asio::post(timer_.get_executor(),
						  [this]()
						  {
							  timer_.cancel();
						  });
	}

} // namespace hical
