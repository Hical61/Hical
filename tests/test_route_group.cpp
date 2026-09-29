/**
 * @file test_route_group.cpp
 * @brief 路由组（前缀分组 + 组级中间件）测试
 */

#include "core/Router.h"
#include "core/RouteGroup.h"
#include "core/Middleware.h"
#include "core/Helmet.h"
#include "test_helpers.h"
#include <gtest/gtest.h>
#include <string>
#include <vector>

using namespace hical;
using hical::test::runCoroutine;

TEST(RouteGroupTest, BasicGroupRouting)
{
	AsioEventLoop loop;
	Router router;

	auto api = router.group("/api/v1");
	api.get("/users",
			[](const HttpRequest&) -> Awaitable<HttpResponse>
			{
				co_return HttpResponse::ok("users list");
			});

	HttpRequest req;
	req.setMethod(HttpMethod::hGet);
	req.setTarget("/api/v1/users");

	auto result = runCoroutine(loop,
							   [&]()
							   {
								   return router.dispatch(req);
							   });

	ASSERT_TRUE(result.has_value());
	EXPECT_EQ(result->statusCode(), HttpStatusCode::hOk);
	EXPECT_EQ(result->body(), "users list");
}

TEST(RouteGroupTest, NestedGroup)
{
	AsioEventLoop loop;
	Router router;

	auto api = router.group("/api/v1");
	auto admin = api.group("/admin");
	admin.get("/stats",
			  [](const HttpRequest&) -> Awaitable<HttpResponse>
			  {
				  co_return HttpResponse::ok("stats");
			  });

	HttpRequest req;
	req.setMethod(HttpMethod::hGet);
	req.setTarget("/api/v1/admin/stats");

	auto result = runCoroutine(loop,
							   [&]()
							   {
								   return router.dispatch(req);
							   });

	ASSERT_TRUE(result.has_value());
	EXPECT_EQ(result->statusCode(), HttpStatusCode::hOk);
	EXPECT_EQ(result->body(), "stats");
}

TEST(RouteGroupTest, GroupMiddleware)
{
	AsioEventLoop loop;
	Router router;

	auto api = router.group("/api");
	api.use(
		[](HttpRequest& req, MiddlewareNext next) -> Awaitable<HttpResponse>
		{
			req.setAttribute("auth", std::string("passed"));
			co_return co_await next(req);
		});

	api.get("/data",
			[](const HttpRequest& req) -> Awaitable<HttpResponse>
			{
				auto auth = req.getAttribute<std::string>("auth");
				if (auth && *auth == "passed")
				{
					co_return HttpResponse::ok("authorized");
				}
				co_return HttpResponse::badRequest("no auth");
			});

	router.get("/public",
			   [](const HttpRequest& req) -> Awaitable<HttpResponse>
			   {
				   auto auth = req.getAttribute<std::string>("auth");
				   co_return HttpResponse::ok(auth.value_or("none"));
			   });

	{
		HttpRequest req;
		req.setMethod(HttpMethod::hGet);
		req.setTarget("/api/data");

		auto result = runCoroutine(loop,
								   [&]()
								   {
									   return router.dispatch(req);
								   });

		ASSERT_TRUE(result.has_value());
		EXPECT_EQ(result->body(), "authorized");
	}

	{
		AsioEventLoop loop2;
		HttpRequest req;
		req.setMethod(HttpMethod::hGet);
		req.setTarget("/public");

		auto result = runCoroutine(loop2,
								   [&]()
								   {
									   return router.dispatch(req);
								   });

		ASSERT_TRUE(result.has_value());
		EXPECT_EQ(result->body(), "none");
	}
}

TEST(RouteGroupTest, EmptyPrefix)
{
	AsioEventLoop loop;
	Router router;

	auto g = router.group("");
	g.get("/hello",
		  [](const HttpRequest&) -> Awaitable<HttpResponse>
		  {
			  co_return HttpResponse::ok("hello");
		  });

	HttpRequest req;
	req.setMethod(HttpMethod::hGet);
	req.setTarget("/hello");

	auto result = runCoroutine(loop,
							   [&]()
							   {
								   return router.dispatch(req);
							   });

	ASSERT_TRUE(result.has_value());
	EXPECT_EQ(result->statusCode(), HttpStatusCode::hOk);
	EXPECT_EQ(result->body(), "hello");
}

TEST(RouteGroupTest, SyncRouteHandler)
{
	AsioEventLoop loop;
	Router router;

	auto api = router.group("/api");
	api.get("/sync",
			[](const HttpRequest&) -> HttpResponse
			{
				return HttpResponse::ok("sync ok");
			});

	HttpRequest req;
	req.setMethod(HttpMethod::hGet);
	req.setTarget("/api/sync");

	auto result = runCoroutine(loop,
							   [&]()
							   {
								   return router.dispatch(req);
							   });

	ASSERT_TRUE(result.has_value());
	EXPECT_EQ(result->body(), "sync ok");
}

TEST(RouteGroupTest, MultipleMethodsInGroup)
{
	AsioEventLoop loop;
	Router router;

	auto api = router.group("/api/v2");
	api.get("/items",
			[](const HttpRequest&) -> HttpResponse
			{
				return HttpResponse::ok("get items");
			});
	api.post("/items",
			 [](const HttpRequest&) -> HttpResponse
			 {
				 return HttpResponse::ok("post items");
			 });
	api.put("/items",
			[](const HttpRequest&) -> HttpResponse
			{
				return HttpResponse::ok("put items");
			});
	api.del("/items",
			[](const HttpRequest&) -> HttpResponse
			{
				return HttpResponse::ok("del items");
			});

	auto dispatch = [&](HttpMethod method) -> std::optional<HttpResponse>
	{
		AsioEventLoop l;
		HttpRequest req;
		req.setMethod(method);
		req.setTarget("/api/v2/items");
		return runCoroutine(l,
							[&]()
							{
								return router.dispatch(req);
							});
	};

	auto r1 = dispatch(HttpMethod::hGet);
	ASSERT_TRUE(r1.has_value());
	EXPECT_EQ(r1->body(), "get items");

	auto r2 = dispatch(HttpMethod::hPost);
	ASSERT_TRUE(r2.has_value());
	EXPECT_EQ(r2->body(), "post items");

	auto r3 = dispatch(HttpMethod::hPut);
	ASSERT_TRUE(r3.has_value());
	EXPECT_EQ(r3->body(), "put items");

	auto r4 = dispatch(HttpMethod::hDelete);
	ASSERT_TRUE(r4.has_value());
	EXPECT_EQ(r4->body(), "del items");
}

TEST(RouteGroupTest, GroupDoesNotAffectOtherRoutes)
{
	AsioEventLoop loop;
	Router router;

	// 在组注册之前就注册外部路由
	router.get("/outside",
			   [](const HttpRequest&) -> HttpResponse
			   {
				   return HttpResponse::ok("outside");
			   });

	auto api = router.group("/api");
	api.use(
		[](HttpRequest& req, MiddlewareNext next) -> Awaitable<HttpResponse>
		{
			req.setAttribute("injected", std::string("yes"));
			co_return co_await next(req);
		});
	api.get("/inside",
			[](const HttpRequest& req) -> HttpResponse
			{
				auto v = req.getAttribute<std::string>("injected");
				return HttpResponse::ok(v.value_or("no"));
			});

	{
		AsioEventLoop l;
		HttpRequest req;
		req.setMethod(HttpMethod::hGet);
		req.setTarget("/outside");
		auto result = runCoroutine(l,
								   [&]()
								   {
									   return router.dispatch(req);
								   });
		ASSERT_TRUE(result.has_value());
		// 组外路由不被组中间件影响，attribute 不存在
		EXPECT_EQ(result->body(), "outside");
	}

	{
		AsioEventLoop l;
		HttpRequest req;
		req.setMethod(HttpMethod::hGet);
		req.setTarget("/api/inside");
		auto result = runCoroutine(l,
								   [&]()
								   {
									   return router.dispatch(req);
								   });
		ASSERT_TRUE(result.has_value());
		EXPECT_EQ(result->body(), "yes");
	}
}

// ============ 裸 SyncAfterHandler 重载（RouteGroup::use(SyncAfterHandler)） ============
//
// RouteGroup 这条链走的是 wrapHandler 里的静态 buildOptimizedChain，跟 HttpServer 那条
// 不是同一条，得单独盯。

// 异步路由处理器：走 wrapHandler → buildOptimizedChain
TEST(RouteGroupTest, AfterOnlyMiddleware_AsyncHandler_AddsHeadersAndRunsInReverseOrder)
{
	AsioEventLoop loop;
	Router router;
	std::vector<std::string> order;

	auto api = router.group("/api");
	api.use(
		[&order](HttpRequest&, HttpResponse& res) -> void
		{
			order.push_back("first");
			res.setHeader("X-First", "1");
		});
	api.use(
		[&order](HttpRequest&, HttpResponse& res) -> void
		{
			order.push_back("second");
			res.setHeader("X-Second", "1");
		});

	api.get("/secure",
			[&order](const HttpRequest&) -> Awaitable<HttpResponse>
			{
				order.push_back("handler");
				co_return HttpResponse::ok("secure");
			});

	HttpRequest req;
	req.setMethod(HttpMethod::hGet);
	req.setTarget("/api/secure");

	auto result = runCoroutine(loop,
							   [&]()
							   {
								   return router.dispatch(req);
							   });

	ASSERT_TRUE(result.has_value());
	EXPECT_EQ(result->body(), "secure");
	EXPECT_EQ(result->header("X-First"), "1");
	EXPECT_EQ(result->header("X-Second"), "1");

	ASSERT_EQ(order.size(), 3u);
	EXPECT_EQ(order[0], "handler");
	EXPECT_EQ(order[1], "second"); // 后注册的先跑
	EXPECT_EQ(order[2], "first");
}

// 同步路由处理器：走 route(SyncRouteHandler) 里那条纯同步快速路径（另一段循环）
TEST(RouteGroupTest, AfterOnlyMiddleware_SyncHandler_AddsHeadersAndRunsInReverseOrder)
{
	AsioEventLoop loop;
	Router router;
	std::vector<std::string> order;

	auto api = router.group("/api");
	api.use(
		[&order](HttpRequest&, HttpResponse& res) -> void
		{
			order.push_back("first");
			res.setHeader("X-First", "1");
		});
	api.use(
		[&order](HttpRequest&, HttpResponse& res) -> void
		{
			order.push_back("second");
			res.setHeader("X-Second", "1");
		});

	api.get("/secure-sync",
			[&order](const HttpRequest&) -> HttpResponse
			{
				order.push_back("handler");
				return HttpResponse::ok("secure-sync");
			});

	HttpRequest req;
	req.setMethod(HttpMethod::hGet);
	req.setTarget("/api/secure-sync");

	auto result = runCoroutine(loop,
							   [&]()
							   {
								   return router.dispatch(req);
							   });

	ASSERT_TRUE(result.has_value());
	EXPECT_EQ(result->body(), "secure-sync");
	EXPECT_EQ(result->header("X-First"), "1");
	EXPECT_EQ(result->header("X-Second"), "1");

	ASSERT_EQ(order.size(), 3u);
	EXPECT_EQ(order[0], "handler");
	EXPECT_EQ(order[1], "second");
	EXPECT_EQ(order[2], "first");
}

// 直接拿 helmet 工厂验证：组内路由真的带上安全头
TEST(RouteGroupTest, AfterOnlyHelmetMiddleware_GroupRoutesGetSecurityHeaders)
{
	AsioEventLoop loop;
	Router router;

	auto api = router.group("/api");
	api.use(makeHelmetMiddleware());

	api.get("/secure",
			[](const HttpRequest&) -> Awaitable<HttpResponse>
			{
				co_return HttpResponse::ok("secure");
			});

	HttpRequest req;
	req.setMethod(HttpMethod::hGet);
	req.setTarget("/api/secure");

	auto result = runCoroutine(loop,
							   [&]()
							   {
								   return router.dispatch(req);
							   });

	ASSERT_TRUE(result.has_value());
	EXPECT_EQ(result->statusCode(), HttpStatusCode::hOk);
	EXPECT_EQ(result->header("X-Content-Type-Options"), "nosniff");
	EXPECT_EQ(result->header("X-Frame-Options"), "DENY");
	EXPECT_EQ(result->header("Content-Security-Policy"), "default-src 'self'");
}
