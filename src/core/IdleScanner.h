/**
 * @file IdleScanner.h
 * @brief 集中式空闲连接扫描器
 */

#pragma once

#include "Coroutine.h"
#include <boost/asio.hpp>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>

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
		 * @brief 连接条目，直接嵌在协程栈上，不额外分配堆内存
		 */
		struct Entry
		{
			std::atomic<int64_t> lastActiveMs {0};
			boost::asio::ip::tcp::socket* socket = nullptr; // non-owning
			Entry* prev = nullptr;
			Entry* next = nullptr;

			/**
			 * @brief 自定义超时（毫秒），0 表示使用 scanner 的默认 timeoutMs
			 * SSE 长连接设 30 分钟超时，普通 keep-alive 用全局默认 60 秒。
			 */
			int64_t customTimeoutMs = 0;

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

			/**
			 * @brief 提前注销（幂等）。socket 所有权转移给其他协程时调用，
			 * 避免悬空指针残留在扫描链表上。
			 */
			void release()
			{
				if (scanner_ != nullptr)
				{
					scanner_->unregisterEntry(entry_);
					scanner_ = nullptr;
				}
			}

		private:
			IdleScanner* scanner_;
			Entry& entry_;
		};

		explicit IdleScanner(boost::asio::any_io_executor executor, int64_t timeoutMs);

		/**
		 * @brief 启动扫描协程（设置 thread_local 指针 + 周期性遍历链表关闭超时连接）
		 */
		Awaitable<void> run();

		/**
		 * @brief 停掉扫描协程（任意线程都能调，post cancel timer 让协程退出）
		 */
		void stop();

		/**
		 * @brief 在 io_context 析构前调——先 cancel timer 再销毁它，切断对 timer_service 的依赖
		 */
		void shutdown();

		/**
		 * @brief 将连接条目注册到扫描链表（单线程调用，无需加锁）
		 */
		void registerEntry(Entry& entry);
		/**
		 * @brief 从扫描链表中移除连接条目
		 */
		void unregisterEntry(Entry& entry);

		/**
		 * @brief 关闭链表中所有注册的 socket
		 * stop 时调——把还挂着的 read/write 全 abort 掉，好让协程走正常退出路径。
		 * 要求在 scanner 所属 io_context 的线程上调，或者 run() 已经退出了。
		 */
		void closeAll();

		/**
		 * @brief 拿 scanner 绑的 executor，stop() 需要 post 操作到正确线程上
		 */
		[[nodiscard]] boost::asio::any_io_executor getExecutor() const;

	private:
		// optional 是为了 shutdown() 时能提前销毁 timer，
		// 不然 io_context 析构后 timer 析构会访问已经没了的 timer_service
		std::optional<boost::asio::steady_timer> timer_;
		boost::asio::any_io_executor executor_;
		int64_t timeoutMs_;
		std::atomic<bool> running_ {true};

		Entry sentinel_;
		size_t count_ = 0;
	};

} // namespace hical
