#include "TestHttpClient.h"
#include "core/HttpServer.h"
#include <boost/asio.hpp>
#include <boost/json.hpp>
#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <string>
#include <thread>

using namespace hical;
using boost::asio::ip::tcp;
using hical::test::httpGet;
using hical::test::httpPost;

// 辅助：启动服务器并等待就绪，返回实际端口
uint16_t startServerAndWait(HttpServer& server, std::thread& serverThread)
{
	serverThread = std::thread(
		[&server]()
		{
			server.start();
		});

	// 等待端口分配
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

	// 等待可连接
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

// 测试 HttpServer 基本启动
TEST(HttpServerTest, StartAndStop)
{
	HttpServer server(0); // 端口 0 = 系统分配
	server.router().get("/",
						[](const HttpRequest&) -> HttpResponse
						{
							return HttpResponse::ok("hello");
						});

	std::thread serverThread;
	startServerAndWait(server, serverThread);
	EXPECT_TRUE(server.isRunning());

	server.stop();
	serverThread.join();
}

// 测试 HttpServer GET 请求
TEST(HttpServerTest, GetRequest)
{
	HttpServer server(0);

	server.router().get("/api/hello",
						[](const HttpRequest&) -> HttpResponse
						{
							return HttpResponse::ok("Hello from hical!");
						});

	std::thread serverThread;
	uint16_t port = startServerAndWait(server, serverThread);

	auto [status, body] = httpGet("127.0.0.1", port, "/api/hello");
	EXPECT_EQ(status, 200u);
	EXPECT_EQ(body, "Hello from hical!");

	server.stop();
	serverThread.join();
}

// 测试 HttpServer POST 请求
TEST(HttpServerTest, PostRequest)
{
	HttpServer server(0);

	server.router().post("/api/echo",
						 [](const HttpRequest& req) -> HttpResponse
						 {
							 return HttpResponse::ok(req.body());
						 });

	std::thread serverThread;
	uint16_t port = startServerAndWait(server, serverThread);

	auto [status, body] = httpPost("127.0.0.1", port, "/api/echo", "Echo this!");
	EXPECT_EQ(status, 200u);
	EXPECT_EQ(body, "Echo this!");

	server.stop();
	serverThread.join();
}

// 测试 404
TEST(HttpServerTest, NotFound)
{
	HttpServer server(0);

	std::thread serverThread;
	uint16_t port = startServerAndWait(server, serverThread);

	auto [status, body] = httpGet("127.0.0.1", port, "/nonexistent");
	EXPECT_EQ(status, 404u);

	server.stop();
	serverThread.join();
}

// 测试路径参数
TEST(HttpServerTest, PathParam)
{
	HttpServer server(0);

	server.router().get("/users/{id}",
						[](const HttpRequest& req) -> HttpResponse
						{
							return HttpResponse::ok("User " + req.param("id"));
						});

	std::thread serverThread;
	uint16_t port = startServerAndWait(server, serverThread);

	auto [status, body] = httpGet("127.0.0.1", port, "/users/42");
	EXPECT_EQ(status, 200u);
	EXPECT_EQ(body, "User 42");

	server.stop();
	serverThread.join();
}

// 测试中间件
TEST(HttpServerTest, Middleware)
{
	HttpServer server(0);

	server.use(
		[](HttpRequest& req, MiddlewareNext next) -> Awaitable<HttpResponse>
		{
			auto res = co_await next(req);
			res.setHeader("X-Powered-By", "hical");
			co_return res;
		});

	server.router().get("/api/test",
						[](const HttpRequest&) -> HttpResponse
						{
							return HttpResponse::ok("test");
						});

	std::thread serverThread;
	uint16_t port = startServerAndWait(server, serverThread);

	auto result = hical::test::httpGetFull("127.0.0.1", port, "/api/test");
	EXPECT_EQ(result.status, 200u);
	EXPECT_EQ(result.findHeader("X-Powered-By"), "hical");

	server.stop();
	serverThread.join();
}

// 测试 JSON 响应
TEST(HttpServerTest, JsonResponse)
{
	HttpServer server(0);

	server.router().get("/api/status",
						[](const HttpRequest&) -> HttpResponse
						{
							return HttpResponse::json({{"status", "ok"}, {"version", "0.2.0"}});
						});

	std::thread serverThread;
	uint16_t port = startServerAndWait(server, serverThread);

	auto result = hical::test::httpGetFull("127.0.0.1", port, "/api/status");
	EXPECT_EQ(result.status, 200u);
	EXPECT_EQ(result.findHeader("content-type"), "application/json");

	auto json = boost::json::parse(result.body);
	EXPECT_EQ(json.at("status").as_string(), "ok");

	server.stop();
	serverThread.join();
}
