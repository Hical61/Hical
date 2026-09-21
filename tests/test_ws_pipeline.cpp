/**
 * @file test_ws_pipeline.cpp
 * @brief 验证 WebSocket 内核「单 read 多帧 drain」能力：一次 TCP 读入多帧后逐条回显，不丢帧
 */

#include "TestHttpClient.h"
#include "core/HttpServer.h"
#include "core/WebSocket.h"
#include <gtest/gtest.h>
#include <boost/asio.hpp>
#include <cstdint>
#include <string>
#include <thread>

using namespace hical;
using hical::test::TestWsClient;

// 一次 write 发多少帧（确保超出单次 TCP read 的典型缓冲粒度，可靠触发多帧 drain 路径）
static constexpr int kFrameCount = 16;

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

// 把 kFrameCount 个文本帧拼成一个连续字节串，一次 TCP write 全部发出。
// 服务端内核一次 async_read_some 可能读到多帧，echo 也必须逐条回显，验证无丢帧。
TEST(WsPipelineTest, DrainMultipleFramesInSingleRead)
{
	HttpServer server(0);

	server.router().ws("/ws/pipeline",
					   [](const std::string& msg, WebSocketSession& session) -> Awaitable<void>
					   {
						   co_await session.send(msg);
					   });

	std::thread serverThread;
	uint16_t port = startWsServerAndWait(server, serverThread);

	boost::asio::io_context ioCtx;
	TestWsClient wsClient(ioCtx);
	wsClient.connect("127.0.0.1", port, "/ws/pipeline");

	// 客户端帧必须 mask；拼 N 帧到单个 buffer，随后一次性写入 socket
	static constexpr uint8_t kMaskKey[4] = {0x12, 0x34, 0x56, 0x78};
	constexpr std::string_view kPayload = "hello";
	std::string allFrames;
	for (int i = 0; i < kFrameCount; ++i)
	{
		allFrames += hical::buildMaskedWsFrame(hical::WsOpcode::hText, kPayload, kMaskKey);
	}
	boost::asio::write(wsClient.socket(), boost::asio::buffer(allFrames));

	// 连续回读，逐条校验数量与内容，顺序也必须一致
	for (int i = 0; i < kFrameCount; ++i)
	{
		std::string reply = wsClient.read();
		EXPECT_EQ(reply, "hello");
	}

	wsClient.close();
	server.stop();
	serverThread.join();
}
