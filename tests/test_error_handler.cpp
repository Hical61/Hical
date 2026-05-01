#include "core/HttpServer.h"
#include <gtest/gtest.h>
#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <atomic>
#include <chrono>
#include <string>
#include <thread>

using namespace hical;
namespace beast = boost::beast;
namespace http = beast::http;
using boost::asio::ip::tcp;

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

// 自定义错误处理器：将 exception.what() 写入响应体
TEST(ErrorHandlerTest, CustomErrorHandlerReceivesExceptionMessage)
{
	HttpServer server(0);

	server.router().get("/throw",
						[](const HttpRequest&) -> Awaitable<HttpResponse>
						{
							throw std::runtime_error("test error");
							co_return HttpResponse::ok();
						});

	server.setErrorHandler(
		[](const std::exception& e, const HttpRequest&) -> HttpResponse
		{
			auto res = HttpResponse::serverError();
			res.setBody(e.what());
			return res;
		});

	std::thread serverThread;
	auto port = startServerAndWait(server, serverThread);

	auto [status, body] = httpGet("127.0.0.1", port, "/throw");

	EXPECT_EQ(status, 500u);
	EXPECT_NE(body.find("test error"), std::string::npos);

	server.stop();
	if (serverThread.joinable())
	{
		serverThread.join();
	}
}

// 未设置错误处理器时，默认返回 500
TEST(ErrorHandlerTest, DefaultFallbackReturns500)
{
	HttpServer server(0);

	server.router().get("/throw",
						[](const HttpRequest&) -> Awaitable<HttpResponse>
						{
							throw std::runtime_error("boom");
							co_return HttpResponse::ok();
						});

	std::thread serverThread;
	auto port = startServerAndWait(server, serverThread);

	auto [status, body] = httpGet("127.0.0.1", port, "/throw");

	EXPECT_EQ(status, 500u);

	server.stop();
	if (serverThread.joinable())
	{
		serverThread.join();
	}
}

// 正常请求不受错误处理器影响
TEST(ErrorHandlerTest, NormalRequestUnaffected)
{
	HttpServer server(0);

	server.router().get("/ok",
						[](const HttpRequest&) -> Awaitable<HttpResponse>
						{
							co_return HttpResponse::ok("all good");
						});

	server.setErrorHandler(
		[](const std::exception&, const HttpRequest&) -> HttpResponse
		{
			return HttpResponse::serverError();
		});

	std::thread serverThread;
	auto port = startServerAndWait(server, serverThread);

	auto [status, body] = httpGet("127.0.0.1", port, "/ok");

	EXPECT_EQ(status, 200u);
	EXPECT_EQ(body, "all good");

	server.stop();
	if (serverThread.joinable())
	{
		serverThread.join();
	}
}

// 错误处理器自身抛异常时，fallback 到 500
TEST(ErrorHandlerTest, ErrorHandlerSelfThrowFallback)
{
	HttpServer server(0);

	server.router().get("/throw",
						[](const HttpRequest&) -> Awaitable<HttpResponse>
						{
							throw std::runtime_error("original");
							co_return HttpResponse::ok();
						});

	server.setErrorHandler(
		[](const std::exception&, const HttpRequest&) -> HttpResponse
		{
			throw std::runtime_error("handler also throws");
		});

	std::thread serverThread;
	auto port = startServerAndWait(server, serverThread);

	auto [status, body] = httpGet("127.0.0.1", port, "/throw");

	EXPECT_EQ(status, 500u);

	server.stop();
	if (serverThread.joinable())
	{
		serverThread.join();
	}
}

// 经过中间件管道时异常同样被处理器捕获
TEST(ErrorHandlerTest, ExceptionThroughMiddlewarePipeline)
{
	HttpServer server(0);

	server.use(
		[](HttpRequest& req, MiddlewareNext next) -> Awaitable<HttpResponse>
		{
			co_return co_await next(req);
		});

	server.router().get("/throw",
						[](const HttpRequest&) -> Awaitable<HttpResponse>
						{
							throw std::runtime_error("pipeline error");
							co_return HttpResponse::ok();
						});

	server.setErrorHandler(
		[](const std::exception& e, const HttpRequest&) -> HttpResponse
		{
			auto res = HttpResponse::serverError();
			res.setBody(e.what());
			return res;
		});

	std::thread serverThread;
	auto port = startServerAndWait(server, serverThread);

	auto [status, body] = httpGet("127.0.0.1", port, "/throw");

	EXPECT_EQ(status, 500u);
	EXPECT_NE(body.find("pipeline error"), std::string::npos);

	server.stop();
	if (serverThread.joinable())
	{
		serverThread.join();
	}
}

// start() 之后调用 setErrorHandler 应该抛 logic_error
TEST(ErrorHandlerTest, SetAfterStartThrows)
{
	HttpServer server(0);

	server.router().get("/",
						[](const HttpRequest&) -> Awaitable<HttpResponse>
						{
							co_return HttpResponse::ok();
						});

	std::thread serverThread;
	startServerAndWait(server, serverThread);

	EXPECT_THROW(server.setErrorHandler(
					 [](const std::exception&, const HttpRequest&) -> HttpResponse
					 {
						 return HttpResponse::serverError();
					 }),
				 std::logic_error);

	server.stop();
	if (serverThread.joinable())
	{
		serverThread.join();
	}
}
