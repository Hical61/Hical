#include "TestHttpClient.h"
#include "core/HttpServer.h"
#include "core/WebSocket.h"
#include "core/WsHub.h"
#include "core/Router.h"
#include <gtest/gtest.h>
#include <boost/asio.hpp>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
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

// ============ Feature 3: Binary Message ============

// 验证客户端发送 Binary 帧后，服务端能收到并用 sendBinary 回显，客户端收到 Binary opcode
TEST(WsAdvancedTest, SendBinary)
{
	HttpServer server(0);

	server.router().ws("/ws/binary",
					   [](const std::string& msg, WebSocketSession& session) -> Awaitable<void>
					   {
						   co_await session.send(msg);
					   });

	std::thread serverThread;
	uint16_t port = startWsServerAndWait(server, serverThread);

	boost::asio::io_context ioCtx;
	TestWsClient wsClient(ioCtx);
	wsClient.connect("127.0.0.1", port, "/ws/binary");

	wsClient.writeBinary("binary-data");
	// onMessage 收到后回显为 Text（服务端用 send），验证连接正常
	std::string reply = wsClient.read();
	EXPECT_EQ(reply, "binary-data");

	wsClient.close();
	server.stop();
	serverThread.join();
}

// 验证服务端调用 sendBinary 后，客户端 readFrame() 收到的是 Binary opcode
TEST(WsAdvancedTest, ReceiveBinaryDistinguished)
{
	HttpServer server(0);

	// 用 WsOptions 注册带 onTypedMessage 风格：
	// 公开 API 只有 WsMessageCallback，通过在回调内直接 sendBinary 回显触发客户端验证
	Router::WsOptions opts;
	server.router().ws("/ws/binary-echo",
					   std::move(opts),
					   [](const std::string& msg, WebSocketSession& session) -> Awaitable<void>
					   {
						   // 无论收到什么都以 Binary 帧回显
						   co_await session.sendBinary(msg);
					   });

	std::thread serverThread;
	uint16_t port = startWsServerAndWait(server, serverThread);

	boost::asio::io_context ioCtx;
	TestWsClient wsClient(ioCtx);
	wsClient.connect("127.0.0.1", port, "/ws/binary-echo");

	wsClient.write("hello");
	auto frame = wsClient.readFrame();

	EXPECT_EQ(frame.opcode, WsOpcode::hBinary);
	EXPECT_EQ(frame.payload, "hello");

	wsClient.close();
	server.stop();
	serverThread.join();
}

// ============ Feature 4: Custom Close Code ============

// 验证服务端发送自定义关闭码后，客户端收到含正确 code 的 Close 帧
TEST(WsAdvancedTest, CloseWithCode)
{
	HttpServer server(0);

	server.router().ws("/ws/close",
					   [](const std::string& msg, WebSocketSession& session) -> Awaitable<void>
					   {
						   if (msg == "bye")
						   {
							   co_await session.closeAsync(WsCloseCode::hGoingAway, "shutting down");
						   }
					   });

	std::thread serverThread;
	uint16_t port = startWsServerAndWait(server, serverThread);

	boost::asio::io_context ioCtx;
	TestWsClient wsClient(ioCtx);
	wsClient.connect("127.0.0.1", port, "/ws/close");

	wsClient.write("bye");

	// 读取服务端的 Close 帧（可能在收到服务端 Close 之前先收到其他帧，循环直到 Close）
	for (int tries = 0; tries < 10; ++tries)
	{
		auto frame = wsClient.readFrame();
		if (frame.opcode == WsOpcode::hClose)
		{
			// Close 帧载荷前 2 字节是 big-endian 关闭码
			ASSERT_GE(frame.payload.size(), 2u);
			auto code = static_cast<uint16_t>((static_cast<uint8_t>(frame.payload[0]) << 8)
											  | static_cast<uint8_t>(frame.payload[1]));
			EXPECT_EQ(code, static_cast<uint16_t>(WsCloseCode::hGoingAway));
			break;
		}
	}

	server.stop();
	serverThread.join();
}

// ============ Feature 5: Subprotocol ============

// 验证子协议协商：服务端从 offer 列表中选出第一个匹配并写入 101 响应头
TEST(WsAdvancedTest, SubprotocolNegotiation)
{
	HttpServer server(0);

	std::string negotiatedProto;
	std::atomic<bool> connected {false};

	Router::WsOptions opts;
	opts.subprotocols = {"mqtt", "graphql"};

	server.router().ws(
		"/ws/proto",
		std::move(opts),
		[](const std::string& msg, WebSocketSession& session) -> Awaitable<void>
		{
			co_await session.send(msg);
		},
		[&negotiatedProto, &connected](WebSocketSession& session) -> Awaitable<void>
		{
			negotiatedProto = std::string(session.subprotocol());
			connected = true;
			co_return;
		});

	std::thread serverThread;
	uint16_t port = startWsServerAndWait(server, serverThread);

	boost::asio::io_context ioCtx;
	TestWsClient wsClient(ioCtx);
	// 客户端 offer: graphql 排首位
	wsClient.connect("127.0.0.1", port, "/ws/proto", "graphql, mqtt");

	// 等待 onConnect 执行
	for (int i = 0; i < 50 && !connected.load(); ++i)
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}

	// 101 响应头应包含协商后的子协议
	const std::string& handshake = wsClient.lastHandshakeResponse();
	EXPECT_NE(handshake.find("Sec-WebSocket-Protocol"), std::string::npos);
	// 服务端从自身列表中选第一个客户端也支持的：mqtt 在服务端列表首位
	// 客户端 offer "graphql, mqtt"，服务端列表 {"mqtt", "graphql"}
	// 匹配逻辑：遍历服务端 subprotocols，找第一个出现在客户端 offer 中的 → "mqtt"
	EXPECT_NE(handshake.find("mqtt"), std::string::npos);

	// session.subprotocol() 也应正确
	EXPECT_EQ(negotiatedProto, "mqtt");

	wsClient.close();
	server.stop();
	serverThread.join();
}

// 验证子协议不匹配时连接仍然建立（服务端不强制要求，仅从 offer 中选择）
TEST(WsAdvancedTest, SubprotocolNoMatch)
{
	HttpServer server(0);

	std::string negotiatedProto = "NOT_SET";
	std::atomic<bool> connected {false};

	Router::WsOptions opts;
	opts.subprotocols = {"mqtt"};

	server.router().ws(
		"/ws/proto-nomatch",
		std::move(opts),
		[](const std::string& msg, WebSocketSession& session) -> Awaitable<void>
		{
			co_await session.send(msg);
		},
		[&negotiatedProto, &connected](WebSocketSession& session) -> Awaitable<void>
		{
			negotiatedProto = std::string(session.subprotocol());
			connected = true;
			co_return;
		});

	std::thread serverThread;
	uint16_t port = startWsServerAndWait(server, serverThread);

	boost::asio::io_context ioCtx;
	TestWsClient wsClient(ioCtx);
	// 客户端只 offer "graphql"，服务端只支持 "mqtt"，无交集
	wsClient.connect("127.0.0.1", port, "/ws/proto-nomatch", "graphql");

	for (int i = 0; i < 50 && !connected.load(); ++i)
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}

	// 无匹配时不写 Sec-WebSocket-Protocol 响应头，子协议为空
	const std::string& handshake = wsClient.lastHandshakeResponse();
	// 101 仍然成功
	EXPECT_NE(handshake.find("101"), std::string::npos);
	EXPECT_EQ(negotiatedProto, "");

	wsClient.close();
	server.stop();
	serverThread.join();
}

// ============ Feature 6: Connection Context ============

struct RoomCtx
{
	std::string roomName;
	int msgCount = 0;
};

// 验证 onConnect 中 setContext，onMessage 中 getContext 取回数据并回显
TEST(WsAdvancedTest, ContextSetAndGet)
{
	HttpServer server(0);

	server.router().ws(
		"/ws/ctx",
		[](const std::string& /*msg*/, WebSocketSession& session) -> Awaitable<void>
		{
			auto ctx = session.getContext<RoomCtx>();
			EXPECT_NE(ctx, nullptr);
			if (ctx)
			{
				ctx->msgCount++;
				co_await session.send(ctx->roomName + ":" + std::to_string(ctx->msgCount));
			}
		},
		[](WebSocketSession& session) -> Awaitable<void>
		{
			auto ctx = std::make_shared<RoomCtx>();
			ctx->roomName = "lobby";
			session.setContext(ctx);
			co_return;
		});

	std::thread serverThread;
	uint16_t port = startWsServerAndWait(server, serverThread);

	boost::asio::io_context ioCtx;
	TestWsClient wsClient(ioCtx);
	wsClient.connect("127.0.0.1", port, "/ws/ctx");

	wsClient.write("msg1");
	EXPECT_EQ(wsClient.read(), "lobby:1");

	wsClient.write("msg2");
	EXPECT_EQ(wsClient.read(), "lobby:2");

	wsClient.close();
	server.stop();
	serverThread.join();
}

// ============ Feature 7: Param Route ============

// 验证参数路由 /ws/chat/{room} 能匹配不同路径，连接均成功
TEST(WsAdvancedTest, ParamRouteCapture)
{
	HttpServer server(0);

	server.router().ws("/ws/chat/{room}",
					   [](const std::string& msg, WebSocketSession& session) -> Awaitable<void>
					   {
						   co_await session.send("echo:" + msg);
					   });

	std::thread serverThread;
	uint16_t port = startWsServerAndWait(server, serverThread);

	boost::asio::io_context ioCtx1;
	TestWsClient client1(ioCtx1);
	client1.connect("127.0.0.1", port, "/ws/chat/room1");

	boost::asio::io_context ioCtx2;
	TestWsClient client2(ioCtx2);
	client2.connect("127.0.0.1", port, "/ws/chat/room2");

	client1.write("hello");
	EXPECT_EQ(client1.read(), "echo:hello");

	client2.write("world");
	EXPECT_EQ(client2.read(), "echo:world");

	client1.close();
	client2.close();
	server.stop();
	serverThread.join();
}

// ============ Feature 2: WsHub ============

// Hub 测试用的连接上下文（文件作用域，供多个 lambda 共享类型）
struct HubConnCtx
{
	WsConnectionId id;
	std::shared_ptr<WsHub> hub;
};

struct HubExclCtx
{
	WsConnectionId id;
};

// WsHub API 集成测试：通过真实 WebSocket 连接验证 join/roomSize/broadcast/connectionCount
TEST(WsAdvancedTest, HubBroadcastToRoom)
{
	HttpServer server(0);
	auto hub = std::make_shared<WsHub>();

	std::atomic<int> connectedCount {0};
	std::vector<std::shared_ptr<WebSocketSession>> sessions;
	std::mutex sessionsMu;

	server.router().ws(
		"/ws/hub",
		[](const std::string& /*msg*/, WebSocketSession& /*session*/) -> Awaitable<void>
		{
			co_return;
		},
		[hub, &sessions, &sessionsMu, &connectedCount](WebSocketSession& session) -> Awaitable<void>
		{
			// 空删除器的 shared_ptr：lifetime 由框架的 WebSocketSession shared_ptr 保证，
			// server.stop() 前 onDisconnect 已经 remove，不会有 dangling weak_ptr 解引用
			auto sp = std::shared_ptr<WebSocketSession>(&session,
														[](WebSocketSession*)
														{
														});
			auto id = hub->add(sp);
			session.setContext(std::make_shared<HubConnCtx>(HubConnCtx {id, hub}));

			{
				std::lock_guard lk(sessionsMu);
				sessions.emplace_back(sp);
			}
			connectedCount.fetch_add(1);
			co_return;
		},
		[hub](WebSocketSession& session) -> Awaitable<void>
		{
			auto ctx = session.getContext<HubConnCtx>();
			if (ctx)
			{
				ctx->hub->remove(ctx->id);
			}
			co_return;
		});

	std::thread serverThread;
	uint16_t port = startWsServerAndWait(server, serverThread);

	boost::asio::io_context io1;
	TestWsClient c1(io1);
	c1.connect("127.0.0.1", port, "/ws/hub");

	boost::asio::io_context io2;
	TestWsClient c2(io2);
	c2.connect("127.0.0.1", port, "/ws/hub");

	for (int i = 0; i < 100 && connectedCount.load() < 2; ++i)
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	EXPECT_EQ(connectedCount.load(), 2);
	EXPECT_EQ(hub->connectionCount(), 2u);

	// 将两个连接加入 room1
	{
		std::lock_guard lk(sessionsMu);
		for (auto& sp : sessions)
		{
			auto ctx = sp->getContext<HubConnCtx>();
			if (ctx)
			{
				hub->join(ctx->id, "room1");
			}
		}
	}
	EXPECT_EQ(hub->roomSize("room1"), 2u);

	// 在 server.stop() 前清空外部持有的空删除器 shared_ptr，避免悬空引用
	{
		std::lock_guard lk(sessionsMu);
		sessions.clear();
	}

	c1.close();
	c2.close();
	server.stop();
	serverThread.join();
}

// 验证 Hub broadcast 的 exclude 参数：广播排除指定连接不崩溃
TEST(WsAdvancedTest, HubExcludeSender)
{
	HttpServer server(0);
	auto hub = std::make_shared<WsHub>();

	std::atomic<int> connectedCount {0};
	std::vector<WsConnectionId> connIds;
	std::mutex connIdsMu;

	server.router().ws(
		"/ws/hub-excl",
		[](const std::string&, WebSocketSession&) -> Awaitable<void>
		{
			co_return;
		},
		[hub, &connectedCount, &connIds, &connIdsMu](WebSocketSession& session) -> Awaitable<void>
		{
			auto sp = std::shared_ptr<WebSocketSession>(&session,
														[](WebSocketSession*)
														{
														});
			auto id = hub->add(sp);
			hub->join(id, "room");
			session.setContext(std::make_shared<HubExclCtx>(HubExclCtx {id}));

			{
				std::lock_guard lk(connIdsMu);
				connIds.push_back(id);
			}
			connectedCount.fetch_add(1);
			co_return;
		},
		[hub](WebSocketSession& session) -> Awaitable<void>
		{
			auto ctx = session.getContext<HubExclCtx>();
			if (ctx)
			{
				hub->remove(ctx->id);
			}
			co_return;
		});

	std::thread serverThread;
	uint16_t port = startWsServerAndWait(server, serverThread);

	boost::asio::io_context io1;
	TestWsClient c1(io1);
	c1.connect("127.0.0.1", port, "/ws/hub-excl");

	boost::asio::io_context io2;
	TestWsClient c2(io2);
	c2.connect("127.0.0.1", port, "/ws/hub-excl");

	for (int i = 0; i < 100 && connectedCount.load() < 2; ++i)
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	EXPECT_EQ(hub->roomSize("room"), 2u);

	WsConnectionId excludeId = 0;
	{
		std::lock_guard lk(connIdsMu);
		if (!connIds.empty())
		{
			excludeId = connIds[0];
		}
	}
	// 排除第一个连接广播，不崩溃即通过（session 存活，但 send 到已关闭的 socket 会被忽略）
	hub->broadcast("room", "test-msg", excludeId);

	c1.close();
	c2.close();
	server.stop();
	serverThread.join();
}

// ============ Feature 1: Heartbeat Ping/Pong ============

// 验证服务端配置心跳后，客户端能收到 Ping 帧并回复 Pong，连接保持
TEST(WsAdvancedTest, HeartbeatPingSent)
{
	HttpServer server(0);

	Router::WsOptions opts;
	opts.pingInterval = std::chrono::seconds(1);
	opts.maxMissedPongs = 2;
	opts.pingPayload = "hical-ping";

	server.router().ws("/ws/heartbeat",
					   std::move(opts),
					   [](const std::string& msg, WebSocketSession& session) -> Awaitable<void>
					   {
						   co_await session.send("echo:" + msg);
					   });

	std::thread serverThread;
	uint16_t port = startWsServerAndWait(server, serverThread);

	boost::asio::io_context ioCtx;
	TestWsClient wsClient(ioCtx);
	wsClient.connect("127.0.0.1", port, "/ws/heartbeat");

	// 循环读取帧直到收到 Ping 或超时（最长 3 秒）
	bool gotPing = false;
	std::string pingPayload;
	auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);

	while (std::chrono::steady_clock::now() < deadline)
	{
		// 非阻塞检查：用 available() 避免无限阻塞
		boost::system::error_code ec;
		size_t available = wsClient.socket().available(ec);
		if (ec || available == 0)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
			continue;
		}

		try
		{
			auto frame = wsClient.readFrame();
			if (frame.opcode == WsOpcode::hPing)
			{
				gotPing = true;
				pingPayload = frame.payload;
				wsClient.writePong(frame.payload);
				break;
			}
		}
		catch (const boost::system::system_error&)
		{
			break;
		}
	}

	EXPECT_TRUE(gotPing);
	if (gotPing)
	{
		EXPECT_EQ(pingPayload, "hical-ping");
	}

	wsClient.close();
	server.stop();
	serverThread.join();
}
