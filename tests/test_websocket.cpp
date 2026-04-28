#include "core/HttpServer.h"
#include "core/WebSocket.h"
#include <gtest/gtest.h>
#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/beast/websocket.hpp>
#include <atomic>
#include <chrono>
#include <string>
#include <thread>

using namespace hical;
namespace beast = boost::beast;
namespace ws = beast::websocket;
using boost::asio::ip::tcp;

// 辅助：启动服务器并等待就绪，返回实际端口
static uint16_t startWsServerAndWait(HttpServer& server, std::thread& serverThread)
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

// 测试 WebSocket Echo
TEST(WebSocketTest, EchoMessage)
{
	HttpServer server(0);

	server.router().ws("/ws/echo",
					   [](const std::string& msg, WebSocketSession& session) -> Awaitable<void>
					   {
						   co_await session.send("Echo: " + msg);
					   });

	std::thread serverThread;
	uint16_t port = startWsServerAndWait(server, serverThread);

	// 客户端 WebSocket 连接
	boost::asio::io_context ioCtx;
	tcp::socket socket(ioCtx);
	socket.connect(tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), port));

	ws::stream<tcp::socket> wsClient(std::move(socket));
	wsClient.handshake("127.0.0.1:" + std::to_string(port), "/ws/echo");

	// 发送消息
	wsClient.write(boost::asio::buffer(std::string("Hello WS")));

	// 接收回复
	beast::flat_buffer buffer;
	wsClient.read(buffer);
	std::string reply = beast::buffers_to_string(buffer.data());

	EXPECT_EQ(reply, "Echo: Hello WS");

	// 关闭
	wsClient.close(ws::close_code::normal);

	server.stop();
	serverThread.join();
}

// 测试 WebSocket 连接回调
TEST(WebSocketTest, ConnectCallback)
{
	HttpServer server(0);

	server.router().ws(
		"/ws/greet",
		[](const std::string& msg, WebSocketSession& session) -> Awaitable<void>
		{
			co_await session.send("Got: " + msg);
		},
		[](WebSocketSession& session) -> Awaitable<void>
		{
			co_await session.send("Welcome!");
		});

	std::thread serverThread;
	uint16_t port = startWsServerAndWait(server, serverThread);

	boost::asio::io_context ioCtx;
	tcp::socket socket(ioCtx);
	socket.connect(tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), port));

	ws::stream<tcp::socket> wsClient(std::move(socket));
	wsClient.handshake("127.0.0.1:" + std::to_string(port), "/ws/greet");

	// 应先收到 Welcome 消息
	beast::flat_buffer buffer;
	wsClient.read(buffer);
	std::string welcome = beast::buffers_to_string(buffer.data());
	EXPECT_EQ(welcome, "Welcome!");

	// 发送消息并接收回复
	buffer.consume(buffer.size());
	wsClient.write(boost::asio::buffer(std::string("test")));
	wsClient.read(buffer);
	std::string reply = beast::buffers_to_string(buffer.data());
	EXPECT_EQ(reply, "Got: test");

	wsClient.close(ws::close_code::normal);
	server.stop();
	serverThread.join();
}

// 测试 WebSocket 未注册路由返回 404
TEST(WebSocketTest, UnregisteredPathFallsToHttp)
{
	HttpServer server(0);

	// 仅注册 HTTP 路由，不注册 WS 路由
	server.router().get("/",
						[](const HttpRequest&) -> HttpResponse
						{
							return HttpResponse::ok("http");
						});

	std::thread serverThread;
	uint16_t port = startWsServerAndWait(server, serverThread);

	// 尝试 WS 握手到未注册路径，服务端应当作普通 HTTP 处理（404）
	// 但由于 Beast 的 ws::is_upgrade 检查，服务端不会升级
	// 直接发 HTTP 请求验证
	boost::asio::io_context ioCtx;
	tcp::socket socket(ioCtx);
	socket.connect(tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), port));

	beast::http::request<beast::http::string_body> req(beast::http::verb::get, "/ws/nonexist", 11);
	req.set(beast::http::field::host, "127.0.0.1");
	beast::http::write(socket, req);

	beast::flat_buffer buffer;
	beast::http::response<beast::http::string_body> res;
	beast::http::read(socket, buffer, res);

	EXPECT_EQ(res.result_int(), 404);

	socket.shutdown(tcp::socket::shutdown_both);
	server.stop();
	serverThread.join();
}

// ============ 安全：WebSocket 空闲超时不触发 use-after-free ============

// 测试：WebSocket 空闲超时正常关闭连接，不 crash（UAF 修复验证）
TEST(WebSocketTest, IdleTimeoutClosesCleanly)
{
	HttpServer server(0);
	server.setIdleTimeout(1.0); // 1 秒超时

	std::atomic<bool> disconnected {false};

	server.router().ws(
		"/ws/idle",
		[](const std::string&, WebSocketSession&) -> Awaitable<void>
		{
			co_return;
		},
		nullptr,
		[&disconnected](WebSocketSession&) -> Awaitable<void>
		{
			disconnected = true;
			co_return;
		});

	std::thread serverThread;
	uint16_t port = startWsServerAndWait(server, serverThread);

	// 客户端连接后不发任何消息，等待服务端超时关闭
	boost::asio::io_context ioCtx;
	tcp::socket socket(ioCtx);
	socket.connect(tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), port));

	ws::stream<tcp::socket> wsClient(std::move(socket));
	wsClient.handshake("127.0.0.1:" + std::to_string(port), "/ws/idle");

	// 等待服务端超时关闭连接（1 秒超时 + 余量）
	beast::flat_buffer buffer;
	beast::error_code ec;
	wsClient.read(buffer, ec);

	// 连接应被服务端关闭（EOF 或 closed）
	EXPECT_TRUE(ec == boost::asio::error::eof || ec == ws::error::closed || ec == boost::asio::error::connection_reset
				|| ec == boost::asio::error::operation_aborted);

	server.stop();
	serverThread.join();

	// 关键：执行到这里没 crash 说明 UAF 已修复
	EXPECT_TRUE(disconnected.load());
}

// 测试：服务器 stop() 时 WebSocket 连接有序关闭，不 crash
TEST(WebSocketTest, ServerStopDuringConnection)
{
	HttpServer server(0);
	server.setIdleTimeout(60.0); // 长超时，确保 timer pending

	server.router().ws("/ws/stop",
					   [](const std::string& msg, WebSocketSession& session) -> Awaitable<void>
					   {
						   co_await session.send("Echo: " + msg);
					   });

	std::thread serverThread;
	uint16_t port = startWsServerAndWait(server, serverThread);

	boost::asio::io_context ioCtx;
	tcp::socket socket(ioCtx);
	socket.connect(tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), port));

	ws::stream<tcp::socket> wsClient(std::move(socket));
	wsClient.handshake("127.0.0.1:" + std::to_string(port), "/ws/stop");

	// 发一条消息确保连接完全建立且 timer pending
	wsClient.write(boost::asio::buffer(std::string("hello")));
	beast::flat_buffer buffer;
	wsClient.read(buffer);
	EXPECT_EQ(beast::buffers_to_string(buffer.data()), "Echo: hello");

	// 在连接仍打开且 timer pending 时 stop 服务器
	server.stop();
	serverThread.join();

	// 关键：执行到这里没 crash 说明 timer 回调不会访问已析构的 session
}
