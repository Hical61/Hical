#include "core/HttpTypes.h"
#include "core/InetAddress.h"
#include "core/HttpRequest.h"
#include "core/HttpResponse.h"
#include <gtest/gtest.h>

#include <string_view>
#include <utility>
#include <vector>

using namespace hical;

// ============ HttpTypes 测试 ============

TEST(HttpTypesTest, MethodToString)
{
	EXPECT_STREQ(httpMethodToString(HttpMethod::hGet), "GET");
	EXPECT_STREQ(httpMethodToString(HttpMethod::hPost), "POST");
	EXPECT_STREQ(httpMethodToString(HttpMethod::hPut), "PUT");
	EXPECT_STREQ(httpMethodToString(HttpMethod::hDelete), "DELETE");
	EXPECT_STREQ(httpMethodToString(HttpMethod::hPatch), "PATCH");
	EXPECT_STREQ(httpMethodToString(HttpMethod::hHead), "HEAD");
	EXPECT_STREQ(httpMethodToString(HttpMethod::hOptions), "OPTIONS");
	EXPECT_STREQ(httpMethodToString(HttpMethod::hUnknown), "UNKNOWN");
}

TEST(HttpTypesTest, StringToMethod)
{
	EXPECT_EQ(stringToHttpMethod("GET"), HttpMethod::hGet);
	EXPECT_EQ(stringToHttpMethod("POST"), HttpMethod::hPost);
	EXPECT_EQ(stringToHttpMethod("PUT"), HttpMethod::hPut);
	EXPECT_EQ(stringToHttpMethod("DELETE"), HttpMethod::hDelete);
	EXPECT_EQ(stringToHttpMethod("PATCH"), HttpMethod::hPatch);
	EXPECT_EQ(stringToHttpMethod("HEAD"), HttpMethod::hHead);
	EXPECT_EQ(stringToHttpMethod("OPTIONS"), HttpMethod::hOptions);
	EXPECT_EQ(stringToHttpMethod("INVALID"), HttpMethod::hUnknown);
}

TEST(HttpTypesTest, StatusCodeToString)
{
	EXPECT_STREQ(httpStatusCodeToString(HttpStatusCode::hOk), "OK");
	EXPECT_STREQ(httpStatusCodeToString(HttpStatusCode::hNotFound), "Not Found");
	EXPECT_STREQ(httpStatusCodeToString(HttpStatusCode::hInternalServerError), "Internal Server Error");
	EXPECT_STREQ(httpStatusCodeToString(HttpStatusCode::hBadRequest), "Bad Request");
}

// ============ InetAddress 测试 ============

TEST(InetAddressTest, IsValid_DefaultConstructed_ReturnsFalse)
{
	InetAddress addr;
	EXPECT_FALSE(addr.isValid());
}

TEST(InetAddressTest, IsValid_Ipv4_ReturnsTrue)
{
	InetAddress addr("127.0.0.1", 8080);
	EXPECT_TRUE(addr.isValid());
}

TEST(InetAddressTest, IsValid_Ipv6_ReturnsTrue)
{
	InetAddress addr("::1", 8080);
	EXPECT_TRUE(addr.isValid());
}

// ============ HttpRequest 测试 ============

TEST(HttpRequestTest, DefaultConstruction)
{
	HttpRequest req;
	EXPECT_EQ(req.body(), "");
}

TEST(HttpRequestTest, PeerAddr_DefaultConstruction_Invalid)
{
	HttpRequest req;
	EXPECT_FALSE(req.peerAddr().isValid());
}

TEST(HttpRequestTest, SetPeerAddr)
{
	HttpRequest req;
	req.setPeerAddr(InetAddress("10.0.0.1", 1234));
	EXPECT_EQ(req.peerAddr().toIp(), "10.0.0.1");
	EXPECT_EQ(req.peerAddr().port(), 1234);
}

TEST(HttpRequestTest, SetAndGetMethod)
{
	HttpRequest req;
	req.setMethod(HttpMethod::hGet);
	EXPECT_EQ(req.method(), HttpMethod::hGet);

	req.setMethod(HttpMethod::hPost);
	EXPECT_EQ(req.method(), HttpMethod::hPost);
}

TEST(HttpRequestTest, SetAndGetTarget)
{
	HttpRequest req;
	req.setTarget("/api/users?page=1");

	EXPECT_EQ(req.target(), "/api/users?page=1");
	EXPECT_EQ(req.path(), "/api/users");
	EXPECT_EQ(req.query(), "page=1");
}

TEST(HttpRequestTest, PathWithoutQuery)
{
	HttpRequest req;
	req.setTarget("/api/users");

	EXPECT_EQ(req.path(), "/api/users");
	EXPECT_EQ(req.query(), "");
}

TEST(HttpRequestTest, SetAndGetHeader)
{
	HttpRequest req;
	req.setHeader("Content-Type", "application/json");
	EXPECT_EQ(req.header("Content-Type"), "application/json");
	EXPECT_EQ(req.contentType(), "application/json");
}

TEST(HttpRequestTest, SetAndGetBody)
{
	HttpRequest req;
	req.setBody("{\"name\": \"hical\"}");
	EXPECT_EQ(req.body(), "{\"name\": \"hical\"}");
}

TEST(HttpRequestTest, JsonBody)
{
	HttpRequest req;
	req.setBody("{\"key\": \"value\"}");

	auto json = req.jsonBody();
	EXPECT_EQ(json.at("key").as_string(), "value");
}

TEST(HttpRequestTest, MissingHeader)
{
	HttpRequest req;
	EXPECT_EQ(req.header("X-Custom"), "");
}

TEST(HttpRequestTest, FromNativeRequest)
{
	NativeRequest nativeReq;
	nativeReq.method = HttpMethod::hGet;
	nativeReq.target = "/test";
	nativeReq.httpVersionMajor = 1;
	nativeReq.httpVersionMinor = 1;
	nativeReq.headers.add("Host", "localhost");

	HttpRequest req = HttpRequest::fromParsed(std::move(nativeReq));
	EXPECT_EQ(req.method(), HttpMethod::hGet);
	EXPECT_EQ(req.path(), "/test");
}

// ============ NativeRequest::isUpgrade 测试 ============

TEST(HttpRequestTest, IsUpgrade_NoConnectionHeader_ReturnsFalse)
{
	NativeRequest nativeReq;
	EXPECT_FALSE(nativeReq.isUpgrade());
}

TEST(HttpRequestTest, IsUpgrade_KeepAliveOnly_ReturnsFalse)
{
	NativeRequest nativeReq;
	nativeReq.headers.add("Connection", "keep-alive");
	nativeReq.headers.add("Upgrade", "websocket");
	EXPECT_FALSE(nativeReq.isUpgrade());
}

TEST(HttpRequestTest, IsUpgrade_ConnectionUpgradeWithWebsocket_ReturnsTrue)
{
	NativeRequest nativeReq;
	nativeReq.headers.add("Connection", "Upgrade");
	nativeReq.headers.add("Upgrade", "websocket");
	EXPECT_TRUE(nativeReq.isUpgrade());
}

TEST(HttpRequestTest, IsUpgrade_CommaSeparatedUpgrade_ReturnsTrue)
{
	NativeRequest nativeReq;
	nativeReq.headers.add("Connection", "keep-alive, Upgrade");
	nativeReq.headers.add("Upgrade", "websocket");
	EXPECT_TRUE(nativeReq.isUpgrade());
}

TEST(HttpRequestTest, IsUpgrade_CaseInsensitive_ReturnsTrue)
{
	NativeRequest nativeReq;
	nativeReq.headers.add("Connection", "UPGRADE");
	nativeReq.headers.add("Upgrade", "WebSocket");
	EXPECT_TRUE(nativeReq.isUpgrade());
}

TEST(HttpRequestTest, IsUpgrade_ConnectionUpgradeNoUpgradeHeader_ReturnsFalse)
{
	NativeRequest nativeReq;
	nativeReq.headers.add("Connection", "Upgrade");
	EXPECT_FALSE(nativeReq.isUpgrade());
}

TEST(HttpRequestTest, IsUpgrade_NonWebsocketUpgrade_ReturnsFalse)
{
	NativeRequest nativeReq;
	nativeReq.headers.add("Connection", "Upgrade");
	nativeReq.headers.add("Upgrade", "h2c");
	EXPECT_FALSE(nativeReq.isUpgrade());
}

TEST(HttpRequestTest, IsUpgrade_ValueWithSurroundingSpaces_ReturnsTrue)
{
	NativeRequest nativeReq;
	nativeReq.headers.add("Connection", "  Upgrade ");
	nativeReq.headers.add("Upgrade", "websocket");
	EXPECT_TRUE(nativeReq.isUpgrade());
}

// ============ isUpgrade 预扫标志路径测试 ============

// 预扫置 true 且带 Upgrade: websocket，即使没有 Connection 头也该判 true
TEST(HttpRequestTest, IsUpgrade_PreScannedTrue_UsesFlagWithoutConnectionHeader)
{
	NativeRequest nativeReq;
	nativeReq.upgradePreScanned = true;
	nativeReq.headers.add("Upgrade", "websocket");
	EXPECT_TRUE(nativeReq.isUpgrade());
}

// 预扫置 true 但 Upgrade 头不是 websocket，仍然 false（Upgrade 那一步照查）
TEST(HttpRequestTest, IsUpgrade_PreScannedTrue_NoUpgradeHeader_ReturnsFalse)
{
	NativeRequest nativeReq;
	nativeReq.upgradePreScanned = true;
	EXPECT_FALSE(nativeReq.isUpgrade());
}

// 预扫置 false 就以标志为准，Connection 头里写 Upgrade 也不再回头扫
TEST(HttpRequestTest, IsUpgrade_PreScannedFalse_IgnoresConnectionToken)
{
	NativeRequest nativeReq;
	nativeReq.upgradePreScanned = false;
	nativeReq.headers.add("Connection", "Upgrade");
	nativeReq.headers.add("Upgrade", "websocket");
	EXPECT_FALSE(nativeReq.isUpgrade());
}

// ============ RequestHeaders 测试 ============

TEST(HttpRequestTest, RequestHeaders_DefaultConstructed_IsEmpty)
{
	RequestHeaders h;
	EXPECT_EQ(h.size(), 0u);
	EXPECT_TRUE(h.find("anything").empty());
	EXPECT_FALSE(h.contains("anything"));
	EXPECT_EQ(h.begin(), h.end());
}

TEST(HttpRequestTest, RequestHeaders_Add_StoresNameAndValue)
{
	RequestHeaders h;
	EXPECT_EQ(h.size(), 0u);

	h.add("Host", "example.com");
	EXPECT_EQ(h.size(), 1u);

	h.add("Accept", "text/html");
	EXPECT_EQ(h.size(), 2u);

	// find 命中，值取回
	EXPECT_EQ(h.find("Host"), "example.com");
	EXPECT_EQ(h.find("Accept"), "text/html");
	EXPECT_TRUE(h.contains("Host"));
	EXPECT_FALSE(h.contains("Cookie"));

	// operator[] 按插入顺序索引
	EXPECT_EQ(h[0].name, "Host");
	EXPECT_EQ(h[0].value, "example.com");
	EXPECT_EQ(h[1].name, "Accept");
	EXPECT_EQ(h[1].value, "text/html");
}

TEST(HttpRequestTest, RequestHeaders_Find_IsCaseInsensitive)
{
	RequestHeaders h;
	h.add("Content-Type", "application/json");

	EXPECT_EQ(h.find("content-type"), "application/json");
	EXPECT_EQ(h.find("CONTENT-TYPE"), "application/json");
	EXPECT_TRUE(h.contains("Content-type"));
	EXPECT_TRUE(h.contains("cOnTeNt-TyPe"));
	EXPECT_FALSE(h.contains("content-length"));
	EXPECT_FALSE(h.contains("content-typ")); // 长度不同，不匹配
}

TEST(HttpRequestTest, RequestHeaders_Find_ReturnsFirstMatch)
{
	RequestHeaders h;
	h.add("Set-Cookie", "a=1");
	h.add("Set-Cookie", "b=2");

	EXPECT_EQ(h.size(), 2u);
	// 同名多条时 find 只返回第一条
	EXPECT_EQ(h.find("set-cookie"), "a=1");
	EXPECT_TRUE(h.contains("Set-Cookie"));
	// 遍历仍能看到两条
	EXPECT_EQ(h[0].value, "a=1");
	EXPECT_EQ(h[1].value, "b=2");
}

TEST(HttpRequestTest, RequestHeaders_Iteration_PreservesInsertionOrder)
{
	RequestHeaders h;
	h.add("A", "1");
	h.add("B", "2");
	h.add("C", "3");

	std::vector<std::pair<std::string_view, std::string_view>> seen;
	for (const auto& e : h)
	{
		seen.emplace_back(e.name, e.value);
	}

	ASSERT_EQ(seen.size(), 3u);
	EXPECT_EQ(seen[0].first, "A");
	EXPECT_EQ(seen[1].first, "B");
	EXPECT_EQ(seen[2].first, "C");
	EXPECT_EQ(seen[0].second, "1");
	EXPECT_EQ(seen[1].second, "2");
	EXPECT_EQ(seen[2].second, "3");

	// begin()/end() 遍历出的条数应与 size() 一致
	EXPECT_EQ(static_cast<size_t>(h.end() - h.begin()), h.size());
}

TEST(HttpRequestTest, RequestHeaders_EmptyValue_ContainsButFindEmpty)
{
	RequestHeaders h;
	h.add("X-Empty", "");

	EXPECT_EQ(h.size(), 1u);
	EXPECT_TRUE(h.contains("X-Empty"));     // 条目存在
	EXPECT_TRUE(h.find("X-Empty").empty()); // 但值本身为空
}

TEST(HttpRequestTest, RequestHeaders_Add_AtCapacity_SilentlyDropsOverflow)
{
	RequestHeaders h;
	for (size_t i = 0; i < RequestHeaders::kMaxHeaders; ++i)
	{
		h.add("X-H", "v");
	}
	EXPECT_EQ(h.size(), RequestHeaders::kMaxHeaders);

	// 超出上限的头静默丢弃，不报错也不扩容
	h.add("X-Overflow", "dropped");
	EXPECT_EQ(h.size(), RequestHeaders::kMaxHeaders);
	EXPECT_FALSE(h.contains("X-Overflow"));
}

TEST(HttpRequestTest, RequestHeaders_Clear_ResetsThenReusable)
{
	RequestHeaders h;
	h.add("Host", "example.com");
	h.add("Accept", "text/html");
	ASSERT_EQ(h.size(), 2u);

	h.clear();
	EXPECT_EQ(h.size(), 0u);
	EXPECT_TRUE(h.find("Host").empty());
	EXPECT_FALSE(h.contains("Host"));
	EXPECT_EQ(h.begin(), h.end());

	// clear 之后还能继续 add，从索引 0 重新开始
	h.add("X-New", "v");
	EXPECT_EQ(h.size(), 1u);
	EXPECT_EQ(h[0].name, "X-New");
	EXPECT_EQ(h.find("X-New"), "v");
}

TEST(HttpRequestTest, RequestHeaders_MoveConstruction_TransfersData)
{
	RequestHeaders src;
	src.add("Host", "example.com");
	src.add("Accept", "text/html");
	src.add("X-Custom", "abc");

	RequestHeaders dst(std::move(src));

	// 目标完整拿到数据，顺序不变
	EXPECT_EQ(dst.size(), 3u);
	EXPECT_EQ(dst.find("Host"), "example.com");
	EXPECT_EQ(dst.find("accept"), "text/html");
	EXPECT_EQ(dst[2].name, "X-Custom");
	EXPECT_EQ(dst[2].value, "abc");

	// RequestHeaders 用的是自定义 move：只搬前 size_ 个有效元素（string_view 移动本就是
	// 拷指针+长度），源对象原样保留，size_ 不变，move 之后还能接着读源。
	EXPECT_EQ(src.size(), 3u);
	EXPECT_EQ(src.find("Host"), "example.com");
	EXPECT_TRUE(src.contains("Accept"));
}

TEST(HttpRequestTest, RequestHeaders_MoveAssignment_OverwritesTarget)
{
	RequestHeaders src;
	src.add("Host", "example.com");
	src.add("Accept", "text/html");

	RequestHeaders dst;
	dst.add("Old", "gone");
	ASSERT_EQ(dst.size(), 1u);

	dst = std::move(src);

	// 目标被源覆盖：旧内容消失，新内容就位
	EXPECT_EQ(dst.size(), 2u);
	EXPECT_EQ(dst.find("Host"), "example.com");
	EXPECT_EQ(dst.find("Accept"), "text/html");
	EXPECT_FALSE(dst.contains("Old"));
}

// ============ HttpResponse 测试 ============

TEST(HttpResponseTest, DefaultConstruction)
{
	HttpResponse res;
	EXPECT_EQ(res.statusCode(), HttpStatusCode::hOk);
}

TEST(HttpResponseTest, SetAndGetStatus)
{
	HttpResponse res;
	res.setStatus(HttpStatusCode::hNotFound);
	EXPECT_EQ(res.statusCode(), HttpStatusCode::hNotFound);
}

TEST(HttpResponseTest, SetAndGetHeader)
{
	HttpResponse res;
	res.setHeader("X-Custom", "test-value");
	EXPECT_EQ(res.header("X-Custom"), "test-value");
}

TEST(HttpResponseTest, SetBody)
{
	HttpResponse res;
	res.setBody("Hello, World!");
	EXPECT_EQ(res.body(), "Hello, World!");
	// 单参数 setBody 不该冲掉 Content-Type
	EXPECT_EQ(res.header("Content-Type"), "");
}

TEST(HttpResponseTest, SetBodyPreservesContentType)
{
	HttpResponse res;
	res.setHeader("Content-Type", "application/xml");
	res.setBody("<root/>");
	// 先设了 Content-Type，再 setBody 应该保持原值
	EXPECT_EQ(res.header("Content-Type"), "application/xml");
	EXPECT_EQ(res.body(), "<root/>");
}

TEST(HttpResponseTest, SetBodyWithContentType)
{
	HttpResponse res;
	res.setBody("<html></html>", "text/html");
	EXPECT_EQ(res.body(), "<html></html>");
	EXPECT_EQ(res.header("Content-Type"), "text/html");
}

TEST(HttpResponseTest, SetJsonBody)
{
	HttpResponse res;
	boost::json::value json = {{"status", "ok"}, {"count", 42}};
	res.setJsonBody(json);

	EXPECT_EQ(res.header("Content-Type"), "application/json");
	EXPECT_FALSE(res.body().empty());

	auto parsed = boost::json::parse(res.body());
	EXPECT_EQ(parsed.at("status").as_string(), "ok");
	EXPECT_EQ(parsed.at("count").as_int64(), 42);
}

// ============ HttpResponse 工厂方法测试 ============

TEST(HttpResponseTest, FactoryOk)
{
	auto res = HttpResponse::ok("Hello");
	EXPECT_EQ(res.statusCode(), HttpStatusCode::hOk);
	EXPECT_EQ(res.body(), "Hello");
	EXPECT_EQ(res.header("Content-Type"), "text/plain; charset=utf-8");
}

TEST(HttpResponseTest, FactoryOkWithCustomContentType)
{
	auto res = HttpResponse::ok("{}", "application/json");
	EXPECT_EQ(res.statusCode(), HttpStatusCode::hOk);
	EXPECT_EQ(res.body(), "{}");
	EXPECT_EQ(res.header("Content-Type"), "application/json");
}

TEST(HttpResponseTest, FactoryJson)
{
	auto res = HttpResponse::json({{"key", "value"}});
	EXPECT_EQ(res.statusCode(), HttpStatusCode::hOk);
	EXPECT_EQ(res.header("Content-Type"), "application/json");
}

TEST(HttpResponseTest, FactoryNotFound)
{
	auto res = HttpResponse::notFound();
	EXPECT_EQ(res.statusCode(), HttpStatusCode::hNotFound);
	EXPECT_EQ(res.body(), "Not Found");
}

TEST(HttpResponseTest, FactoryBadRequest)
{
	auto res = HttpResponse::badRequest("Invalid input");
	EXPECT_EQ(res.statusCode(), HttpStatusCode::hBadRequest);
	EXPECT_EQ(res.body(), "Invalid input");
}

TEST(HttpResponseTest, FactoryServerError)
{
	auto res = HttpResponse::serverError();
	EXPECT_EQ(res.statusCode(), HttpStatusCode::hInternalServerError);
}
