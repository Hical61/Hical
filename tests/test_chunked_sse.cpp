#include "core/ChunkedBody.h"
#include "core/HttpResponse.h"
#include "core/HttpTypes.h"
#include "core/Router.h"
#include "core/SseSession.h"
#include <gtest/gtest.h>
#include <string>
#include <string_view>

using namespace hical;

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
