/**
 * @file IdleFd.h
 * @brief 空闲文件描述符预留机制（防止 fd 耗尽时 accept 循环空转）
 * 设计灵感来自 Trantor 网络库的 Acceptor::idleFd_ 机制。
 * 当系统 fd 耗尽（EMFILE）时：
 * 1. 释放预留的 fd（腾出一个名额）
 * 2. accept 新连接（消费掉 pending 的连接请求）
 * 3. 立即关闭该连接
 * 4. 重新预留 fd
 * 这样可以避免 accept 循环因 EMFILE 反复立即失败导致 CPU 空转（busy loop）。
 * 跨平台：
 * - POSIX（Linux/macOS）：预留 /dev/null 的 fd
 * - Windows：IOCP 模型下不需要此机制（空实现）
 */

#pragma once

#if defined(_WIN32)

namespace hical
{

	/**
	 * @brief Windows 空实现（IOCP 模型不需要 idleFd 机制）
	 */
	class IdleFd
	{
	public:
		IdleFd() = default;
		~IdleFd() = default;

		void temporaryRelease()
		{
		}

		void reacquire()
		{
		}

		IdleFd(const IdleFd&) = delete;
		IdleFd& operator=(const IdleFd&) = delete;
	};

} // namespace hical

#else // POSIX

	#include <fcntl.h>
	#include <unistd.h>

namespace hical
{

	/**
	 * @brief POSIX 实现：预留一个 fd 指向 /dev/null
	 */
	class IdleFd
	{
	public:
		IdleFd() : fd_(::open("/dev/null", O_RDONLY | O_CLOEXEC))
		{
		}

		~IdleFd()
		{
			if (fd_ >= 0)
			{
				::close(fd_);
			}
		}

		/**
		 * @brief 临时释放预留的 fd（为 accept 腾出空间）
		 */
		void temporaryRelease()
		{
			if (fd_ >= 0)
			{
				::close(fd_);
				fd_ = -1;
			}
		}

		/**
		 * @brief 重新预留 fd
		 */
		void reacquire()
		{
			if (fd_ < 0)
			{
				fd_ = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
			}
		}

		IdleFd(const IdleFd&) = delete;
		IdleFd& operator=(const IdleFd&) = delete;

	private:
		int fd_ {-1};
	};

} // namespace hical

#endif // _WIN32
