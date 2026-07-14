/**
 * @file test_optimistic_write.cpp
 * @brief 乐观同步写 + readBufHandle 提前归还 正确性测试
 */

#include "TestHttpClient.h"
#include "core/HttpServer.h"
#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <string>
#include <thread>

using namespace hical;
using hical::test::httpGet;

namespace
{
	uint16_t startServerAndWait(HttpServer& server, std::thread& serverThread)
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
				boost::asio::ip::tcp::socket sock(io);
				sock.connect(boost::asio::ip::tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), port));
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
} // namespace

/**
 * @brief 小响应（hello-world）一次性写完不丢数据
 * 这是乐观同步写覆盖率最高的路径：head + body <= 512，单 buffer
 */
TEST(OptimisticWriteTest, SmallResponse)
{
	HttpServer server(0);
	server.router().get("/hello",
						[](const HttpRequest&) -> HttpResponse
						{
							return HttpResponse::ok("hello");
						});

	std::thread serverThread;
	uint16_t port = startServerAndWait(server, serverThread);

	auto [status, body] = httpGet("127.0.0.1", port, "/hello");
	EXPECT_EQ(status, 200);
	EXPECT_EQ(body, "hello");

	server.stop();
	serverThread.join();
}

/**
 * @brief 大响应 body > 512 字节，走 scatter-gather 或退化路径
 */
TEST(OptimisticWriteTest, LargeBodyResponse)
{
	HttpServer server(0);
	server.router().get("/large",
						[](const HttpRequest&) -> HttpResponse
						{
							// 构造一个超过 512 字节的 body
							std::string body;
							body.resize(2048, 'A');
							return HttpResponse::ok(std::move(body));
						});

	std::thread serverThread;
	uint16_t port = startServerAndWait(server, serverThread);

	auto [status, body] = httpGet("127.0.0.1", port, "/large");
	EXPECT_EQ(status, 200);
	EXPECT_EQ(body.size(), 2048);
	EXPECT_EQ(body[0], 'A');

	server.stop();
	serverThread.join();
}

/**
 * @brief 同一 keep-alive 连接连续发多个请求，
 * 验证 readBuf 提前归还不影响 pipeline 读取
 */
TEST(OptimisticWriteTest, MultipleKeepAliveRequests)
{
	HttpServer server(0);
	std::atomic<int> counter {0};
	server.router().get("/inc",
						[&counter](const HttpRequest&) -> HttpResponse
						{
							counter.fetch_add(1);
							return HttpResponse::ok(std::to_string(counter.load()));
						});

	std::thread serverThread;
	uint16_t port = startServerAndWait(server, serverThread);

	// 建立连接，发 5 个请求
	boost::asio::io_context io;
	boost::asio::ip::tcp::socket sock(io);
	sock.connect(boost::asio::ip::tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), port));

	for (int i = 0; i < 5; ++i)
	{
		std::string req = "GET /inc HTTP/1.1\r\nHost: localhost\r\nConnection: keep-alive\r\n\r\n";
		boost::asio::write(sock, boost::asio::buffer(req));

		std::string response;
		response.resize(4096);
		boost::system::error_code ec;
		size_t n = sock.read_some(boost::asio::buffer(response), ec);
		EXPECT_FALSE(ec);
		std::string_view resp(response.data(), n);
		EXPECT_NE(resp.find("200"), std::string::npos);
	}

	sock.close();
	EXPECT_EQ(counter.load(), 5);

	server.stop();
	serverThread.join();
}

/**
 * @brief 空 body 响应（状态码 204），走 skipBody/空 body 路径
 */
TEST(OptimisticWriteTest, EmptyBodyResponse)
{
	HttpServer server(0);
	server.router().post("/empty",
						 [](const HttpRequest&) -> HttpResponse
						 {
							 return HttpResponse::ok("");
						 });

	std::thread serverThread;
	uint16_t port = startServerAndWait(server, serverThread);

	boost::asio::io_context io;
	boost::asio::ip::tcp::socket sock(io);
	sock.connect(boost::asio::ip::tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), port));

	std::string req = "POST /empty HTTP/1.1\r\nHost: localhost\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
	boost::asio::write(sock, boost::asio::buffer(req));

	std::string response;
	boost::system::error_code ec;
	for (;;)
	{
		char buf[256];
		size_t n = sock.read_some(boost::asio::buffer(buf), ec);
		if (ec == boost::asio::error::eof || ec == boost::asio::error::connection_reset)
		{
			break;
		}
		EXPECT_FALSE(ec);
		response.append(buf, n);
	}
	EXPECT_NE(response.find("200"), std::string::npos);

	sock.close();
	server.stop();
	serverThread.join();
}

/**
 * @brief 404 错误响应走 sendRawResponse，验证乐观同步写对错误路径也生效
 */
TEST(OptimisticWriteTest, ErrorResponse404)
{
	HttpServer server(0);

	std::thread serverThread;
	uint16_t port = startServerAndWait(server, serverThread);

	auto [status, body] = httpGet("127.0.0.1", port, "/nonexistent");
	EXPECT_EQ(status, 404);

	server.stop();
	serverThread.join();
}

/**
 * @brief 验证 non_blocking(true) 后 server 完整读写路径正常
 * non_blocking(true) 在 handleSession 入口执行，确保 write_some 在阻塞 fd
 * 上不会进 poll() 忙等。用带 body 的 POST 覆盖读取 + 乐观同步写的完整路径。
 */
TEST(OptimisticWriteTest, NonBlockingSocketWithBodyRead)
{
	HttpServer server(0);
	server.router().post("/echo",
						 [](const HttpRequest& req) -> HttpResponse
						 {
							 return HttpResponse::ok(req.body());
						 });

	std::thread serverThread;
	uint16_t port = startServerAndWait(server, serverThread);

	boost::asio::io_context io;
	boost::asio::ip::tcp::socket sock(io);
	sock.connect(boost::asio::ip::tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), port));

	std::string bodyData(1024, 'X');
	std::string req =
		"POST /echo HTTP/1.1\r\nHost: localhost\r\nContent-Length: 1024\r\nConnection: close\r\n\r\n" + bodyData;
	boost::asio::write(sock, boost::asio::buffer(req));

	std::string response;
	boost::system::error_code ec;
	for (;;)
	{
		char buf[512];
		size_t n = sock.read_some(boost::asio::buffer(buf), ec);
		if (ec == boost::asio::error::eof || ec == boost::asio::error::connection_reset)
		{
			break;
		}
		EXPECT_FALSE(ec);
		response.append(buf, n);
	}
	EXPECT_NE(response.find("200"), std::string::npos);
	EXPECT_NE(response.find("X"), std::string::npos);

	sock.close();
	server.stop();
	serverThread.join();
}

/**
 * @brief 三层异步中间件在消除转发协程帧后洋葱顺序不回归
 * B1/B2 把 co_return co_await mw(r, next) 改成 return mw(r, next)，
 * 中间件的前置/后置执行顺序应当和原来完全一致。
 */
TEST(OptimisticWriteTest, AsyncMiddlewareOnionOrder)
{
	HttpServer server(0);
	std::vector<int> order;
	std::mutex mtx;

	// 三层异步中间件：各自在前置和后置阶段记录顺序
	server.use(
		[&order, &mtx](HttpRequest& req, MiddlewareNext next) -> Awaitable<HttpResponse>
		{
			{
				std::lock_guard lk(mtx);
				order.push_back(1);
			}
			auto res = co_await next(req);
			{
				std::lock_guard lk(mtx);
				order.push_back(6);
			}
			co_return res;
		});

	server.use(
		[&order, &mtx](HttpRequest& req, MiddlewareNext next) -> Awaitable<HttpResponse>
		{
			{
				std::lock_guard lk(mtx);
				order.push_back(2);
			}
			auto res = co_await next(req);
			{
				std::lock_guard lk(mtx);
				order.push_back(5);
			}
			co_return res;
		});

	server.use(
		[&order, &mtx](HttpRequest& req, MiddlewareNext next) -> Awaitable<HttpResponse>
		{
			{
				std::lock_guard lk(mtx);
				order.push_back(3);
			}
			auto res = co_await next(req);
			{
				std::lock_guard lk(mtx);
				order.push_back(4);
			}
			co_return res;
		});

	server.router().get("/onion",
						[&order, &mtx](const HttpRequest&) -> HttpResponse
						{
							{
								std::lock_guard lk(mtx);
								order.push_back(0);
							}
							return HttpResponse::ok("ok");
						});

	std::thread serverThread;
	uint16_t port = startServerAndWait(server, serverThread);

	auto [status, body] = httpGet("127.0.0.1", port, "/onion");
	EXPECT_EQ(status, 200);

	// 洋葱前置：1 -> 2 -> 3 -> handler(0)
	// 洋葱后置：4 -> 5 -> 6
	ASSERT_EQ(order.size(), 7);
	EXPECT_EQ(order[0], 1);
	EXPECT_EQ(order[1], 2);
	EXPECT_EQ(order[2], 3);
	EXPECT_EQ(order[3], 0); // handler
	EXPECT_EQ(order[4], 4);
	EXPECT_EQ(order[5], 5);
	EXPECT_EQ(order[6], 6);

	server.stop();
	serverThread.join();
}
