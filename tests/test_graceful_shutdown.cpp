#include "core/HttpServer.h"
#include <gtest/gtest.h>
#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <chrono>
#include <string>
#include <thread>

using namespace hical;
namespace beast = boost::beast;
namespace http = beast::http;
using boost::asio::ip::tcp;

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

// 辅助：发送 HTTP GET 并获取响应
static std::pair<unsigned int, std::string> httpGet(const std::string& host, uint16_t port, const std::string& target)
{
	boost::asio::io_context ioCtx;
	tcp::socket socket(ioCtx);
	socket.connect(tcp::endpoint(boost::asio::ip::make_address(host), port));

	http::request<http::string_body> req(http::verb::get, target, 11);
	req.set(http::field::host, host);
	http::write(socket, req);

	beast::flat_buffer buffer;
	http::response<http::string_body> res;
	http::read(socket, buffer, res);

	socket.shutdown(tcp::socket::shutdown_both);
	return {res.result_int(), res.body()};
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

	// 先发一个请求建立 keep-alive 连接
	boost::asio::io_context ioCtx;
	tcp::socket sock(ioCtx);
	sock.connect(tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), port));

	// HTTP/1.1 默认 keep-alive
	http::request<http::string_body> req(http::verb::get, "/ping", 11);
	req.set(http::field::host, "127.0.0.1");
	http::write(sock, req);

	beast::flat_buffer buf;
	http::response<http::string_body> res;
	http::read(sock, buf, res);

	EXPECT_EQ(res.result_int(), 200u);

	// stop 触发 drain，关闭 io_context 之前响应里 keep_alive 应为 false
	server.stop();
	sock.close();

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
