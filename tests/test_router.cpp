/**
 * @file test_router.cpp
 * @brief 路由器测试（静态/参数/通配匹配、404/405 与 Allow 头）
 */

#include "core/Router.h"
#include "test_helpers.h"
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>

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

// 通配路由上的方法不匹配同样算 405，以前这段是缺的，一律掉 404 且 Allow 为空
TEST(RouterTest, MethodNotAllowedWildcardRoute)
{
	AsioEventLoop loop;
	Router router;

	router.get("/api/*path",
			   [](const HttpRequest&) -> HttpResponse
			   {
				   return HttpResponse::ok("get");
			   });

	HttpRequest req;
	req.setMethod(HttpMethod::hPost);
	req.setTarget("/api/users");

	auto result = runCoroutine(loop,
							   [&]()
							   {
								   return router.dispatch(req);
							   });

	ASSERT_TRUE(result.has_value());
	EXPECT_EQ(result->statusCode(), HttpStatusCode::hMethodNotAllowed);
	auto allow = result->header("Allow");
	EXPECT_EQ(allow, "GET");
}

// HEAD 是最常见的躺枪者：同一个通配路由，HEAD 也该拿到 405 而不是 404
TEST(RouterTest, MethodNotAllowedWildcardRouteHead)
{
	AsioEventLoop loop;
	Router router;

	router.get("/api/*path",
			   [](const HttpRequest&) -> HttpResponse
			   {
				   return HttpResponse::ok("get");
			   });

	HttpRequest req;
	req.setMethod(HttpMethod::hHead);
	req.setTarget("/api/users");

	auto result = runCoroutine(loop,
							   [&]()
							   {
								   return router.dispatch(req);
							   });

	ASSERT_TRUE(result.has_value());
	EXPECT_EQ(result->statusCode(), HttpStatusCode::hMethodNotAllowed);
	EXPECT_EQ(result->header("Allow"), "GET");
}

// 静态 GET 和通配 GET 并存时，Allow 不能出现重复方法
TEST(RouterTest, AllowHeaderDedupesStaticAndWildcard)
{
	AsioEventLoop loop;
	Router router;

	router.get("/api/users",
			   [](const HttpRequest&) -> HttpResponse
			   {
				   return HttpResponse::ok("static");
			   });
	router.get("/api/*path",
			   [](const HttpRequest&) -> HttpResponse
			   {
				   return HttpResponse::ok("wildcard");
			   });

	HttpRequest req;
	req.setMethod(HttpMethod::hPost);
	req.setTarget("/api/users");

	auto result = runCoroutine(loop,
							   [&]()
							   {
								   return router.dispatch(req);
							   });

	ASSERT_TRUE(result.has_value());
	EXPECT_EQ(result->statusCode(), HttpStatusCode::hMethodNotAllowed);
	EXPECT_EQ(result->header("Allow"), "GET");
}

// 同一段里重复注册同方法（静态 GET 两条）也不能重复输出
TEST(RouterTest, AllowHeaderDedupesRepeatedRegistration)
{
	AsioEventLoop loop;
	Router router;

	router.get("/api/users",
			   [](const HttpRequest&) -> HttpResponse
			   {
				   return HttpResponse::ok("first");
			   });
	router.get("/api/users",
			   [](const HttpRequest&) -> HttpResponse
			   {
				   return HttpResponse::ok("second");
			   });
	router.post("/api/users",
				[](const HttpRequest&) -> HttpResponse
				{
					return HttpResponse::ok("post");
				});

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
	EXPECT_EQ(result->header("Allow"), "GET, POST");
}

// ============ 404/405 快速判定（首段索引） ============

namespace
{
	// 编译期链用的占位中间件，只放行；作用是把路由从 compileTimeRoute 那条注册重载挂上去
	SyncMiddlewareResult ctPassThrough(HttpRequest&)
	{
		return std::nullopt;
	}

	// 404/405 用例只关心状态码和 Allow 头，压根不看响应体，注册用的 handler 直接共用一个占位
	SyncRouteHandler placeholderHandler()
	{
		return [](const HttpRequest&) -> HttpResponse
		{
			return HttpResponse::ok("ok");
		};
	}

	// 发一次请求断言 405，并把 Allow 头逐字比对。
	// AsioEventLoop 只能跑一次（io_context 没 restart），一个 loop 发第二个请求会静默拿到空结果，
	// 所以 loop 关在函数里头——一个 TEST 想发几个请求就调几次，别自己在外头复用 loop。
	void expectMethodNotAllowed(Router& router,
								HttpMethod method,
								const std::string& target,
								const std::string& expectedAllow)
	{
		AsioEventLoop loop;
		HttpRequest req;
		req.setMethod(method);
		req.setTarget(target);

		auto result = runCoroutine(loop,
								   [&]()
								   {
									   return router.dispatch(req);
								   });

		ASSERT_TRUE(result.has_value()) << "target=" << target;
		EXPECT_EQ(result->statusCode(), HttpStatusCode::hMethodNotAllowed) << "target=" << target;
		EXPECT_EQ(result->header("Allow"), expectedAllow) << "target=" << target;
	}

	// 同上，断言 404。405 和 404 只差一个 Allow 头，写错一边这条就会红
	void expectNotFound(Router& router, HttpMethod method, const std::string& target)
	{
		AsioEventLoop loop;
		HttpRequest req;
		req.setMethod(method);
		req.setTarget(target);

		auto result = runCoroutine(loop,
								   [&]()
								   {
									   return router.dispatch(req);
								   });

		ASSERT_TRUE(result.has_value()) << "target=" << target;
		EXPECT_EQ(result->statusCode(), HttpStatusCode::hNotFound) << "target=" << target;
		// 404 不该顺手带上 Allow，带了就说明这一路其实是 405 判错了
		EXPECT_TRUE(result->header("Allow").empty()) << "target=" << target;
	}
} // namespace

// 首段在所有 method 下都没注册过 → 直接 404，不必去扫 param/wildcard 路由
TEST(RouterTest, NotFound_UnknownFirstSegmentForAllMethods_Returns404)
{
	AsioEventLoop loop;
	Router router;

	router.get("/api/users/{id}",
			   [](const HttpRequest&) -> HttpResponse
			   {
				   return HttpResponse::ok("user");
			   });
	router.post("/api/*rest",
				[](const HttpRequest&) -> HttpResponse
				{
					return HttpResponse::ok("wildcard");
				});

	HttpRequest req;
	req.setMethod(HttpMethod::hDelete);
	req.setTarget("/metrics/latency");

	auto result = runCoroutine(loop,
							   [&]()
							   {
								   return router.dispatch(req);
							   });

	ASSERT_TRUE(result.has_value());
	EXPECT_EQ(result->statusCode(), HttpStatusCode::hNotFound);
}

// 405 的 Allow 按 kMethods 枚举顺序输出，跟注册顺序无关（这里先注册 PUT 后注册 GET）
TEST(RouterTest, MethodNotAllowed_ParamRouteAllowInEnumOrder_Returns405)
{
	AsioEventLoop loop;
	Router router;

	router.put("/api/items/{id}",
			   [](const HttpRequest&) -> HttpResponse
			   {
				   return HttpResponse::ok("put");
			   });
	router.get("/api/items/{id}",
			   [](const HttpRequest&) -> HttpResponse
			   {
				   return HttpResponse::ok("get");
			   });

	HttpRequest req;
	req.setMethod(HttpMethod::hDelete);
	req.setTarget("/api/items/42");

	auto result = runCoroutine(loop,
							   [&]()
							   {
								   return router.dispatch(req);
							   });

	ASSERT_TRUE(result.has_value());
	EXPECT_EQ(result->statusCode(), HttpStatusCode::hMethodNotAllowed);
	EXPECT_EQ(result->header("Allow"), "GET, PUT");
}

// compileTimeRoute 是另一条注册重载，首段索引同样得维护上，漏了就会让 405 静默变 404
TEST(RouterTest, MethodNotAllowed_CompileTimeRegisteredRoute_Returns405)
{
	AsioEventLoop loop;
	Router router;

	router.compileTimeRoute<CompileTimeSyncMw<ctPassThrough>>(HttpMethod::hGet,
															  "/ct/items/{id}",
															  [](const HttpRequest&) -> HttpResponse
															  {
																  return HttpResponse::ok("ct");
															  });

	HttpRequest req;
	req.setMethod(HttpMethod::hDelete);
	req.setTarget("/ct/items/9");

	auto result = runCoroutine(loop,
							   [&]()
							   {
								   return router.dispatch(req);
							   });

	ASSERT_TRUE(result.has_value());
	EXPECT_EQ(result->statusCode(), HttpStatusCode::hMethodNotAllowed);
	EXPECT_EQ(result->header("Allow"), "GET");
}

// 加了通配的 405 收集之后，方法匹配的请求照旧正常命中
TEST(RouterTest, WildcardRouteStillMatchesOwnMethod)
{
	Router router;
	bool handlerCalled = false;

	router.get("/api/*path",
			   [&handlerCalled](const HttpRequest& req) -> HttpResponse
			   {
				   handlerCalled = true;
				   EXPECT_EQ(req.param("path"), "users");
				   return HttpResponse::ok("wildcard");
			   });

	HttpRequest req;
	req.setMethod(HttpMethod::hGet);
	req.setTarget("/api/users");

	auto result = router.dispatchSync(req);
	ASSERT_TRUE(result.has_value());
	EXPECT_EQ(result->statusCode(), HttpStatusCode::hOk);
	EXPECT_TRUE(handlerCalled);
}

// ============ Allow 头语义（405） ============

// 静态和参数路由共用首段时两类都得进 405 收集，漏一类 Allow 就少一个方法
TEST(RouterTest, MethodNotAllowed_StaticAndParamShareFirstSegment_Returns405)
{
	Router router;
	router.get("/api/users", placeholderHandler());
	router.put("/api/{id}", placeholderHandler());

	expectMethodNotAllowed(router, HttpMethod::hPost, "/api/users", "GET, PUT");
}

// 静态 / 参数 / 通配三类同时给出方法时，Allow 按 kMethods 枚举顺序拼，不看注册顺序也不看路由种类。
// 这里故意先注册 DELETE 再 GET 再 PUT，跟枚举顺序完全拧着来
TEST(RouterTest, MethodNotAllowed_MixedKindsOutOfOrder_AllowInEnumOrder)
{
	Router router;
	router.del("/mix/*rest", placeholderHandler());
	router.get("/mix/items", placeholderHandler());
	router.put("/mix/{id}", placeholderHandler());

	expectMethodNotAllowed(router, HttpMethod::hPatch, "/mix/items", "GET, PUT, DELETE");
}

// 根路径的首段是空串，别把它当成「没有首段」而漏掉索引
TEST(RouterTest, MethodNotAllowed_RootPathEmptyFirstSegment_Returns405)
{
	Router router;
	router.get("/", placeholderHandler());
	router.post("/", placeholderHandler());

	expectMethodNotAllowed(router, HttpMethod::hDelete, "/", "GET, POST");
}

// HEAD 打在参数路由上同样是 405 而不是 404
TEST(RouterTest, MethodNotAllowed_HeadOnParamRoute_Returns405)
{
	Router router;
	router.get("/api/users/{id}", placeholderHandler());

	expectMethodNotAllowed(router, HttpMethod::hHead, "/api/users/7", "GET");
}

// 段边界星号（前缀落在 '/' 上）按首段归档：首段对上只说明「有可能是它」，还得前缀真匹配才算 405。
// 兄弟路径 "/api/v2/users" 的首段跟注册前缀一样（索引那一关照样放行），但前缀对不上，必须还是 404——
// 通配索引不能宽到「首段一样就送个 Allow 出去」
TEST(RouterTest, MethodNotAllowed_WildcardPrefixSibling_AllowOnlyForMatchingPrefix)
{
	Router router;
	router.get("/api/v1/*path", placeholderHandler());

	expectMethodNotAllowed(router, HttpMethod::hPost, "/api/v1/users", "GET");
	expectNotFound(router, HttpMethod::hPost, "/api/v2/users");
}

// ============ 首段索引：通配/参数写法各自的归档分支 ============

// 星号落在段中间："/files*x" 能匹配 "/files/..."，索引按前缀归档
TEST(RouterTest, MethodNotAllowed_WildcardMidSegmentPrefix_Returns405)
{
	Router router;
	router.get("/files*x", placeholderHandler());

	expectMethodNotAllowed(router, HttpMethod::hDelete, "/files/readme.txt", "GET");
	// 少个 s 就匹配不上了，前缀索引不能宽到把 "/file/..." 也算进来
	expectNotFound(router, HttpMethod::hDelete, "/file/readme.txt");
}

// 跨段星号："/a/b*c" 的前缀是 "/a/b"，首段只到 "a" 为止
TEST(RouterTest, MethodNotAllowed_CrossSegmentWildcardPrefix_Returns405)
{
	Router router;
	router.get("/a/b*c", placeholderHandler());

	expectMethodNotAllowed(router, HttpMethod::hDelete, "/a/bxxx", "GET");
	expectNotFound(router, HttpMethod::hDelete, "/ax/bxxx");
}

// catch-all "/*x" 前缀就是 "/"，什么首段都可能命中
TEST(RouterTest, MethodNotAllowed_CatchAllWildcard_Returns405)
{
	Router router;
	router.get("/*x", placeholderHandler());

	expectMethodNotAllowed(router, HttpMethod::hDelete, "/anything/at/all", "GET");
}

// 首段本身就是 {param} 时，任何首段都可能被它匹配上，索引得按「动态首段」归档
TEST(RouterTest, MethodNotAllowed_ParamFirstSegment_Returns405)
{
	Router router;
	router.get("/{id}/detail", placeholderHandler());

	expectMethodNotAllowed(router, HttpMethod::hDelete, "/abc/detail", "GET");
	// 动态首段只是「可能命中」，第二段对不上还是该 404
	expectNotFound(router, HttpMethod::hDelete, "/abc/other");
}

// 首段索引看的是 URL 解码之后的路径，拿原始路径去查会在 %xx 上报 404
TEST(RouterTest, MethodNotAllowed_UrlEncodedPath_Returns405)
{
	Router router;
	router.get("/api/items/{id}", placeholderHandler());

	expectMethodNotAllowed(router, HttpMethod::hDelete, "/%61pi/items/7", "GET");
}

// ============ compileTimeRoute 注册路径的首段索引 ============

// compileTimeRoute 注册的静态路由，索引同样得维护上，漏了 405 会静默变 404
TEST(RouterTest, MethodNotAllowed_CompileTimeStaticRoute_Returns405)
{
	Router router;
	router.compileTimeRoute<CompileTimeSyncMw<ctPassThrough>>(HttpMethod::hGet, "/ct/static", placeholderHandler());

	expectMethodNotAllowed(router, HttpMethod::hDelete, "/ct/static", "GET");
}

// compileTimeRoute 注册的通配路由，同上
TEST(RouterTest, MethodNotAllowed_CompileTimeWildcardRoute_Returns405)
{
	Router router;
	router.compileTimeRoute<CompileTimeSyncMw<ctPassThrough>>(HttpMethod::hGet, "/ctw/*rest", placeholderHandler());

	expectMethodNotAllowed(router, HttpMethod::hDelete, "/ctw/x", "GET");
}

// ============ 复杂度守卫 ============

// 路径在所有 method 下都没注册过时，404 判定不该随路由表规模线性增长。
// 首段索引就是拦这个的：一旦有人再把「遍历其他 method 的全部路由」加回来，
// 代价立刻回到 O(路由数)，这条就会红。
TEST(RouterTest, NotFound_LargeRouteTable_HasBoundedScanCost)
{
	constexpr size_t kSmallRouteCount = 50;
	constexpr size_t kLargeRouteCount = 20000;
	constexpr size_t kProbeCount = 1000;

	// 每条路由首段各不相同，探针路径哪个也挨不上
	auto registerBulk = [](Router& router, size_t routeCount)
	{
		for (size_t i = 0; i < routeCount; ++i)
		{
			auto tag = std::to_string(i);
			router.get("/seg" + tag + "/{id}", placeholderHandler());
			router.get("/wseg" + tag + "/*rest", placeholderHandler());
		}
	};

	Router smallRouter;
	Router largeRouter;
	registerBulk(smallRouter, kSmallRouteCount);
	registerBulk(largeRouter, kLargeRouteCount);

	std::vector<std::string> probes;
	probes.reserve(kProbeCount);
	for (size_t i = 0; i < kProbeCount; ++i)
	{
		probes.push_back("/unknown" + std::to_string(i) + "/detail");
	}

	// 先钉住探针走的确实是 404 那个分支，不是 405
	{
		HttpRequest req;
		req.setMethod(HttpMethod::hDelete);
		for (const auto& probe : probes)
		{
			req.setTarget(probe);
			auto result = largeRouter.resolveRoute(req);
			ASSERT_FALSE(result.isMatch()) << "probe=" << probe;
			ASSERT_FALSE(result.isMethodNotAllowed()) << "probe=" << probe;
		}
	}

	// 多轮取最小值，躲开偶发的调度抖动
	auto measure = [&probes](Router& router)
	{
		HttpRequest req;
		req.setMethod(HttpMethod::hDelete);
		auto best = std::chrono::steady_clock::duration::max();
		for (int round = 0; round < 3; ++round)
		{
			auto start = std::chrono::steady_clock::now();
			for (const auto& probe : probes)
			{
				req.setTarget(probe);
				(void)router.resolveRoute(req);
			}
			best = std::min(best, std::chrono::steady_clock::now() - start);
		}
		return best;
	};

	auto smallCost = measure(smallRouter);
	auto largeCost = measure(largeRouter);

	// 为什么只能测耗时：404 的返回结果两条路径一模一样，没有任何可观测的结构差异。
	// 阈值怎么定的：正确实现两条路径都退化成 O(1) 哈希查找，本机实测 1000 次探针都在 100µs 上下
	// （大小表差不到两倍）；把线性扫描塞回去之后实测是 large≈350ms 对 small≈0.8ms，差了两个多数量级。
	// 20 倍斜率 + 20ms 固定余量，对正确实现留出两个数量级的抖动空间，对线性退化则超出近十倍——
	// 两边都不会踩线。
	EXPECT_LT(largeCost, smallCost * 20 + std::chrono::milliseconds(20))
		<< "路由数放大 400 倍后 404 判定耗时也跟着放大（small=" << smallCost.count() << "ns large=" << largeCost.count()
		<< "ns），八成是线性扫描又回来了";
}
