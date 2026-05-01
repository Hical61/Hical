#include "core/Cors.h"
#include "core/Middleware.h"
#include "asio/AsioEventLoop.h"
#include <gtest/gtest.h>

using namespace hical;

// 辅助：在事件循环中运行协程（与 test_middleware.cpp 保持一致）
template <typename F>
auto runCoroutine(F&& f)
{
	using ReturnType = typename std::invoke_result_t<F>::value_type;

	boost::asio::io_context ioCtx;
	std::optional<ReturnType> result;

	coSpawn(ioCtx,
			[&]() -> Awaitable<void>
			{
				result = co_await f();
			});

	ioCtx.run();
	return result;
}

// 辅助：执行单个 CORS 中间件，最终 handler 返回 200 "ok"
static std::optional<HttpResponse> runCors(const CorsOptions& opts,
										   HttpRequest& req,
										   std::function<HttpResponse()> finalHandler = nullptr)
{
	MiddlewarePipeline pipeline;
	pipeline.use(makeCorsMiddleware(opts));

	return runCoroutine(
		[&]()
		{
			return pipeline.execute(req,
									[finalHandler = std::move(finalHandler)](HttpRequest&) -> Awaitable<HttpResponse>
									{
										if (finalHandler)
										{
											co_return finalHandler();
										}
										co_return HttpResponse::ok("ok");
									});
		});
}

// ──────────────────────────────────────────────
// 1. 普通跨域请求附加 Allow-Origin 头
// ──────────────────────────────────────────────
TEST(CorsTest, SimpleRequestWithOrigin)
{
	HttpRequest req;
	req.setMethod(HttpMethod::hGet);
	req.setTarget("/api/data");
	req.setHeader("Origin", "https://example.com");

	auto result = runCors({}, req);

	ASSERT_TRUE(result.has_value());
	EXPECT_EQ(result->header("Access-Control-Allow-Origin"), "*");
}

// ──────────────────────────────────────────────
// 2. OPTIONS preflight 返回 204 及完整预检头
// ──────────────────────────────────────────────
TEST(CorsTest, PreflightReturns204)
{
	HttpRequest req;
	req.setMethod(HttpMethod::hOptions);
	req.setTarget("/api/data");
	req.setHeader("Origin", "https://example.com");
	req.setHeader("Access-Control-Request-Method", "POST");

	auto result = runCors({}, req);

	ASSERT_TRUE(result.has_value());
	EXPECT_EQ(result->statusCode(), HttpStatusCode::hNoContent);
	EXPECT_EQ(result->header("Access-Control-Allow-Origin"), "*");
	EXPECT_FALSE(result->header("Access-Control-Allow-Methods").empty());
	EXPECT_FALSE(result->header("Access-Control-Allow-Headers").empty());
	EXPECT_EQ(result->header("Access-Control-Max-Age"), "86400");
}

// ──────────────────────────────────────────────
// 3. preflight 不调用 next（handler 永远不会被执行）
// ──────────────────────────────────────────────
TEST(CorsTest, PreflightDoesNotCallNext)
{
	bool handlerCalled = false;

	HttpRequest req;
	req.setMethod(HttpMethod::hOptions);
	req.setTarget("/api/data");
	req.setHeader("Origin", "https://example.com");
	req.setHeader("Access-Control-Request-Method", "POST");

	MiddlewarePipeline pipeline;
	pipeline.use(makeCorsMiddleware({}));

	runCoroutine(
		[&]()
		{
			return pipeline.execute(req,
									[&handlerCalled](HttpRequest&) -> Awaitable<HttpResponse>
									{
										handlerCalled = true;
										co_return HttpResponse::ok("should not reach");
									});
		});

	EXPECT_FALSE(handlerCalled);
}

// ──────────────────────────────────────────────
// 3b. 不带 Access-Control-Request-Method 的 OPTIONS 不是 preflight，应透传到 handler
// ──────────────────────────────────────────────
TEST(CorsTest, NonPreflightOptionsPassthrough)
{
	bool handlerCalled = false;

	HttpRequest req;
	req.setMethod(HttpMethod::hOptions);
	req.setTarget("/api/data");
	req.setHeader("Origin", "https://example.com");
	// 不设置 Access-Control-Request-Method → 非 preflight

	MiddlewarePipeline pipeline;
	pipeline.use(makeCorsMiddleware({}));

	auto result = runCoroutine(
		[&]()
		{
			return pipeline.execute(req,
									[&handlerCalled](HttpRequest&) -> Awaitable<HttpResponse>
									{
										handlerCalled = true;
										co_return HttpResponse::ok("options response");
									});
		});

	EXPECT_TRUE(handlerCalled);
	ASSERT_TRUE(result.has_value());
	EXPECT_EQ(result->statusCode(), HttpStatusCode::hOk);
	// 仍应附加 CORS 头
	EXPECT_EQ(result->header("Access-Control-Allow-Origin"), "*");
}

// ──────────────────────────────────────────────
// 4. Origin 不在白名单时不附加 CORS 头
// ──────────────────────────────────────────────
TEST(CorsTest, OriginNotInWhitelist)
{
	CorsOptions opts;
	opts.allowedOrigins = {"https://trusted.com"};

	HttpRequest req;
	req.setMethod(HttpMethod::hGet);
	req.setTarget("/api/data");
	req.setHeader("Origin", "https://evil.com");

	auto result = runCors(opts, req);

	ASSERT_TRUE(result.has_value());
	EXPECT_TRUE(result->header("Access-Control-Allow-Origin").empty());
}

// ──────────────────────────────────────────────
// 5. allowCredentials=true 时回显具体 origin，不用 "*"
// ──────────────────────────────────────────────
TEST(CorsTest, CredentialsWithSpecificOrigin)
{
	CorsOptions opts;
	opts.allowCredentials = true;
	opts.allowedOrigins = {"https://app.example.com"};

	HttpRequest req;
	req.setMethod(HttpMethod::hGet);
	req.setTarget("/api/data");
	req.setHeader("Origin", "https://app.example.com");

	auto result = runCors(opts, req);

	ASSERT_TRUE(result.has_value());
	EXPECT_EQ(result->header("Access-Control-Allow-Origin"), "https://app.example.com");
	EXPECT_EQ(result->header("Access-Control-Allow-Credentials"), "true");
}

// ──────────────────────────────────────────────
// 5b. allowCredentials=true + wildcard 应在创建时抛异常
// ──────────────────────────────────────────────
TEST(CorsTest, CredentialsWithWildcardThrows)
{
	CorsOptions opts;
	opts.allowCredentials = true;
	// 默认 allowedOrigins = {"*"}，应被拒绝

	EXPECT_THROW(makeCorsMiddleware(opts), std::invalid_argument);
}

// ──────────────────────────────────────────────
// 6. 无 Origin 头时直接透传，不附加 CORS 头
// ──────────────────────────────────────────────
TEST(CorsTest, NoOriginHeaderPassthrough)
{
	HttpRequest req;
	req.setMethod(HttpMethod::hGet);
	req.setTarget("/api/data");
	// 不设置 Origin 头

	auto result = runCors({}, req);

	ASSERT_TRUE(result.has_value());
	EXPECT_TRUE(result->header("Access-Control-Allow-Origin").empty());
	EXPECT_EQ(result->body(), "ok");
}

// ──────────────────────────────────────────────
// 7. 非通配符模式附加 Vary: Origin
// ──────────────────────────────────────────────
TEST(CorsTest, VaryOriginHeaderWhenNotWildcard)
{
	CorsOptions opts;
	opts.allowedOrigins = {"https://trusted.com"};

	HttpRequest req;
	req.setMethod(HttpMethod::hGet);
	req.setTarget("/api/data");
	req.setHeader("Origin", "https://trusted.com");

	auto result = runCors(opts, req);

	ASSERT_TRUE(result.has_value());
	EXPECT_EQ(result->header("Vary"), "Origin");
}

// ──────────────────────────────────────────────
// 8. 通配符模式不附加 Vary: Origin
// ──────────────────────────────────────────────
TEST(CorsTest, NoVaryOriginHeaderWhenWildcard)
{
	HttpRequest req;
	req.setMethod(HttpMethod::hGet);
	req.setTarget("/api/data");
	req.setHeader("Origin", "https://anyone.com");

	auto result = runCors({}, req);

	ASSERT_TRUE(result.has_value());
	EXPECT_TRUE(result->header("Vary").empty());
}

// ──────────────────────────────────────────────
// 9. exposeHeaders 正确写入响应头
// ──────────────────────────────────────────────
TEST(CorsTest, ExposeHeaders)
{
	CorsOptions opts;
	opts.exposeHeaders = {"X-Custom-Header", "X-Request-Id"};

	HttpRequest req;
	req.setMethod(HttpMethod::hGet);
	req.setTarget("/api/data");
	req.setHeader("Origin", "https://example.com");

	auto result = runCors(opts, req);

	ASSERT_TRUE(result.has_value());
	EXPECT_EQ(result->header("Access-Control-Expose-Headers"), "X-Custom-Header, X-Request-Id");
}

// ──────────────────────────────────────────────
// 10. preflight 中 Vary: Origin 非通配符也存在
// ──────────────────────────────────────────────
TEST(CorsTest, PreflightVaryOriginHeaderWhenNotWildcard)
{
	CorsOptions opts;
	opts.allowedOrigins = {"https://trusted.com"};

	HttpRequest req;
	req.setMethod(HttpMethod::hOptions);
	req.setTarget("/api/data");
	req.setHeader("Origin", "https://trusted.com");
	req.setHeader("Access-Control-Request-Method", "PUT");

	auto result = runCors(opts, req);

	ASSERT_TRUE(result.has_value());
	EXPECT_EQ(result->statusCode(), HttpStatusCode::hNoContent);
	EXPECT_EQ(result->header("Vary"), "Origin");
}

// ──────────────────────────────────────────────
// 11. 自定义 maxAge 写入预检响应
// ──────────────────────────────────────────────
TEST(CorsTest, CustomMaxAge)
{
	CorsOptions opts;
	opts.maxAge = 3600;

	HttpRequest req;
	req.setMethod(HttpMethod::hOptions);
	req.setTarget("/api/data");
	req.setHeader("Origin", "https://example.com");
	req.setHeader("Access-Control-Request-Method", "DELETE");

	auto result = runCors(opts, req);

	ASSERT_TRUE(result.has_value());
	EXPECT_EQ(result->header("Access-Control-Max-Age"), "3600");
}

// ──────────────────────────────────────────────
// 12. 精确 origin 匹配：白名单多个 origin
// ──────────────────────────────────────────────
TEST(CorsTest, MultipleAllowedOriginsMatch)
{
	CorsOptions opts;
	opts.allowedOrigins = {"https://a.com", "https://b.com", "https://c.com"};

	HttpRequest req;
	req.setMethod(HttpMethod::hGet);
	req.setTarget("/api/data");
	req.setHeader("Origin", "https://b.com");

	auto result = runCors(opts, req);

	ASSERT_TRUE(result.has_value());
	EXPECT_EQ(result->header("Access-Control-Allow-Origin"), "https://b.com");
}

// ──────────────────────────────────────────────
// 13. credentials=true 时 preflight 也回显 origin 并带 credentials 头
// ──────────────────────────────────────────────
TEST(CorsTest, PreflightCredentials)
{
	CorsOptions opts;
	opts.allowCredentials = true;
	opts.allowedOrigins = {"https://app.example.com"};

	HttpRequest req;
	req.setMethod(HttpMethod::hOptions);
	req.setTarget("/api/data");
	req.setHeader("Origin", "https://app.example.com");
	req.setHeader("Access-Control-Request-Method", "POST");

	auto result = runCors(opts, req);

	ASSERT_TRUE(result.has_value());
	EXPECT_EQ(result->statusCode(), HttpStatusCode::hNoContent);
	EXPECT_EQ(result->header("Access-Control-Allow-Origin"), "https://app.example.com");
	EXPECT_EQ(result->header("Access-Control-Allow-Credentials"), "true");
}
