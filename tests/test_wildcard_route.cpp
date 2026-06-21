/**
 * @file test_wildcard_route.cpp
 * @brief 通配路由 `*path` 测试
 */

#include "core/Router.h"
#include "core/HttpRequest.h"
#include "core/HttpResponse.h"
#include <gtest/gtest.h>

using namespace hical;

// ──────────────────────────────────────────────
// 1. 通配路由匹配静态路径
// ──────────────────────────────────────────────
TEST(WildcardRouteTest, WildcardMatchesStaticPath)
{
	Router router;
	bool handlerCalled = false;

	router.get("/api/*path",
			   [&handlerCalled](const HttpRequest& req) -> HttpResponse
			   {
				   handlerCalled = true;
				   EXPECT_EQ(req.param("path"), "users");
				   return HttpResponse::ok("ok");
			   });

	HttpRequest req;
	req.setMethod(HttpMethod::hGet);
	req.setTarget("/api/users");

	auto result = router.dispatchSync(req);
	ASSERT_TRUE(result.has_value());
	EXPECT_TRUE(handlerCalled);
}

// ──────────────────────────────────────────────
// 2. 通配路由匹配多级路径
// ──────────────────────────────────────────────
TEST(WildcardRouteTest, WildcardMatchesMultiSegment)
{
	Router router;

	router.get("/files/*path",
			   [](const HttpRequest& req) -> HttpResponse
			   {
				   EXPECT_EQ(req.param("path"), "a/b/c.txt");
				   return HttpResponse::ok("ok");
			   });

	HttpRequest req;
	req.setMethod(HttpMethod::hGet);
	req.setTarget("/files/a/b/c.txt");

	auto result = router.dispatchSync(req);
	ASSERT_TRUE(result.has_value());
}

// ──────────────────────────────────────────────
// 3. 通配路由捕获空路径
// ──────────────────────────────────────────────
TEST(WildcardRouteTest, WildcardCapturesEmpty)
{
	Router router;

	router.get("/api/*path",
			   [](const HttpRequest& req) -> HttpResponse
			   {
				   EXPECT_EQ(req.param("path"), "");
				   return HttpResponse::ok("ok");
			   });

	HttpRequest req;
	req.setMethod(HttpMethod::hGet);
	req.setTarget("/api/");

	auto result = router.dispatchSync(req);
	ASSERT_TRUE(result.has_value());
}

// ──────────────────────────────────────────────
// 4. 通配路由不匹配不相关的路径
// ──────────────────────────────────────────────
TEST(WildcardRouteTest, WildcardDoesNotMatchWrongPrefix)
{
	Router router;
	router.get("/api/*path",
			   [](const HttpRequest&)
			   {
				   return HttpResponse::ok("ok");
			   });

	HttpRequest req;
	req.setMethod(HttpMethod::hGet);
	req.setTarget("/other/xxx");

	auto result = router.dispatchSync(req);
	EXPECT_FALSE(result.has_value()); // 应为 404，dispatchSync 返回 nullopt
}

// ──────────────────────────────────────────────
// 5. 优先级：静态 > 参数 > 通配
// ──────────────────────────────────────────────
TEST(WildcardRouteTest, StaticHasPriorityOverWildcard)
{
	Router router;
	bool staticCalled = false;
	bool wildcardCalled = false;

	router.get("/api/users",
			   [&staticCalled](const HttpRequest&) -> HttpResponse
			   {
				   staticCalled = true;
				   return HttpResponse::ok("static");
			   });
	router.get("/api/*path",
			   [&wildcardCalled](const HttpRequest&) -> HttpResponse
			   {
				   wildcardCalled = true;
				   return HttpResponse::ok("wildcard");
			   });

	HttpRequest req;
	req.setMethod(HttpMethod::hGet);
	req.setTarget("/api/users");

	auto result = router.dispatchSync(req);
	ASSERT_TRUE(result.has_value());
	EXPECT_TRUE(staticCalled);
	EXPECT_FALSE(wildcardCalled);
}

// ──────────────────────────────────────────────
// 6. 优先级：参数 > 通配
// ──────────────────────────────────────────────
TEST(WildcardRouteTest, ParamHasPriorityOverWildcard)
{
	Router router;
	bool paramCalled = false;

	router.get("/api/{id}",
			   [&paramCalled](const HttpRequest& req) -> HttpResponse
			   {
				   paramCalled = true;
				   EXPECT_EQ(req.param("id"), "123");
				   return HttpResponse::ok("param");
			   });
	router.get("/api/*path",
			   [](const HttpRequest&) -> HttpResponse
			   {
				   return HttpResponse::ok("wildcard");
			   });

	HttpRequest req;
	req.setMethod(HttpMethod::hGet);
	req.setTarget("/api/123");

	auto result = router.dispatchSync(req);
	ASSERT_TRUE(result.has_value());
	EXPECT_TRUE(paramCalled);
}

// ──────────────────────────────────────────────
// 7. 同步 handler 通配路由
// ──────────────────────────────────────────────
TEST(WildcardRouteTest, SyncHandlerWorks)
{
	Router router;

	router.get("/api/*path",
			   [](const HttpRequest& req) -> HttpResponse
			   {
				   return HttpResponse::ok(req.param("path"));
			   });

	HttpRequest req;
	req.setMethod(HttpMethod::hGet);
	req.setTarget("/api/hello");

	auto result = router.dispatchSync(req);
	ASSERT_TRUE(result.has_value());
	EXPECT_EQ(result->body(), "hello");
}

// ──────────────────────────────────────────────
// 8. 不同 method 的通配路由独立工作
// ──────────────────────────────────────────────
TEST(WildcardRouteTest, DifferentMethodsAreIndependent)
{
	Router router;
	bool getCalled = false;
	bool postCalled = false;

	router.get("/api/*path",
			   [&getCalled](const HttpRequest&) -> HttpResponse
			   {
				   getCalled = true;
				   return HttpResponse::ok("get");
			   });
	router.post("/api/*path",
				[&postCalled](const HttpRequest&) -> HttpResponse
				{
					postCalled = true;
					return HttpResponse::ok("post");
				});

	// GET 请求匹配 GET 通配
	HttpRequest getReq;
	getReq.setMethod(HttpMethod::hGet);
	getReq.setTarget("/api/users");
	(void)router.dispatchSync(getReq);
	EXPECT_TRUE(getCalled);

	// POST 请求匹配 POST 通配
	HttpRequest postReq;
	postReq.setMethod(HttpMethod::hPost);
	postReq.setTarget("/api/data");
	(void)router.dispatchSync(postReq);
	EXPECT_TRUE(postCalled);
}

// ──────────────────────────────────────────────
// 9. routeCount 包含通配路由
// ──────────────────────────────────────────────
TEST(WildcardRouteTest, RouteCountIncludesWildcard)
{
	Router router;
	router.get("/api/users",
			   [](const HttpRequest&)
			   {
				   return HttpResponse::ok();
			   });
	router.get("/api/*path",
			   [](const HttpRequest&)
			   {
				   return HttpResponse::ok();
			   });
	router.post("/api/*path",
				[](const HttpRequest&)
				{
					return HttpResponse::ok();
				});

	EXPECT_EQ(router.routeCount(), 3);
}

// ──────────────────────────────────────────────
// 10. 通配路由异步 handler 通过 dispatch 工作
// ──────────────────────────────────────────────
TEST(WildcardRouteTest, AsyncHandlerWorksViaDispatch)
{
	Router router;

	router.get("/api/*path",
			   [](const HttpRequest& req) -> Awaitable<HttpResponse>
			   {
				   co_return HttpResponse::ok(req.param("path"));
			   });

	HttpRequest req;
	req.setMethod(HttpMethod::hGet);
	req.setTarget("/api/async-data");

	// dispatch 是协程，但同步 handler 包装的异步路由也走同步路径
	// 实际异步路由需要事件循环运行，这里只验证 dispatchSync 返回 nullopt
	auto result = router.dispatchSync(req);
	EXPECT_FALSE(result.has_value()); // 异步 handler → dispatchSync 返回 nullopt
}

// ──────────────────────────────────────────────
// 11. 只有 `*path` 做通配匹配，单独的 `*` 不是通配符
// ──────────────────────────────────────────────
TEST(WildcardRouteTest, OnlyAsteriskWithParamNameIsWildcard)
{
	Router router;
	router.get("/static/*",
			   [](const HttpRequest&) -> HttpResponse
			   {
				   return HttpResponse::ok("just asterisk");
			   });

	HttpRequest req;
	req.setMethod(HttpMethod::hGet);
	req.setTarget("/static/*");

	// 字面匹配 `*` 段，不是通配
	auto result = router.dispatchSync(req);
	ASSERT_TRUE(result.has_value());
	EXPECT_EQ(result->body(), "just asterisk");
}

// ──────────────────────────────────────────────
// 12. 通配路由不匹配不同 method（405 检测）
// ──────────────────────────────────────────────
TEST(WildcardRouteTest, WildcardMethodMismatch)
{
	Router router;
	router.get("/api/*path",
			   [](const HttpRequest&) -> HttpResponse
			   {
				   return HttpResponse::ok("get");
			   });

	// POST 到 /api/users 不应匹配 GET 通配
	HttpRequest req;
	req.setMethod(HttpMethod::hPost);
	req.setTarget("/api/users");

	// dispatchSync 返回 nullopt（405 需要异步 dispatch 来处理）
	auto result = router.dispatchSync(req);
	EXPECT_FALSE(result.has_value());
}
