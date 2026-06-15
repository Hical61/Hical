#include "core/HttpTypes.h"
#include "core/HttpRequest.h"
#include "core/HttpResponse.h"
#include <gtest/gtest.h>

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

// ============ HttpRequest 测试 ============

TEST(HttpRequestTest, DefaultConstruction)
{
	HttpRequest req;
	EXPECT_EQ(req.body(), "");
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
