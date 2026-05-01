#include "core/Router.h"
#include "test_helpers.h"
#include <gtest/gtest.h>

using namespace hical;
using hical::test::runCoroutine;

// 测试空路由器返回 404
TEST(RouterTest, EmptyRouterReturns404)
{
	AsioEventLoop loop;
	Router router;

	HttpRequest req;
	req.setMethod(HttpMethod::hGet);
	req.setTarget("/anything");

	auto result = runCoroutine(loop,
							   [&]()
							   {
								   return router.dispatch(req);
							   });

	ASSERT_TRUE(result.has_value());
	EXPECT_EQ(result->statusCode(), HttpStatusCode::hNotFound);
}

// 测试注册和匹配 GET 路由
TEST(RouterTest, GetRoute)
{
	AsioEventLoop loop;
	Router router;

	router.get("/api/hello",
			   [](const HttpRequest&) -> HttpResponse
			   {
				   return HttpResponse::ok("Hello!");
			   });

	EXPECT_EQ(router.routeCount(), 1);

	HttpRequest req;
	req.setMethod(HttpMethod::hGet);
	req.setTarget("/api/hello");

	auto result = runCoroutine(loop,
							   [&]()
							   {
								   return router.dispatch(req);
							   });

	ASSERT_TRUE(result.has_value());
	EXPECT_EQ(result->statusCode(), HttpStatusCode::hOk);
	EXPECT_EQ(result->body(), "Hello!");
}

// 测试 POST 路由
TEST(RouterTest, PostRoute)
{
	AsioEventLoop loop;
	Router router;

	router.post("/api/data",
				[](const HttpRequest& req) -> HttpResponse
				{
					return HttpResponse::ok("Received: " + req.body());
				});

	HttpRequest req;
	req.setMethod(HttpMethod::hPost);
	req.setTarget("/api/data");
	req.setBody("test body");

	auto result = runCoroutine(loop,
							   [&]()
							   {
								   return router.dispatch(req);
							   });

	ASSERT_TRUE(result.has_value());
	EXPECT_EQ(result->body(), "Received: test body");
}

// 测试方法不匹配返回 405（路径存在但方法未注册）
TEST(RouterTest, MethodMismatchReturns405)
{
	AsioEventLoop loop;
	Router router;

	router.get("/api/hello",
			   [](const HttpRequest&) -> HttpResponse
			   {
				   return HttpResponse::ok("Hello!");
			   });

	HttpRequest req;
	req.setMethod(HttpMethod::hPost); // 注册的是 GET
	req.setTarget("/api/hello");

	auto result = runCoroutine(loop,
							   [&]()
							   {
								   return router.dispatch(req);
							   });

	ASSERT_TRUE(result.has_value());
	EXPECT_EQ(result->statusCode(), HttpStatusCode::hMethodNotAllowed);
	EXPECT_NE(result->header("Allow").find("GET"), std::string::npos);
}

// 测试路径不匹配返回 404
TEST(RouterTest, PathMismatchReturns404)
{
	AsioEventLoop loop;
	Router router;

	router.get("/api/hello",
			   [](const HttpRequest&) -> HttpResponse
			   {
				   return HttpResponse::ok("Hello!");
			   });

	HttpRequest req;
	req.setMethod(HttpMethod::hGet);
	req.setTarget("/api/world");

	auto result = runCoroutine(loop,
							   [&]()
							   {
								   return router.dispatch(req);
							   });

	ASSERT_TRUE(result.has_value());
	EXPECT_EQ(result->statusCode(), HttpStatusCode::hNotFound);
}

// 测试多个路由
TEST(RouterTest, MultipleRoutes)
{
	AsioEventLoop loop;
	Router router;

	router.get("/api/users",
			   [](const HttpRequest&) -> HttpResponse
			   {
				   return HttpResponse::ok("users list");
			   });
	router.post("/api/users",
				[](const HttpRequest&) -> HttpResponse
				{
					return HttpResponse::ok("user created");
				});
	router.get("/api/status",
			   [](const HttpRequest&) -> HttpResponse
			   {
				   return HttpResponse::ok("ok");
			   });

	EXPECT_EQ(router.routeCount(), 3);

	// 测试 GET /api/users
	{
		HttpRequest req;
		req.setMethod(HttpMethod::hGet);
		req.setTarget("/api/users");

		auto result = runCoroutine(loop,
								   [&]()
								   {
									   return router.dispatch(req);
								   });
		ASSERT_TRUE(result.has_value());
		EXPECT_EQ(result->body(), "users list");
	}
}

// 测试协程路由处理器
TEST(RouterTest, AsyncRouteHandler)
{
	AsioEventLoop loop;
	Router router;

	router.get("/api/async",
			   [](const HttpRequest&) -> Awaitable<HttpResponse>
			   {
				   co_await sleep(0.01);
				   co_return HttpResponse::ok("async result");
			   });

	HttpRequest req;
	req.setMethod(HttpMethod::hGet);
	req.setTarget("/api/async");

	auto result = runCoroutine(loop,
							   [&]()
							   {
								   return router.dispatch(req);
							   });

	ASSERT_TRUE(result.has_value());
	EXPECT_EQ(result->body(), "async result");
}

// 测试 JSON 响应路由
TEST(RouterTest, JsonRoute)
{
	AsioEventLoop loop;
	Router router;

	router.get("/api/info",
			   [](const HttpRequest&) -> HttpResponse
			   {
				   return HttpResponse::json({{"version", "0.1.0"}, {"name", "hical"}});
			   });

	HttpRequest req;
	req.setMethod(HttpMethod::hGet);
	req.setTarget("/api/info");

	auto result = runCoroutine(loop,
							   [&]()
							   {
								   return router.dispatch(req);
							   });

	ASSERT_TRUE(result.has_value());
	EXPECT_EQ(result->header("Content-Type"), "application/json");

	auto json = boost::json::parse(result->body());
	EXPECT_EQ(json.at("name").as_string(), "hical");
}

// 测试 HICAL_ROUTE 宏
TEST(RouterTest, HicalRouteMacro)
{
	AsioEventLoop loop;
	Router router;

	HICAL_ROUTE(router,
				Get,
				"/macro/test",
				[](const HttpRequest&) -> HttpResponse
				{
					return HttpResponse::ok("macro works");
				});

	EXPECT_EQ(router.routeCount(), 1);

	HttpRequest req;
	req.setMethod(HttpMethod::hGet);
	req.setTarget("/macro/test");

	auto result = runCoroutine(loop,
							   [&]()
							   {
								   return router.dispatch(req);
							   });

	ASSERT_TRUE(result.has_value());
	EXPECT_EQ(result->body(), "macro works");
}

// 测试 PUT 和 DELETE 路由
TEST(RouterTest, PutAndDeleteRoutes)
{
	Router router;

	router.put("/api/item",
			   [](const HttpRequest&) -> HttpResponse
			   {
				   return HttpResponse::ok("updated");
			   });
	router.del("/api/item",
			   [](const HttpRequest&) -> HttpResponse
			   {
				   return HttpResponse::ok("deleted");
			   });

	EXPECT_EQ(router.routeCount(), 2);
}

// ============ 路径参数测试 ============

TEST(RouterTest, PathParameter)
{
	AsioEventLoop loop;
	Router router;

	router.get("/users/{id}",
			   [](const HttpRequest& req) -> HttpResponse
			   {
				   return HttpResponse::ok("User: " + req.param("id"));
			   });

	HttpRequest req;
	req.setMethod(HttpMethod::hGet);
	req.setTarget("/users/42");

	auto result = runCoroutine(loop,
							   [&]()
							   {
								   return router.dispatch(req);
							   });

	ASSERT_TRUE(result.has_value());
	EXPECT_EQ(result->statusCode(), HttpStatusCode::hOk);
	EXPECT_EQ(result->body(), "User: 42");
}

TEST(RouterTest, MultiplePathParameters)
{
	AsioEventLoop loop;
	Router router;

	router.get("/users/{userId}/posts/{postId}",
			   [](const HttpRequest& req) -> HttpResponse
			   {
				   return HttpResponse::ok("user=" + req.param("userId") + " post=" + req.param("postId"));
			   });

	HttpRequest req;
	req.setMethod(HttpMethod::hGet);
	req.setTarget("/users/123/posts/456");

	auto result = runCoroutine(loop,
							   [&]()
							   {
								   return router.dispatch(req);
							   });

	ASSERT_TRUE(result.has_value());
	EXPECT_EQ(result->body(), "user=123 post=456");
}

TEST(RouterTest, PathParameterMismatchSegments)
{
	AsioEventLoop loop;
	Router router;

	router.get("/users/{id}",
			   [](const HttpRequest&) -> HttpResponse
			   {
				   return HttpResponse::ok("found");
			   });

	// 路径段数不匹配
	HttpRequest req;
	req.setMethod(HttpMethod::hGet);
	req.setTarget("/users/42/extra");

	auto result = runCoroutine(loop,
							   [&]()
							   {
								   return router.dispatch(req);
							   });

	ASSERT_TRUE(result.has_value());
	EXPECT_EQ(result->statusCode(), HttpStatusCode::hNotFound);
}

TEST(RouterTest, MixedStaticAndParam)
{
	AsioEventLoop loop;
	Router router;

	router.get("/api/v1/items/{id}/detail",
			   [](const HttpRequest& req) -> HttpResponse
			   {
				   return HttpResponse::ok("item " + req.param("id"));
			   });

	HttpRequest req;
	req.setMethod(HttpMethod::hGet);
	req.setTarget("/api/v1/items/789/detail");

	auto result = runCoroutine(loop,
							   [&]()
							   {
								   return router.dispatch(req);
							   });

	ASSERT_TRUE(result.has_value());
	EXPECT_EQ(result->body(), "item 789");
}

// ============ 安全：URL 解码 NULL 字节过滤 ============

TEST(RouterTest, UrlDecodeStripsNullByte)
{
	AsioEventLoop loop;
	Router router;

	std::string capturedParam;
	router.get("/files/{name}",
			   [&capturedParam](const HttpRequest& req) -> HttpResponse
			   {
				   capturedParam = req.param("name");
				   return HttpResponse::ok("ok");
			   });

	HttpRequest req;
	req.setMethod(HttpMethod::hGet);
	req.setTarget("/files/secret%00.txt");

	auto result = runCoroutine(loop,
							   [&]()
							   {
								   return router.dispatch(req);
							   });

	ASSERT_TRUE(result.has_value());
	EXPECT_EQ(result->statusCode(), HttpStatusCode::hOk);
	// %00 应被剥离，参数值中不应包含 NULL 字节
	EXPECT_EQ(capturedParam, "secret.txt");
	EXPECT_EQ(capturedParam.find('\0'), std::string::npos);
}

TEST(RouterTest, UrlDecodeStripsMultipleNullBytes)
{
	AsioEventLoop loop;
	Router router;

	std::string capturedParam;
	router.get("/data/{key}",
			   [&capturedParam](const HttpRequest& req) -> HttpResponse
			   {
				   capturedParam = req.param("key");
				   return HttpResponse::ok("ok");
			   });

	HttpRequest req;
	req.setMethod(HttpMethod::hGet);
	req.setTarget("/data/%00abc%00def%00");

	auto result = runCoroutine(loop,
							   [&]()
							   {
								   return router.dispatch(req);
							   });

	ASSERT_TRUE(result.has_value());
	EXPECT_EQ(result->statusCode(), HttpStatusCode::hOk);
	EXPECT_EQ(capturedParam, "abcdef");
}

// ============ 405 Method Not Allowed 测试 ============

TEST(RouterTest, MethodNotAllowedStaticRoute)
{
	AsioEventLoop loop;
	Router router;

	router.get("/api/users",
			   [](const HttpRequest&) -> HttpResponse
			   {
				   return HttpResponse::ok("get");
			   });
	router.post("/api/users",
				[](const HttpRequest&) -> HttpResponse
				{
					return HttpResponse::ok("post");
				});

	// DELETE /api/users 未注册，但路径匹配，应返回 405
	HttpRequest req;
	req.setMethod(HttpMethod::hDelete);
	req.setTarget("/api/users");

	auto result = runCoroutine(loop,
							   [&]()
							   {
								   return router.dispatch(req);
							   });

	ASSERT_TRUE(result.has_value());
	EXPECT_EQ(result->statusCode(), HttpStatusCode::hMethodNotAllowed);
	auto allow = result->header("Allow");
	EXPECT_NE(allow.find("GET"), std::string::npos);
	EXPECT_NE(allow.find("POST"), std::string::npos);
}

TEST(RouterTest, NotFoundReturns404NotMethod405)
{
	AsioEventLoop loop;
	Router router;

	router.get("/api/users",
			   [](const HttpRequest&) -> HttpResponse
			   {
				   return HttpResponse::ok("ok");
			   });

	// 完全不匹配的路径应返回 404
	HttpRequest req;
	req.setMethod(HttpMethod::hGet);
	req.setTarget("/api/nonexistent");

	auto result = runCoroutine(loop,
							   [&]()
							   {
								   return router.dispatch(req);
							   });

	ASSERT_TRUE(result.has_value());
	EXPECT_EQ(result->statusCode(), HttpStatusCode::hNotFound);
}

TEST(RouterTest, MethodNotAllowedParamRoute)
{
	AsioEventLoop loop;
	Router router;

	router.get("/api/users/{id}",
			   [](const HttpRequest&) -> HttpResponse
			   {
				   return HttpResponse::ok("get user");
			   });

	// PUT /api/users/123 未注册，但参数路由路径匹配，应返回 405
	HttpRequest req;
	req.setMethod(HttpMethod::hPut);
	req.setTarget("/api/users/123");

	auto result = runCoroutine(loop,
							   [&]()
							   {
								   return router.dispatch(req);
							   });

	ASSERT_TRUE(result.has_value());
	EXPECT_EQ(result->statusCode(), HttpStatusCode::hMethodNotAllowed);
	auto allow = result->header("Allow");
	EXPECT_NE(allow.find("GET"), std::string::npos);
}
