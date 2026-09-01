/**
 * @file PgSocketAdapter.h
 * @brief libpq socket 到 Boost.Asio 协程的桥接适配器
 */

#pragma once

#ifdef HICAL_HAS_PGSQL

	#include "core/Coroutine.h"
	#include <boost/asio.hpp>
	#include <memory>

	#ifdef _WIN32
		#include <boost/asio/windows/object_handle.hpp>
		#include <winsock2.h>
	#else
		#include <boost/asio/posix/stream_descriptor.hpp>
	#endif

	#include <libpq-fe.h>

namespace hical::db
{

	/**
	 * @brief 把 libpq 的 socket 包装成可 co_await 的异步等待对象
	 * libpq 的非阻塞 API（PQconnectPoll、PQsendQuery、PQconsumeInput）只丢给你一个
	 * 裸 socket（PQsocket()），不会自己去注册 io_context。这个适配器把它接成 asio
	 * 的异步等待，连接状态机、发送、接收才能真正协程化，不卡 io_context 线程。
	 * socket 归 PGconn，PQfinish 才负责关。这里只是「借用」，绝不 close 它，析构时
	 * 只收拾自己建的那点桥接资源。
	 * POSIX 上手感最好：socket 就是 int，stream_descriptor 的 async_wait 直接挂
	 * wait_read / wait_write，可读可写天然分开。Windows 麻烦点，asio 没有能等读写
	 * 事件的 stream 抽象（stream_handle 是 IOCP overlapped 模型，没有 async_wait），
	 * 所以改走 WSAEventSelect + 单个 event + object_handle::async_wait，读等写等共用
	 * 同一个 event。
	 * 不保证线程安全，一个适配器只在一个 io_context 线程上用。
	 */
	class PgSocketAdapter
	{
	public:
	#ifdef _WIN32
		// Windows 下 libpq 的 socket 是 SOCKET（PQsocket() 声明返回 int 实为 SOCKET）
		using NativeSocket = SOCKET;
	#else
		// POSIX 下就是普通的 int fd
		using NativeSocket = int;
	#endif

		/**
		 * @brief 从 libpq 连接打包 socket 并准备好异步等待能力
		 * @param ioCtx 关联的 io_context（须与后续 co_await 的协程执行器一致）
		 * @param conn 已打开 socket 的 libpq 连接
		 * @warning conn 必须已完成 PQconnectStart（此时 PQsocket() 才返回有效 socket）。
		 */
		explicit PgSocketAdapter(boost::asio::io_context& ioCtx, PGconn* conn)
			: ioCtx_(ioCtx), sock_(nativeSocket(conn))
		{
			platformSetup();
		}

		/**
		 * @brief 直接从裸 socket 构造（测试 / 延迟初始化场景）
		 * @param ioCtx 关联的 io_context
		 * @param sock 原生 socket 描述符
		 */
		explicit PgSocketAdapter(boost::asio::io_context& ioCtx, NativeSocket sock) : ioCtx_(ioCtx), sock_(sock)
		{
			platformSetup();
		}

		PgSocketAdapter(const PgSocketAdapter&) = delete;
		PgSocketAdapter& operator=(const PgSocketAdapter&) = delete;

		~PgSocketAdapter()
		{
			platformTeardown();
		}

		/**
		 * @brief 等待 socket 变为可读（异步，不阻塞 io_context 线程）
		 * 对应 libpq 状态机返回 PGRES_POLLING_READING，或 PQisBusy() 为真时。
		 */
		[[nodiscard]] Awaitable<void> waitReadable()
		{
	#ifdef _WIN32
			// 先非阻塞预检：数据可能已在 async_wait 注册前到达，此时 FD_READ
			// 边沿已错过、事件永不再 signal。预检命中就立即返回，不挂起。
			if (probeReadable())
			{
				co_return;
			}
			boost::system::error_code ec;
			co_await handle_->async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, ec));
			// async_wait 返回说明事件已触发，复位 event 供下一次等待。
			// 不能放在 co_await 之前复位——那样会把「已触发但还没被等待」的
			// 事件清掉，导致本次等待永久挂起。
			resetEvent();
	#else
			boost::system::error_code ec;
			co_await descriptor_.async_wait(boost::asio::posix::stream_descriptor::wait_read,
											boost::asio::redirect_error(boost::asio::use_awaitable, ec));
	#endif
		}

		/**
		 * @brief 等待 socket 变为可写（异步，不阻塞 io_context 线程）
		 * 对应 libpq 状态机返回 PGRES_POLLING_WRITING，或 PQflush() 返回 1 时。
		 */
		[[nodiscard]] Awaitable<void> waitWritable()
		{
	#ifdef _WIN32
			// FD_WRITE 是边沿触发：只在「不可写→可写」转换时 signal 一次。
			// 连接建立后 socket 持续可写，第二次 waitWritable 不会再有边沿，
			// 会导致 libpq 握手（两次进入 PGRES_POLLING_WRITING）永久挂起。
			// 用 select 预检：socket 已可写就立即返回，不依赖一次性边沿。
			if (probeWritable())
			{
				co_return;
			}
			boost::system::error_code ec;
			co_await handle_->async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, ec));
			resetEvent();
	#else
			boost::system::error_code ec;
			co_await descriptor_.async_wait(boost::asio::posix::stream_descriptor::wait_write,
											boost::asio::redirect_error(boost::asio::use_awaitable, ec));
	#endif
		}

	private:
	#ifdef _WIN32
		static NativeSocket nativeSocket(PGconn* conn)
		{
			// libpq 在 Windows 下 PQsocket() 声明返回 int，实为 SOCKET
			return reinterpret_cast<NativeSocket>(static_cast<uintptr_t>(PQsocket(conn)));
		}

		// Windows 下 WSAEventSelect 一个 socket 只能关联一个 event，没法像 POSIX
		// 那样给可读/可写各挂一个 fd。故用单个 event 同时监听 FD_READ|FD_WRITE，
		// 读等待和写等待共用同一个 object_handle。libpq 的状态机里读等待和写等待
		// 是串行的（先读完再等写），不会同时挂起，所以单 event 足够。
		// 注意：连接刚建立时 socket 已经可写，FD_WRITE 会让 event 立刻 signal，
		// waitReadable 就可能白触发一次。这没影响，上层 PQconsumeInput + PQisBusy
		// 会再复核，没就绪还会继续 waitReadable。
		void platformSetup()
		{
			event_ = ::WSACreateEvent();
			::WSAEventSelect(sock_, event_, FD_READ | FD_WRITE | FD_CLOSE);
			handle_ = std::make_unique<boost::asio::windows::object_handle>(ioCtx_, event_);
		}

		void platformTeardown()
		{
			// 先取消挂起的 async_wait，再销毁 handle。object_handle 没有 release()，
			// 析构也不负责关闭 HANDLE，event 需用 WSACloseEvent 显式关闭。
			if (handle_)
			{
				handle_->cancel();
				handle_.reset();
			}
			if (event_ != WSA_INVALID_EVENT)
			{
				::WSACloseEvent(event_);
				event_ = WSA_INVALID_EVENT;
			}
			// socket 本身归 PGconn，这里绝不 closesocket
		}

		// 复位 event：WSAEventSelect 用的事件是手动复位，触发一次后一直保持
		// signaled。在 async_wait 返回后调用，把事件清掉供下一次等待复用。
		void resetEvent()
		{
			::WSAResetEvent(event_);
		}

		// 非阻塞预检 socket 是否已可读。用 select 0 超时避免依赖 WSAEventSelect
		// 的一次性边沿语义（见 waitReadable/waitWritable 的注释）。
		bool probeReadable() const
		{
			fd_set readSet;
			FD_ZERO(&readSet);
			FD_SET(sock_, &readSet);
			timeval zeroTimeout {};
			return ::select(0, &readSet, nullptr, nullptr, &zeroTimeout) > 0;
		}

		// 非阻塞预检 socket 是否已可写。
		bool probeWritable() const
		{
			fd_set writeSet;
			FD_ZERO(&writeSet);
			FD_SET(sock_, &writeSet);
			timeval zeroTimeout {};
			return ::select(0, nullptr, &writeSet, nullptr, &zeroTimeout) > 0;
		}

		WSAEVENT event_ = WSA_INVALID_EVENT;
		std::unique_ptr<boost::asio::windows::object_handle> handle_;
	#else
		using Descriptor = boost::asio::posix::stream_descriptor;

		static NativeSocket nativeSocket(PGconn* conn)
		{
			return PQsocket(conn);
		}

		void platformSetup()
		{
			descriptor_ = Descriptor(ioCtx_, sock_);
		}

		void platformTeardown()
		{
			descriptor_.cancel();
			// 归还 fd 所有权：release 后 asio 不再 close fd（fd 归 PGconn）
			descriptor_.release();
		}

		Descriptor descriptor_;
	#endif

		boost::asio::io_context& ioCtx_;
		NativeSocket sock_;
	};

} // namespace hical::db

#endif // HICAL_HAS_PGSQL
