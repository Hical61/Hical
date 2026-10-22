#include "TestHttpClient.h"
#include "core/HttpServer.h"
#include "core/WebSocket.h"
#include <gtest/gtest.h>
#include <boost/asio.hpp>
#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

using namespace hical;
using boost::asio::ip::tcp;
using hical::test::TestWsClient;

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

	boost::asio::io_context ioCtx;
	TestWsClient wsClient(ioCtx);
	wsClient.connect("127.0.0.1", port, "/ws/echo");

	wsClient.write("Hello WS");
	std::string reply = wsClient.read();
	EXPECT_EQ(reply, "Echo: Hello WS");

	wsClient.close();
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
	TestWsClient wsClient(ioCtx);
	wsClient.connect("127.0.0.1", port, "/ws/greet");

	// 应先收到 Welcome 消息
	std::string welcome = wsClient.read();
	EXPECT_EQ(welcome, "Welcome!");

	// 发送消息并接收回复
	wsClient.write("test");
	std::string reply = wsClient.read();
	EXPECT_EQ(reply, "Got: test");

	wsClient.close();
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

	// 直接发 HTTP 请求验证未注册路径返回 404
	boost::asio::io_context ioCtx;
	tcp::socket socket(ioCtx);
	socket.connect(tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), port));

	std::string httpReq = "GET /ws/nonexist HTTP/1.1\r\n"
						  "Host: 127.0.0.1\r\n"
						  "Connection: close\r\n"
						  "\r\n";
	boost::asio::write(socket, boost::asio::buffer(httpReq));

	char buf[1024];
	auto n = socket.read_some(boost::asio::buffer(buf));
	std::string response(buf, n);

	auto sp = response.find(' ');
	ASSERT_NE(sp, std::string::npos);
	EXPECT_EQ(response.substr(sp + 1, 3), "404");

	boost::system::error_code ec;
	socket.shutdown(tcp::socket::shutdown_both, ec);
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
	TestWsClient wsClient(ioCtx);
	wsClient.connect("127.0.0.1", port, "/ws/idle");

	// 等待服务端超时关闭连接（1 秒超时 + 余量）
	boost::system::error_code ec;
	wsClient.read(ec);

	// 连接应被服务端关闭（EOF 或 closed）
	EXPECT_TRUE(ec.operator bool());

	// 等待 onDisconnect 回调在 io_context 中完成
	// sanitizer 构建下（ASan/UBSan ~2-5x 性能惩罚），回调链需要更多时间
	for (int i = 0; i < 200 && !disconnected.load(); ++i)
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}

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
	TestWsClient wsClient(ioCtx);
	wsClient.connect("127.0.0.1", port, "/ws/stop");

	// 发一条消息确保连接完全建立且 timer pending
	wsClient.write("hello");
	std::string reply = wsClient.read();
	EXPECT_EQ(reply, "Echo: hello");

	// 在连接仍打开且 timer pending 时 stop 服务器
	server.stop();
	serverThread.join();

	// 关键：执行到这里没 crash 说明 timer 回调不会访问已析构的 session
}

// ============ 写锁：等待者计数不影响串行化和顺序 ============

// 两个协程同时往同一个 session 发消息，写锁必须把它们排开：
// 各自的消息按自己发出的顺序到达，帧内容也不能被另一路踩花
TEST(WebSocketTest, ConcurrentSendersSerializeAndKeepOrder)
{
	constexpr int kPerSender = 200;
	constexpr size_t kTotal = static_cast<size_t>(kPerSender) * 2;

	boost::asio::io_context ioCtx;
	tcp::acceptor acceptor(ioCtx, tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), 0));
	tcp::socket clientSock(ioCtx);
	clientSock.connect(tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), acceptor.local_endpoint().port()));
	tcp::socket serverSock = acceptor.accept();

	WebSocketSession session(std::move(serverSock));

	std::vector<std::string> received;
	std::string readBuf;
	bool timedOut = false;
	bool readError = false;

	// 兜底超时：写锁要是卡死，测试不能跟着挂住
	boost::asio::steady_timer watchdog(ioCtx, std::chrono::seconds(5));

	// 客户端：攒够一整个帧就切出来
	coSpawn(
		ioCtx,
		[&]() -> Awaitable<void>
		{
			for (;;)
			{
				if (auto hdr = parseWsFrameHeader(reinterpret_cast<const uint8_t*>(readBuf.data()), readBuf.size()))
				{
					const size_t frameSize = hdr->headerSize + static_cast<size_t>(hdr->payloadLength);
					if (readBuf.size() >= frameSize)
					{
						received.emplace_back(readBuf.substr(hdr->headerSize, static_cast<size_t>(hdr->payloadLength)));
						readBuf.erase(0, frameSize);
						if (received.size() >= kTotal)
						{
							watchdog.cancel();
							co_return;
						}
						continue;
					}
				}

				char tmp[4096];
				boost::system::error_code ec;
				size_t n =
					co_await clientSock.async_read_some(boost::asio::buffer(tmp),
														boost::asio::redirect_error(boost::asio::use_awaitable, ec));
				if (ec)
				{
					readError = true;
					watchdog.cancel();
					co_return;
				}
				readBuf.append(tmp, n);
			}
		});

	for (const char tag : {'A', 'B'})
	{
		coSpawn(ioCtx,
				[&session, tag]() -> Awaitable<void>
				{
					for (int i = 0; i < kPerSender; ++i)
					{
						co_await session.send(std::string(1, tag) + '#' + std::to_string(i));
					}
				});
	}

	watchdog.async_wait(
		[&](const boost::system::error_code& ec)
		{
			if (!ec)
			{
				timedOut = true;
				boost::system::error_code ignored;
				clientSock.close(ignored);
			}
		});

	ioCtx.run();

	EXPECT_FALSE(timedOut);
	EXPECT_FALSE(readError);
	ASSERT_EQ(received.size(), kTotal);

	// 每路消息的序号必须连续递增：插队、丢帧、帧内容被踩都会让这里对不上
	int nextIndex[2] = {0, 0};
	for (const auto& payload : received)
	{
		ASSERT_GE(payload.size(), 3u);
		ASSERT_EQ(payload[1], '#');
		ASSERT_TRUE(payload[0] == 'A' || payload[0] == 'B');
		const int sender = payload[0] == 'A' ? 0 : 1;
		EXPECT_EQ(std::stoi(payload.substr(2)), nextIndex[sender]);
		++nextIndex[sender];
	}
	EXPECT_EQ(nextIndex[0], kPerSender);
	EXPECT_EQ(nextIndex[1], kPerSender);
}

// ============ 公共单条 receive 路径 ============

// 公共 receiveMessage() 收一条不用回的纯文本：不该产生任何写，也不该把调用方
// 开着的合批窗口顺手关掉。后一条是「没活就跳过 endBatch」唯一能被观察到的差别——
// 空批本来就不落盘，只看写次数分不出来
TEST(WebSocketTest, PublicReceive_NoPendingWork_NoExtraWrite)
{
	boost::asio::io_context ioCtx;
	tcp::acceptor acceptor(ioCtx, tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), 0));
	tcp::socket clientSock(ioCtx);
	clientSock.connect(tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), acceptor.local_endpoint().port()));
	tcp::socket serverSock = acceptor.accept();

	WebSocketSession session(std::move(serverSock));

	// 兜底超时：真卡在 receive 里也不能把用例挂死
	boost::asio::steady_timer watchdog(ioCtx, std::chrono::seconds(5));
	bool timedOut = false;
	watchdog.async_wait(
		[&](const boost::system::error_code& ec)
		{
			if (!ec)
			{
				timedOut = true;
				ioCtx.stop();
			}
		});

	// 下面几个哨兵初值故意跟期望值都不一样，超时提前收场时一眼能看出来
	constexpr size_t kNotMeasured = static_cast<size_t>(-1);
	bool done = false;
	bool gotMessage = false;
	size_t writesAfterReceive = kNotMeasured;
	size_t writesAfterQueuedSend = kNotMeasured;
	size_t writesAfterEndBatch = kNotMeasured;

	coSpawn(ioCtx,
			[&]() -> Awaitable<void>
			{
				static constexpr uint8_t kMaskKey[4] = {0x12, 0x34, 0x56, 0x78};

				// 调用方自己开着合批窗口（beginBatch 是公共 API）
				session.beginBatch();

				auto frame = hical::buildMaskedWsFrame(hical::WsOpcode::hText, "quiet", kMaskKey);
				co_await boost::asio::async_write(clientSock, boost::asio::buffer(frame), boost::asio::use_awaitable);

				auto msg = co_await session.receiveMessage();
				gotMessage = msg.has_value() && msg->data == "quiet";
				writesAfterReceive = session.writeCount();

				// 还在批窗口里：这条只该入队，不该出网
				co_await session.send("after");
				writesAfterQueuedSend = session.writeCount();

				co_await session.endBatch();
				writesAfterEndBatch = session.writeCount();

				done = true;
				watchdog.cancel();
			});

	ioCtx.run();

	EXPECT_FALSE(timedOut);
	EXPECT_TRUE(done);
	EXPECT_TRUE(gotMessage);
	EXPECT_EQ(writesAfterReceive, 0u) << "单条 receive 没东西要回，却写了盘";
	EXPECT_EQ(writesAfterQueuedSend, 0u) << "合批窗口被单条 receive 提前关掉了，批内的 send 直接落了盘";
	EXPECT_EQ(writesAfterEndBatch, 1u) << "endBatch 没把批里的那条刷出去";
}
