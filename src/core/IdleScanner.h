#pragma once

#include "Coroutine.h"
#include <boost/asio.hpp>
#include <atomic>
#include <chrono>
#include <cstdint>

namespace hical
{

	class IdleScanner;

	/// 拿当前线程绑的 IdleScanner（run() 里设的 thread_local，没启用就返回 nullptr）
	IdleScanner* currentThreadIdleScanner();

	/**
	 * @brief 每个 io_context 一个的空闲连接扫描器
	 * 一个 timer + 侵入式双向链表，干掉 per-connection 的 timer 协程开销。
	 * 跑在单线程上（同一个 io_context），不用加锁。
	 * 用法：
	 * 1. HttpServer::start() 给每个 io_context 建一个，coSpawn run()
	 * 2. handleSession 里用 Guard 注册/注销 Entry
	 * 3. 请求处理过程中 Entry::touch() 刷新时间戳
	 */
	class IdleScanner
	{
	public:
		/**
		 * @brief 连接条目（直接嵌在协程栈上，不额外分配堆内存）
		 * 双向链表节点 + 活跃时间戳 + socket 指针
		 */
		struct Entry
		{
			std::atomic<int64_t> lastActiveMs {0};
			boost::asio::ip::tcp::socket* socket = nullptr; // non-owning
			Entry* prev = nullptr;
			Entry* next = nullptr;

			/// 刷新活跃时间戳
			void touch()
			{
				lastActiveMs.store(std::chrono::duration_cast<std::chrono::milliseconds>(
									   std::chrono::steady_clock::now().time_since_epoch())
									   .count(),
								   std::memory_order_relaxed);
			}
		};

		/**
		 * @brief RAII 注册/注销守卫
		 * scanner 非空就注册，析构时注销；传 nullptr 则啥也不干。
		 * 要在 socket 关之前析构（声明在 SocketGuard 后面就行）。
		 */
		class Guard
		{
		public:
			Guard(IdleScanner* scannerPtr, Entry& entry) : scanner_(scannerPtr), entry_(entry)
			{
				if (scanner_ != nullptr)
				{
					scanner_->registerEntry(entry_);
				}
			}

			~Guard()
			{
				if (scanner_ != nullptr)
				{
					scanner_->unregisterEntry(entry_);
				}
			}

			Guard(const Guard&) = delete;
			Guard& operator=(const Guard&) = delete;
			Guard(Guard&&) = delete;
			Guard& operator=(Guard&&) = delete;

		private:
			IdleScanner* scanner_;
			Entry& entry_;
		};

		/**
		 * @brief 构造 IdleScanner
		 * @param executor 所属 io_context 的 executor
		 * @param timeoutMs 空闲超时毫秒数
		 */
		explicit IdleScanner(boost::asio::any_io_executor executor, int64_t timeoutMs);

		/// 跑扫描协程（设 thread_local 指针，定时扫过期连接）
		Awaitable<void> run();

		/// 停掉扫描（任意线程都能调，内部 post cancel timer 让协程退出）
		void stop();

		/// 挂到扫描链表（同线程调，不加锁）
		void registerEntry(Entry& entry);

		/// 从扫描链表摘掉（同线程调，不加锁）
		void unregisterEntry(Entry& entry);

	private:
		boost::asio::steady_timer timer_;
		int64_t timeoutMs_;
		std::atomic<bool> running_ {true};

		// 双向链表哨兵（prev/next 自指 = 空链表）
		Entry sentinel_;
		size_t count_ = 0;
	};

} // namespace hical
