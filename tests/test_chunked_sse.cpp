/**
 * @file test_chunked_sse.cpp
 * @brief Chunked Transfer-Encoding + SSE 测试
 */

#include "TestHttpClient.h"
#include "core/ChunkedBody.h"
#include "core/HttpResponse.h"
#include "core/HttpServer.h"
#include "core/HttpTypes.h"
#include "core/Router.h"
#include "core/SseSession.h"
#include <boost/asio.hpp>
#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <string>
#include <string_view>
#include <thread>

using namespace hical;
using hical::test::httpGet;
using hical::test::detail::ParsedResponse;

using boost::asio::ip::tcp;

// ============ ChunkedBody 编码测试 ============

TEST(ChunkedBodyTest, SingleChunk)
{
	auto frame = serializeChunkFrame("Hello");
	EXPECT_EQ(frame, "5\r\nHello\r\n");
}

TEST(ChunkedBodyTest, MultipleChunks)
{
	auto frame1 = serializeChunkFrame("Hello");
	auto frame2 = serializeChunkFrame(" World");
	EXPECT_EQ(frame1, "5\r\nHello\r\n");
	EXPECT_EQ(frame2, "6\r\n World\r\n");
}

TEST(ChunkedBodyTest, EmptyChunk)
{
	auto frame = serializeChunkFrame("");
	EXPECT_EQ(frame, "0\r\n\r\n");
}

TEST(ChunkedBodyTest, TrailerFrame)
{
	auto frame = serializeTrailerFrame();
	EXPECT_EQ(frame, "0\r\n\r\n");
}

TEST(ChunkedBodyTest, LargeChunkSize)
{
	// 256 bytes — hex = 100
	std::string data(256, 'A');
	auto frame = serializeChunkFrame(data);
	// frame = "100\r\n" (5B) + data (256B) + "\r\n" (2B) = 263B
	EXPECT_EQ(frame.substr(0, 5), "100\r\n");
	EXPECT_EQ(frame.substr(5, 256), data);
	EXPECT_EQ(frame.substr(5 + 256), "\r\n");
}

TEST(ChunkedBodyTest, ChunkedBodyCollectWrite)
{
	ChunkedBody body;
	body.write("Hello");
	body.write(" World");
	body.end();

	EXPECT_TRUE(body.finished());
	ASSERT_EQ(body.chunks().size(), 2u);
	EXPECT_EQ(body.chunks()[0], "Hello");
	EXPECT_EQ(body.chunks()[1], " World");
}

TEST(ChunkedBodyTest, ChunkedBodyCollectSingleWrite)
{
	ChunkedBody body;
	body.write("test");
	body.end();

	EXPECT_TRUE(body.finished());
	ASSERT_EQ(body.chunks().size(), 1u);
	EXPECT_EQ(body.chunks()[0], "test");
}

TEST(ChunkedBodyTest, WriteAfterEnd_NoOp)
{
	ChunkedBody body;
	body.end();
	// 写入已结束的 body 不应增加 chunk
	body.write("should not appear");
	ASSERT_EQ(body.chunks().size(), 0u);
	EXPECT_TRUE(body.finished());
}

// ============ NativeResponse chunked 标记测试 ============

TEST(ChunkedBodyTest, ResponseHasChunkedBody)
{
	NativeResponse res;
	EXPECT_FALSE(res.hasChunkedBody());

	res.chunkedBody.emplace();
	EXPECT_TRUE(res.hasChunkedBody());
}

TEST(ChunkedBodyTest, PreparePayload_SetsChunkedEncoding)
{
	NativeResponse res;
	res.chunkedBody.emplace();
	res.chunkedBody->write("hello");
	res.chunkedBody->end();
	res.preparePayload();

	// 有 chunkedBody 时不应设 Content-Length
	auto cl = res.headers.find("Content-Length");
	EXPECT_TRUE(cl.empty());

	// 应设 Transfer-Encoding: chunked
	auto te = res.headers.find("Transfer-Encoding");
	EXPECT_EQ(te, "chunked");
}

TEST(ChunkedBodyTest, PreparePayload_NoChunkedBody_StillSetsContentLength)
{
	NativeResponse res;
	res.body = "hello";
	res.preparePayload();
	auto cl = res.headers.find("Content-Length");
	EXPECT_EQ(cl, "5");
}

// ============ HttpResponse chunked 工厂测试 ============

TEST(ChunkedBodyTest, HttpResponseChunkedFactory)
{
	auto res = HttpResponse::chunked();
	EXPECT_TRUE(res.native().hasChunkedBody());
	EXPECT_EQ(res.statusCode(), HttpStatusCode::hOk);
}

TEST(ChunkedBodyTest, HttpResponseChunkedBodyAccessor)
{
	auto res = HttpResponse::chunked();
	auto& body = res.chunkedBody();
	body.write("data");
	body.end();

	ASSERT_EQ(body.chunks().size(), 1u);
	EXPECT_EQ(body.chunks()[0], "data");
	EXPECT_TRUE(body.finished());
}

// ============ ChunkedBody 整体序列化 ============

TEST(ChunkedBodyTest, SerializeFullChunkedBody)
{
	ChunkedBody body;
	body.write("Hello");
	body.write(" World");
	body.end();

	auto wire = serializeChunkedBody(body);
	EXPECT_EQ(wire, "5\r\nHello\r\n6\r\n World\r\n0\r\n\r\n");
}

TEST(ChunkedBodyTest, SerializeEmptyChunkedBody)
{
	ChunkedBody body;
	body.end();

	auto wire = serializeChunkedBody(body);
	EXPECT_EQ(wire, "0\r\n\r\n");
}

TEST(SseRouterTest, RegisterAndMatchStatic)
{
	Router router;
	router.sse("/events",
			   [](std::shared_ptr<SseSession>) -> Awaitable<void>
			   {
				   co_return;
			   });
	auto match = router.findSseRoute("/events");
	ASSERT_NE(match.route, nullptr);
	EXPECT_EQ(match.route->path, "/events");
	EXPECT_TRUE(match.params.empty());
	auto noMatch = router.findSseRoute("/other");
	EXPECT_EQ(noMatch.route, nullptr);
}

TEST(SseRouterTest, RegisterAndMatchParam)
{
	Router router;
	router.sse("/events/{id}",
			   [](std::shared_ptr<SseSession>) -> Awaitable<void>
			   {
				   co_return;
			   });
	auto match = router.findSseRoute("/events/42");
	ASSERT_NE(match.route, nullptr);
	ASSERT_EQ(match.params.size(), 1u);
	EXPECT_EQ(match.params[0].first, "id");
	EXPECT_EQ(match.params[0].second, "42");
}

TEST(SseRouterTest, RouteCount)
{
	Router router;
	EXPECT_EQ(router.routeCount(), 0u);
	router.sse("/events",
			   [](std::shared_ptr<SseSession>) -> Awaitable<void>
			   {
				   co_return;
			   });
	EXPECT_EQ(router.routeCount(), 1u);
	router.get("/health",
			   [](const HttpRequest&) -> Awaitable<HttpResponse>
			   {
				   co_return HttpResponse::ok();
			   });
	EXPECT_EQ(router.routeCount(), 2u);
}

// ============ HttpServer 集成测试 ============

/// 启动服务器并等待就绪，返回实际端口
static uint16_t startSseServerAndWait(HttpServer& server, std::thread& serverThread)
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
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	EXPECT_NE(port, 0u) << "Server failed to start";
	return port;
}

TEST(ChunkedIntegrationTest, ChunkedResponseFromServer)
{
	HttpServer server(0);
	server.setIdleTimeout(0);

	server.router().get("/chunked",
						[](const HttpRequest&) -> Awaitable<HttpResponse>
						{
							auto res = HttpResponse::chunked();
							auto& body = res.chunkedBody();
							body.write("Hello");
							body.write(" World");
							body.end();
							co_return res;
						});

	std::thread serverThread;
	uint16_t port = startSseServerAndWait(server, serverThread);

	// 用 raw socket 读取，手动验证 chunked 编码
	boost::asio::io_context ioc;
	tcp::socket sock(ioc);
	sock.connect(tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), port));

	std::string req = "GET /chunked HTTP/1.1\r\n"
					  "Host: 127.0.0.1\r\n"
					  "Connection: close\r\n"
					  "\r\n";
	boost::asio::write(sock, boost::asio::buffer(req));

	std::string raw;
	char buf[4096];
	boost::system::error_code ec;
	while (true)
	{
		auto n = sock.read_some(boost::asio::buffer(buf), ec);
		if (ec)
		{
			break;
		}
		raw.append(buf, n);
	}
	sock.close();

	EXPECT_NE(raw.find("200 OK"), std::string::npos);
	EXPECT_NE(raw.find("Transfer-Encoding: chunked"), std::string::npos);
	EXPECT_NE(raw.find("5\r\nHello\r\n6\r\n World\r\n0\r\n\r\n"), std::string::npos);

	server.stop();
	serverThread.join();
}

TEST(ChunkedIntegrationTest, ChunkedResponseHasChunkedEncoding)
{
	HttpServer server(0);
	server.setIdleTimeout(0);

	server.router().get("/multi",
						[](const HttpRequest&) -> Awaitable<HttpResponse>
						{
							auto res = HttpResponse::chunked();
							auto& body = res.chunkedBody();
							body.write("Hello");
							body.write(" ");
							body.write("World");
							body.end();
							co_return res;
						});

	std::thread serverThread;
	uint16_t port = startSseServerAndWait(server, serverThread);

	// 用 raw socket 确认 Transfer-Encoding 头
	boost::asio::io_context ioc;
	tcp::socket sock(ioc);
	sock.connect(tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), port));

	std::string req = "GET /multi HTTP/1.1\r\n"
					  "Host: 127.0.0.1\r\n"
					  "Connection: close\r\n"
					  "\r\n";
	boost::asio::write(sock, boost::asio::buffer(req));

	std::string raw;
	char buf[4096];
	boost::system::error_code ec;
	while (true)
	{
		auto n = sock.read_some(boost::asio::buffer(buf), ec);
		if (ec)
		{
			break;
		}
		raw.append(buf, n);
	}
	sock.close();

	EXPECT_NE(raw.find("Transfer-Encoding: chunked"), std::string::npos);

	// 验证 chunked body wire 格式
	auto headerEnd = raw.find("\r\n\r\n");
	ASSERT_NE(headerEnd, std::string::npos);
	auto bodyPart = raw.substr(headerEnd + 4);

	EXPECT_EQ(bodyPart, "5\r\nHello\r\n1\r\n \r\n5\r\nWorld\r\n0\r\n\r\n");

	server.stop();
	serverThread.join();
}

TEST(SseIntegrationTest, SseConnectionAndEvents)
{
	// 起服务器
	HttpServer server(0);
	server.setIdleTimeout(0);

	std::atomic<int> eventCount {0};
	server.router().sse("/events",
						[&eventCount](std::shared_ptr<SseSession> session) -> Awaitable<void>
						{
							co_await session->sendData("hello from SSE");
							eventCount.fetch_add(1);
							co_await session->close();
						});

	std::thread serverThread;
	uint16_t port = startSseServerAndWait(server, serverThread);

	// 连接并发送请求
	boost::asio::io_context ioc;
	tcp::socket sock(ioc);
	sock.connect(tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), port));

	std::string req = "GET /events HTTP/1.1\r\n"
					  "Host: 127.0.0.1\r\n"
					  "Connection: close\r\n"
					  "\r\n";
	boost::asio::write(sock, boost::asio::buffer(req));

	// 阻塞式读取直到服务器 close（服务器 close 表示 onConnect 已完成）
	std::string raw;
	char buf[4096];
	sock.non_blocking(false);
	boost::system::error_code ec;
	while (true)
	{
		auto n = sock.read_some(boost::asio::buffer(buf), ec);
		if (ec)
		{
			break; // EOF/connection_reset — 服务器已完成回调
		}
		raw.append(buf, n);
	}
	sock.close();

	// 验证响应头
	EXPECT_NE(raw.find("200 OK"), std::string::npos) << "raw: " << raw;
	EXPECT_NE(raw.find("Content-Type: text/event-stream"), std::string::npos);
	EXPECT_NE(raw.find("Transfer-Encoding: chunked"), std::string::npos);

	// 验证回调被执行了
	EXPECT_EQ(eventCount.load(), 1);

	server.stop();
	serverThread.join();
}
