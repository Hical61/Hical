/**
 * @file test_optimistic_write.cpp
 * @brief 乐观同步写 + readBufHandle 提前归还 正确性测试
 */

#include "TestHttpClient.h"
#include "core/HttpServer.h"
#include <gtest/gtest.h>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <thread>

using namespace hical;
using hical::test::httpGet;

namespace
{
	/// 生成确定性内容：字节只要错位或重复一段，逐字节比对就对不上
	std::string makePatternBody(size_t size)
	{
		std::string body(size, '\0');
		for (size_t i = 0; i < size; ++i)
		{
			body[i] = static_cast<char>('a' + (i * 7 + i / 251) % 26);
		}
		return body;
	}

	/// 从响应头里取 Content-Length（头名大小写不敏感）
	size_t parseContentLength(std::string_view head)
	{
		static constexpr std::string_view kName = "content-length:";
		auto toLower = [](char c)
		{
			return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		};

		size_t pos = 0;
		while (pos < head.size())
		{
			auto eol = head.find("\r\n", pos);
			if (eol == std::string_view::npos)
			{
				eol = head.size();
			}
			std::string_view line = head.substr(pos, eol - pos);
			if (line.size() > kName.size())
			{
				bool match = true;
				for (size_t i = 0; i < kName.size(); ++i)
				{
					if (toLower(line[i]) != kName[i])
					{
						match = false;
						break;
					}
				}
				if (match)
				{
					std::string value(line.substr(kName.size()));
					return static_cast<size_t>(std::strtoull(value.c_str(), nullptr, 10));
				}
			}
			pos = eol + 2;
		}
		return 0;
	}

	/// 按 Content-Length 读回一个完整响应的 body（keep-alive 连接上可以连续调用）
	std::string readResponseBody(boost::asio::ip::tcp::socket& sock, size_t& headerBytes)
	{
		std::string buf;
		char tmp[8192];
		size_t headEnd = std::string::npos;
		size_t bodyNeed = 0;

		for (;;)
		{
			boost::system::error_code ec;
			size_t n = sock.read_some(boost::asio::buffer(tmp), ec);
			if (ec)
			{
				return {};
			}
			buf.append(tmp, n);

			if (headEnd == std::string::npos)
			{
				headEnd = buf.find("\r\n\r\n");
				if (headEnd == std::string::npos)
				{
					continue;
				}
				bodyNeed = parseContentLength(std::string_view(buf.data(), headEnd));
			}

			if (buf.size() >= headEnd + 4 + bodyNeed)
			{
				headerBytes = headEnd + 4;
				return buf.substr(headerBytes, bodyNeed);
			}
		}
	}

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
 * @brief 大 body 走 2-iov 乐观写：客户端先不读，把服务端的发送路径堵住，
 * 逼出 write_some 写不完 → 回退续写。收到的 body 必须逐字节和发送端一致——
 * 回退时若把已经写出去的头部/前段重发一遍，这里就会多出字节、整体错位。
 */
TEST(OptimisticWriteTest, LargeBodySlowReaderKeepsBytesIntact)
{
	HttpServer server(0);
	const std::string payload = makePatternBody(2 * 1024 * 1024);
	server.router().get("/big",
						[&payload](const HttpRequest&) -> HttpResponse
						{
							return HttpResponse::ok(payload);
						});

	std::thread serverThread;
	uint16_t port = startServerAndWait(server, serverThread);

	boost::asio::io_context io;
	boost::asio::ip::tcp::socket sock(io);
	sock.connect(boost::asio::ip::tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), port));

	// 两条请求走同一个 keep-alive 连接，第二次同样落在这条路径上
	static constexpr const char* kReq = "GET /big HTTP/1.1\r\nHost: localhost\r\nConnection: keep-alive\r\n\r\n";
	for (int round = 0; round < 2; ++round)
	{
		boost::asio::write(sock, boost::asio::buffer(kReq, std::char_traits<char>::length(kReq)));

		// 先让服务端写一会儿（客户端不读，发送缓冲区迟早被填满）
		std::this_thread::sleep_for(std::chrono::milliseconds(150));

		size_t headerBytes = 0;
		std::string body = readResponseBody(sock, headerBytes);
		ASSERT_GT(headerBytes, 0u);
		ASSERT_EQ(body.size(), payload.size());
		EXPECT_TRUE(std::memcmp(body.data(), payload.data(), payload.size()) == 0);
	}

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
