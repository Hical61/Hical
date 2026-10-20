/**
 * @file test_ws_pipeline.cpp
 * @brief 验证 WebSocket 内核「单 read 多帧 drain」能力：一次 TCP 读入多帧后逐条回显，不丢帧
 */

#include "TestHttpClient.h"
#include "core/HttpServer.h"
#include "core/WebSocket.h"
#include <gtest/gtest.h>
#include <atomic>
#include <boost/asio.hpp>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

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

// 等一个原子量被服务端线程写进去（服务端会话收尾是异步的）
static size_t waitForCount(const std::atomic<size_t>& counter, size_t expectAtLeast, int timeoutMs = 3000)
{
	for (int i = 0; i < timeoutMs / 10; ++i)
	{
		size_t v = counter.load();
		if (v >= expectAtLeast)
		{
			return v;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	return counter.load();
}

// 一批里 16 帧只应该落一两次 async_write；修复前是 16 次（一帧一次系统调用）
TEST(WsPipelineTest, EchoBatch_SixteenFrames_OneWritePerBatch)
{
	HttpServer server(0);
	std::atomic<size_t> serverWriteCount {0};

	server.router().ws(
		"/ws/pipeline",
		[](const std::string& msg, WebSocketSession& session) -> Awaitable<void>
		{
			co_await session.send(msg);
		},
		nullptr,
		[&serverWriteCount](WebSocketSession& session) -> Awaitable<void>
		{
			// 会话收尾时记下实际发起的写次数：16 帧一次读进来，就不该写 16 次
			serverWriteCount.store(session.writeCount());
			co_return;
		});

	std::thread serverThread;
	uint16_t port = startWsServerAndWait(server, serverThread);

	boost::asio::io_context ioCtx;
	TestWsClient wsClient(ioCtx);
	wsClient.connect("127.0.0.1", port, "/ws/pipeline");

	static constexpr uint8_t kMaskKey[4] = {0x12, 0x34, 0x56, 0x78};
	std::string allFrames;
	for (int i = 0; i < kFrameCount; ++i)
	{
		allFrames += hical::buildMaskedWsFrame(hical::WsOpcode::hText, "hello", kMaskKey);
	}
	boost::asio::write(wsClient.socket(), boost::asio::buffer(allFrames));

	for (int i = 0; i < kFrameCount; ++i)
	{
		EXPECT_EQ(wsClient.read(), "hello");
	}

	wsClient.close();
	// 收尾那次写是 close 回帧，算上它上限也就是 2 次
	size_t writes = waitForCount(serverWriteCount, 1);
	EXPECT_GT(writes, 0u);
	EXPECT_LE(writes, 2u) << "16 帧一次读进来却写了 " << writes << " 次，合批没生效";

	server.stop();
	serverThread.join();
}

// 数据帧中间插 ping：pong 不能因为合批被吞掉或延后到关闭前，两个类别的相对顺序都要对
TEST(WsPipelineTest, PingMixedWithData_ControlReplyKeptInOrder)
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

	static constexpr uint8_t kMaskKey[4] = {0x12, 0x34, 0x56, 0x78};
	std::string allFrames;
	allFrames += hical::buildMaskedWsFrame(hical::WsOpcode::hText, "a", kMaskKey);
	allFrames += hical::buildMaskedWsFrame(hical::WsOpcode::hPing, "p1", kMaskKey);
	allFrames += hical::buildMaskedWsFrame(hical::WsOpcode::hText, "b", kMaskKey);
	allFrames += hical::buildMaskedWsFrame(hical::WsOpcode::hPing, "p2", kMaskKey);
	boost::asio::write(wsClient.socket(), boost::asio::buffer(allFrames));

	std::vector<std::string> texts;
	std::vector<std::string> pongs;
	while (texts.size() < 2 || pongs.size() < 2)
	{
		auto frame = wsClient.readFrame();
		if (frame.opcode == hical::WsOpcode::hPong)
		{
			pongs.push_back(frame.payload);
		}
		else if (frame.opcode == hical::WsOpcode::hText)
		{
			texts.push_back(frame.payload);
		}
	}

	EXPECT_EQ(texts, (std::vector<std::string> {"a", "b"}));
	EXPECT_EQ(pongs, (std::vector<std::string> {"p1", "p2"}));

	wsClient.close();
	server.stop();
	serverThread.join();
}

// 分片消息中间插 ping：重组照旧，pong 也照回
TEST(WsPipelineTest, FragmentedMessageWithInterleavedPing_StillReassembled)
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

	static constexpr uint8_t kMaskKey[4] = {0x11, 0x22, 0x33, 0x44};
	std::string allFrames;
	// 首帧 FIN=0 → ping 插在中间 → 续帧 FIN=1
	allFrames += hical::buildMaskedWsFrame(hical::WsOpcode::hText, "hel", kMaskKey, false);
	allFrames += hical::buildMaskedWsFrame(hical::WsOpcode::hPing, "mid", kMaskKey);
	allFrames += hical::buildMaskedWsFrame(hical::WsOpcode::hContinuation, "lo", kMaskKey, true);
	boost::asio::write(wsClient.socket(), boost::asio::buffer(allFrames));

	std::string echo;
	std::string pongPayload;
	while (echo.empty() || pongPayload.empty())
	{
		auto frame = wsClient.readFrame();
		if (frame.opcode == hical::WsOpcode::hText)
		{
			echo = frame.payload;
		}
		else if (frame.opcode == hical::WsOpcode::hPong)
		{
			pongPayload = frame.payload;
		}
	}

	EXPECT_EQ(echo, "hello");
	EXPECT_EQ(pongPayload, "mid");

	wsClient.close();
	server.stop();
	serverThread.join();
}

// 一批回帧累积超过出站缓冲上限会自动中途落盘：结果顺序和内容仍然不能乱
TEST(WsPipelineTest, LargeBurstAboveFlushThreshold_AllEchoedInOrder)
{
	static constexpr int kBigFrameCount = 32;
	static constexpr size_t kBigPayload = 10000;

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

	static constexpr uint8_t kMaskKey[4] = {0x55, 0x66, 0x77, 0x88};
	std::vector<std::string> payloads;
	payloads.reserve(kBigFrameCount);
	std::string allFrames;
	for (int i = 0; i < kBigFrameCount; ++i)
	{
		payloads.push_back(std::string(kBigPayload, static_cast<char>('a' + i % 26)));
		allFrames += hical::buildMaskedWsFrame(hical::WsOpcode::hText, payloads.back(), kMaskKey);
	}
	boost::asio::write(wsClient.socket(), boost::asio::buffer(allFrames));

	for (int i = 0; i < kBigFrameCount; ++i)
	{
		EXPECT_EQ(wsClient.read(), payloads[static_cast<size_t>(i)]) << "第 " << i << " 条回显对不上";
	}

	wsClient.close();
	server.stop();
	serverThread.join();
}

// 一次灌进来的帧数超过每批解帧上限：解到上限先交一批出去，剩下的字节还得留在
// 读缓冲里等下一轮接着解。少解一条、顺序错位、把一帧劈成两半都会在这儿露出来
TEST(WsPipelineTest, BurstAboveFrameLimit_AllEchoedInOrder)
{
	// 得比 kMaxFramesPerBatch 多，才压得到「解够一批就提前返回」那条路
	static constexpr int kBurstFrames = 300;
	static_assert(kBurstFrames > static_cast<int>(WebSocketSession::kMaxFramesPerBatch),
				  "爆发帧数没超过每批上限，压不到提前返回");

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

	static constexpr uint8_t kMaskKey[4] = {0x21, 0x43, 0x65, 0x87};
	std::vector<std::string> payloads;
	payloads.reserve(kBurstFrames);
	std::string allFrames;
	for (int i = 0; i < kBurstFrames; ++i)
	{
		payloads.push_back("m" + std::to_string(i));
		allFrames += hical::buildMaskedWsFrame(hical::WsOpcode::hText, payloads.back(), kMaskKey);
	}
	boost::asio::write(wsClient.socket(), boost::asio::buffer(allFrames));

	// 回读走非阻塞轮询 + deadline 兜底，真丢一帧也不会把整条用例挂死
	std::vector<std::string> replies;
	std::string readBuf;
	char tmp[4096];
	wsClient.socket().non_blocking(true);
	auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	constexpr size_t kWantReplies = static_cast<size_t>(kBurstFrames);

	while (replies.size() < kWantReplies && std::chrono::steady_clock::now() < deadline)
	{
		// 手里能解几帧解几帧，解不出完整帧再去读
		for (;;)
		{
			auto hdr = hical::parseWsFrameHeader(reinterpret_cast<const uint8_t*>(readBuf.data()), readBuf.size());
			if (!hdr)
			{
				break;
			}
			const size_t frameSize = hdr->headerSize + static_cast<size_t>(hdr->payloadLength);
			if (readBuf.size() < frameSize)
			{
				break;
			}
			replies.emplace_back(readBuf.data() + hdr->headerSize, static_cast<size_t>(hdr->payloadLength));
			readBuf.erase(0, frameSize);
		}
		if (replies.size() >= kWantReplies)
		{
			break;
		}

		boost::system::error_code readEc;
		const auto n = wsClient.socket().read_some(boost::asio::buffer(tmp), readEc);
		if (!readEc)
		{
			readBuf.append(tmp, n);
		}
		else if (readEc == boost::asio::error::would_block)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		else
		{
			break; // EOF / 连接重置，条数对不上下面会报
		}
	}

	wsClient.socket().non_blocking(false);
	wsClient.close();
	// 先收拾干净再断言：ASSERT 失败直接就 return 了，服务端线程不能晾着
	server.stop();
	serverThread.join();

	ASSERT_EQ(replies.size(), kWantReplies) << "回显条数对不上，丢帧或提前断流了";
	for (size_t i = 0; i < replies.size(); ++i)
	{
		EXPECT_EQ(replies[i], payloads[i]) << "第 " << i << " 条回显对不上";
	}
}

// 一批里跟着 close：前面几条的响应必须先发出去，close 回帧排在最后
TEST(WsPipelineTest, CloseArrivesInSameBatch_PendingRepliesFlushedFirst)
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

	static constexpr uint8_t kMaskKey[4] = {0x12, 0x34, 0x56, 0x78};
	std::string allFrames;
	allFrames += hical::buildMaskedWsFrame(hical::WsOpcode::hText, "a", kMaskKey);
	allFrames += hical::buildMaskedWsFrame(hical::WsOpcode::hText, "b", kMaskKey);
	auto closePayload = hical::buildClosePayload(hical::WsCloseCode::hNormal);
	allFrames += hical::buildMaskedWsFrame(hical::WsOpcode::hClose, closePayload, kMaskKey);
	boost::asio::write(wsClient.socket(), boost::asio::buffer(allFrames));

	auto first = wsClient.readFrame();
	EXPECT_EQ(first.opcode, hical::WsOpcode::hText);
	EXPECT_EQ(first.payload, "a");

	auto second = wsClient.readFrame();
	EXPECT_EQ(second.opcode, hical::WsOpcode::hText);
	EXPECT_EQ(second.payload, "b");

	auto third = wsClient.readFrame();
	EXPECT_EQ(third.opcode, hical::WsOpcode::hClose) << "close 回帧抢在数据响应前面发出去了";

	server.stop();
	serverThread.join();
}
