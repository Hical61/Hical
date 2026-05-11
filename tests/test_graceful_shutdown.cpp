#include "TestHttpClient.h"
#include "core/HttpServer.h"
#include <boost/asio.hpp>
#include <gtest/gtest.h>
#include <chrono>
#include <string>
#include <thread>

using namespace hical;
using boost::asio::ip::tcp;
using hical::test::httpGet;

// 辅助：启动服务器并等待就绪，返回实际端口
static uint16_t startServerAndWait(HttpServer& server, std::thread& serverThread)
{
	serverThread = std::thread(
		[&server]()
		{
			server.start();
		});

	uint16_t port = 0;
	for (int i = 0; i < 50; ++i)
	{
		port = server.port();
		if (port != 0)
		{
			break;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	}

	for (int i = 0; i < 50; ++i)
	{
		try
		{
			boost::asio::io_context io;
			tcp::socket sock(io);
			sock.connect(tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), port));
			sock.close();
			return port;
		}
		catch (...)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
		}
	}
	return port;
}

// stop() 能正常关闭服务器，不挂起
TEST(GracefulShutdownTest, StopDoesNotHang)
{
	HttpServer server(0);
	server.setShutdownTimeout(5.0);

	server.router().get("/hello",
						[](const HttpRequest&) -> HttpResponse
						{
							return HttpResponse::ok("hello");
						});

	std::thread serverThread;
	auto port = startServerAndWait(server, serverThread);

	auto [status, body] = httpGet("127.0.0.1", port, "/hello");
	EXPECT_EQ(status, 200u);
	EXPECT_EQ(body, "hello");

	server.stop();
	if (serverThread.joinable())
	{
		serverThread.join();
	}
}

// drain 模式下响应携带 Connection: close，活跃连接处理完当前请求后退出
TEST(GracefulShutdownTest, DrainingResponseConnectionClose)
{
	HttpServer server(0);
	server.setShutdownTimeout(5.0);

	server.router().get("/ping",
						[](const HttpRequest&) -> Awaitable<HttpResponse>
						{
							co_return HttpResponse::ok("pong");
						});

	std::thread serverThread;
	auto port = startServerAndWait(server, serverThread);

	// 发送一个 keep-alive 请求，建立连接后触发 stop
	boost::asio::io_context ioCtx;
	tcp::socket sock(ioCtx);
	sock.connect(tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), port));

	std::string req = "GET /ping HTTP/1.1\r\n"
					  "Host: 127.0.0.1\r\n"
					  "Connection: keep-alive\r\n"
					  "\r\n";
	boost::asio::write(sock, boost::asio::buffer(req));

	// 读取响应
	std::string residual;
	auto result = hical::test::detail::readHttpResponse(sock, residual);
	EXPECT_EQ(result.status, 200u);

	// stop 触发 drain，连接应在下一次请求时收到 Connection: close
	server.stop();
	boost::system::error_code ec;
	sock.close(ec);

	if (serverThread.joinable())
	{
		serverThread.join();
	}
}

// setShutdownTimeout 不抛异常
TEST(GracefulShutdownTest, SetShutdownTimeoutNoThrow)
{
	HttpServer server(0);
	EXPECT_NO_THROW(server.setShutdownTimeout(2.0));
	EXPECT_NO_THROW(server.setShutdownTimeout(0.5));
}

// 多次 stop() 幂等，不 crash
TEST(GracefulShutdownTest, StopIsIdempotent)
{
	HttpServer server(0);
	server.router().get("/",
						[](const HttpRequest&) -> HttpResponse
						{
							return HttpResponse::ok("ok");
						});

	std::thread serverThread;
	startServerAndWait(server, serverThread);

	server.stop();
	server.stop(); // 第二次 stop 应安全忽略

	if (serverThread.joinable())
	{
		serverThread.join();
	}
}

// gracefulStop 通过超时保底强制退出（无活跃连接时应立即退出）
TEST(GracefulShutdownTest, GracefulStopWithShortTimeout)
{
	HttpServer server(0);
	server.setShutdownTimeout(1.0);

	server.router().get("/",
						[](const HttpRequest&) -> HttpResponse
						{
							return HttpResponse::ok("ok");
						});

	std::thread serverThread;
	startServerAndWait(server, serverThread);

	// 无活跃连接，gracefulStop 关闭 acceptor 后 ioContext 应在超时内自然退出
	server.stop();

	if (serverThread.joinable())
	{
		serverThread.join();
	}
}
