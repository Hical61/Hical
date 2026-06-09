/**
 * @file ReadBufferPool.h
 * @brief 读缓冲区 thread_local 对象池，让空闲连接不持有读缓冲区
 */

#pragma once

#include <cstddef>
#include <span>
#include <string>

namespace hical
{

	/**
	 * @brief 读缓冲区对象池
	 * 空闲连接不持有缓冲区，读请求时借一块，响应写完就还。
	 * 百万长连接下能省好几个 GB 的内存。
	 * thread_local 实现，无锁。SO_REUSEPORT 模型保证同一连接的 accept/read/write
	 * 在同一线程，借还不会有竞争。
	 * 还回来的 buffer 超过 kMaxReturnSize 直接扔掉，别让偶尔来的大请求把池子撑大。
	 */
	class ReadBufferPool
	{
	public:
		/// 借出的标准缓冲区大小（8 KB），和原 readBuf 保持一致
		static constexpr size_t kBufferSize = 8192;

		/// 超过此大小时归还时丢弃，不放回池（防止异常大请求污染池子）
		static constexpr size_t kMaxReturnSize = 65536;

		/// 每线程最多池化多少个，32 个 × 8 KB = 256 KB 上限
		static constexpr size_t kMaxPooled = 32;

		/**
		 * @brief RAII 借用句柄，析构时自动归还
		 * 生命周期：从 acquire() 到析构（或显式 release()）。
		 * 析构时 buf 必须不再被任何 string_view 引用。
		 */
		struct BufferHandle
		{
			std::string* buf_ = nullptr;

			BufferHandle() = default;

			explicit BufferHandle(std::string* p) : buf_(p)
			{
			}

			BufferHandle(const BufferHandle&) = delete;
			BufferHandle& operator=(const BufferHandle&) = delete;

			BufferHandle(BufferHandle&& other) noexcept : buf_(other.buf_)
			{
				other.buf_ = nullptr;
			}

			BufferHandle& operator=(BufferHandle&& other) noexcept
			{
				if (this != &other)
				{
					doRelease();
					buf_ = other.buf_;
					other.buf_ = nullptr;
				}
				return *this;
			}

			~BufferHandle()
			{
				doRelease();
			}

			/// 获取缓冲区引用（buf_ 必须非空）
			[[nodiscard]] std::string& get() const
			{
				return *buf_;
			}

			/// 提前归还（调用后 buf_ 置 nullptr，再次 get() 是 UB）
			void release()
			{
				doRelease();
				buf_ = nullptr;
			}

		private:
			void doRelease()
			{
				if (buf_ != nullptr)
				{
					ReadBufferPool::returnBuffer(buf_);
				}
			}
		};

		/**
		 * @brief 借一块读缓冲区
		 * 从线程本地池取，池空则新建（预留 kBufferSize）。
		 * 返回的 string 已 clear()，容量 >= kBufferSize。
		 */
		static BufferHandle acquire();

	private:
		static void returnBuffer(std::string* buf);

		struct PoolSlots
		{
			std::string* slots_[kMaxPooled] = {};
			size_t count_ = 0;

			~PoolSlots()
			{
#ifndef __MINGW32__
				// MinGW 下的 thread_local 析构是在 DLL TLS 回调里跑的，那个时机
				// 不确定 CRT 堆还在不在，delete 下去就是 0xc0000374。
				// 进程退出 OS 会帮我们收，再说一个线程撑死也就 2MB，无所谓了。
				for (std::string* p : std::span<std::string*>(static_cast<std::string**>(slots_), count_))
				{
					delete p;
				}
#endif
			}
		};

		static thread_local PoolSlots tlsPool;
	};

} // namespace hical
