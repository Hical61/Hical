/**
 * @file test_pgsql_socket.cpp
 * @brief PgSocketAdapter 异步等待测试（不依赖真实 PG）
 */

#include <gtest/gtest.h>

#ifdef HICAL_HAS_PGSQL

	#include "db/PgSocketAdapter.h"
	#include "core/Coroutine.h"
	#include <boost/asio.hpp>
	#include <boost/asio/awaitable.hpp>
	#include <boost/asio/co_spawn.hpp>

using namespace hical;
using namespace hical::db;

namespace
{
	// 跨平台 socket 类型与关闭/发送原语：Windows 用 SOCKET + closesocket，
	// POSIX 用 int + close，跟随 PgSocketAdapter::NativeSocket 保持一致。
	using NativeSocket = PgSocketAdapter::NativeSocket;

	#ifdef _WIN32
	inline int closeSocket(NativeSocket s)
	{
		return ::closesocket(s);
	}

	inline int sendByte(NativeSocket s, char c)
	{
		return ::send(s, &c, 1, 0);
	}
	#else
	inline int closeSocket(NativeSocket s)
	{
		return ::close(s);
	}

	inline int sendByte(NativeSocket s, char c)
	{
		return static_cast<int>(::send(s, &c, 1, 0));
	}
	#endif

	/**
	 * @brief 建立一对已连接的本地回环 socket，交给适配器测试异步等待
	 * 用 asio 建好 TCP 回环连接后 release()，把裸 socket 的所有权移交出去，
	 * 避免 asio socket 在析构时关闭 socket 造成 use-after-close。
	 */
	std::pair<NativeSocket, NativeSocket> makeSocketPair(boost::asio::io_context& ioCtx)
	{
		using boost::asio::ip::tcp;

		tcp::acceptor acceptor(ioCtx, tcp::endpoint(tcp::v4(), 0));
		tcp::socket client(ioCtx);
		tcp::socket server(ioCtx);

		// 显式绑 127.0.0.1：acceptor.local_endpoint() 会返回 0.0.0.0，client 连不动
		boost::system::error_code ec;
		acceptor.async_accept(server,
							  [](boost::system::error_code)
							  {
							  });
		client.connect(tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), acceptor.local_endpoint().port()), ec);
		ioCtx.run();
		ioCtx.restart();

		// release 后 socket 归调用方管，asio 不再负责关闭
		NativeSocket serverSock = server.native_handle();
		NativeSocket clientSock = client.native_handle();
		server.release();
		client.release();
		return {serverSock, clientSock};
	}
} // namespace

TEST(PgSocketAdapterTest, waitReadable_PeerWritesData_ReturnsWithoutBlocking)
{
	boost::asio::io_context ioCtx;
	auto [serverSock, clientSock] = makeSocketPair(ioCtx);

	PgSocketAdapter adapter(ioCtx, serverSock);

	bool readable = false;
	bool timedOut = false;
	boost::asio::steady_timer timeout(ioCtx);

	coSpawn(ioCtx,
			[&]() -> Awaitable<void>
			{
				co_await adapter.waitReadable();
				readable = true;
				// 等待已返回，主动取消兜底定时器，让 run 立即收尾，不空等超时时间
				timeout.cancel();
			});

	// 对端写入 1 字节，触发 serverSock 可读
	int sent = sendByte(clientSock, 'x');
	ASSERT_EQ(sent, 1);

	// 超时兜底：正常时 waitReadable 极快返回并取消 timer；万一异步等待失效，
	// 2 秒后 timer 兜底置 timedOut，避免 run 永久阻塞。
	timeout.expires_after(std::chrono::seconds(2));
	timeout.async_wait(
		[&](boost::system::error_code ec)
		{
			if (!ec)
			{
				timedOut = true;
			}
		});
	ioCtx.run();

	EXPECT_TRUE(readable);
	EXPECT_FALSE(timedOut);

	closeSocket(serverSock);
	closeSocket(clientSock);
}

TEST(PgSocketAdapterTest, waitWritable_ReturnsWithoutBlocking)
{
	boost::asio::io_context ioCtx;
	auto [serverSock, clientSock] = makeSocketPair(ioCtx);
	(void)clientSock;

	PgSocketAdapter adapter(ioCtx, serverSock);

	bool writable = false;
	bool timedOut = false;
	boost::asio::steady_timer timeout(ioCtx);

	coSpawn(ioCtx,
			[&]() -> Awaitable<void>
			{
				co_await adapter.waitWritable();
				writable = true;
				timeout.cancel();
			});

	// 刚建立的回环 socket 发送缓冲区空闲，可写事件应立即就绪
	timeout.expires_after(std::chrono::seconds(2));
	timeout.async_wait(
		[&](boost::system::error_code ec)
		{
			if (!ec)
			{
				timedOut = true;
			}
		});
	ioCtx.run();

	EXPECT_TRUE(writable);
	EXPECT_FALSE(timedOut);

	closeSocket(serverSock);
	closeSocket(clientSock);
}

// 回归测试：连续两次 waitWritable 模拟 libpq 握手两次进入 PGRES_POLLING_WRITING。
// 连接建立后 socket 持续可写，Windows 下 FD_WRITE 是边沿触发，第一次等待消费掉
// 边沿后第二次若仍依赖事件会永久挂起。此测试验证预检让第二次立即返回。
TEST(PgSocketAdapterTest, waitWritable_TwiceInARow_SecondReturnsWithoutBlocking)
{
	boost::asio::io_context ioCtx;
	auto [serverSock, clientSock] = makeSocketPair(ioCtx);
	(void)clientSock;

	PgSocketAdapter adapter(ioCtx, serverSock);

	int writesDone = 0;
	bool timedOut = false;
	boost::asio::steady_timer timeout(ioCtx);

	coSpawn(ioCtx,
			[&]() -> Awaitable<void>
			{
				co_await adapter.waitWritable();
				++writesDone;
				co_await adapter.waitWritable();
				++writesDone;
				timeout.cancel();
			});

	timeout.expires_after(std::chrono::seconds(2));
	timeout.async_wait(
		[&](boost::system::error_code ec)
		{
			if (!ec)
			{
				timedOut = true;
			}
		});
	ioCtx.run();

	EXPECT_EQ(writesDone, 2);
	EXPECT_FALSE(timedOut);

	closeSocket(serverSock);
	closeSocket(clientSock);
}

#else

TEST(PgSocketAdapterTest, DisabledByMacro_Skips)
{
	GTEST_SKIP() << "HICAL_HAS_PGSQL not defined; PostgreSQL backend disabled";
}

#endif // HICAL_HAS_PGSQL
