/**
 * @file test_helmet.cpp
 * @brief 安全头中间件 Helmet 单元测试
 */

#include "core/Helmet.h"
#include "core/Middleware.h"
#include "core/HttpRequest.h"
#include "core/HttpResponse.h"
#include <gtest/gtest.h>
#include <string>

using namespace hical;

// ──────────────────────────────────────────────
// 1. 默认配置：所有安全头都存在
// ──────────────────────────────────────────────
TEST(HelmetTest, DefaultHeaders_AllPresent)
{
	auto middleware = makeHelmetMiddleware();

	HttpRequest req;
	HttpResponse res;

	// 模拟路由处理器设置了 body，触发 Content-Length
	res.setBody("hello", "text/plain");

	middleware(req, res);

	EXPECT_EQ(res.header("X-Content-Type-Options"), "nosniff");
	EXPECT_EQ(res.header("X-Frame-Options"), "DENY");
	EXPECT_EQ(res.header("Strict-Transport-Security"), "max-age=31536000; includeSubDomains");
	EXPECT_EQ(res.header("X-XSS-Protection"), "0");
	EXPECT_EQ(res.header("Content-Security-Policy"), "default-src 'self'");
	EXPECT_EQ(res.header("Referrer-Policy"), "strict-origin-when-cross-origin");
	EXPECT_EQ(res.header("Permissions-Policy"), "geolocation=(), microphone=(), camera=()");
}

// ──────────────────────────────────────────────
// 2. 关闭单个选项后对应头消失
// ──────────────────────────────────────────────
TEST(HelmetTest, DisableContentTypeNosniff_HeaderAbsent)
{
	auto middleware = makeHelmetMiddleware(HelmetOptions {
		.contentTypeNosniff = false,
		.customHeaders = {},
	});

	HttpRequest req;
	HttpResponse res;
	res.setBody("hello", "text/plain");

	middleware(req, res);

	EXPECT_EQ(res.header("X-Content-Type-Options"), "");
	// 其他头仍在
	EXPECT_EQ(res.header("X-Frame-Options"), "DENY");
	EXPECT_EQ(res.header("Strict-Transport-Security"), "max-age=31536000; includeSubDomains");
}

TEST(HelmetTest, DisableFrameDeny_HeaderAbsent)
{
	auto middleware = makeHelmetMiddleware(HelmetOptions {
		.frameDeny = false,
		.customHeaders = {},
	});

	HttpRequest req;
	HttpResponse res;
	res.setBody("hello", "text/plain");

	middleware(req, res);

	EXPECT_EQ(res.header("X-Frame-Options"), "");
	EXPECT_EQ(res.header("X-Content-Type-Options"), "nosniff");
}

TEST(HelmetTest, DisableHsts_HeaderAbsent)
{
	auto middleware = makeHelmetMiddleware(HelmetOptions {
		.hsts = false,
		.customHeaders = {},
	});

	HttpRequest req;
	HttpResponse res;
	res.setBody("hello", "text/plain");

	middleware(req, res);

	EXPECT_EQ(res.header("Strict-Transport-Security"), "");
	EXPECT_EQ(res.header("X-Content-Type-Options"), "nosniff");
}

TEST(HelmetTest, DisableXssProtection_HeaderAbsent)
{
	auto middleware = makeHelmetMiddleware(HelmetOptions {
		.xssProtection = false,
		.customHeaders = {},
	});

	HttpRequest req;
	HttpResponse res;
	res.setBody("hello", "text/plain");

	middleware(req, res);

	EXPECT_EQ(res.header("X-XSS-Protection"), "");
}

// ──────────────────────────────────────────────
// 3. 设置策略值为空 = 不设置对应头
// ──────────────────────────────────────────────
TEST(HelmetTest, EmptyCsp_HeaderAbsent)
{
	auto middleware = makeHelmetMiddleware(HelmetOptions {
		.csp = "",
		.customHeaders = {},
	});

	HttpRequest req;
	HttpResponse res;
	res.setBody("hello", "text/plain");

	middleware(req, res);

	EXPECT_EQ(res.header("Content-Security-Policy"), "");
}

TEST(HelmetTest, EmptyPermissionsPolicy_HeaderAbsent)
{
	auto middleware = makeHelmetMiddleware(HelmetOptions {
		.permissionsPolicy = "",
		.customHeaders = {},
	});

	HttpRequest req;
	HttpResponse res;
	res.setBody("hello", "text/plain");

	middleware(req, res);

	EXPECT_EQ(res.header("Permissions-Policy"), "");
}

// ──────────────────────────────────────────────
// 4. 自定义 CustomHeaders 能正确添加
// ──────────────────────────────────────────────
TEST(HelmetTest, CustomHeaders_AddedCorrectly)
{
	auto middleware = makeHelmetMiddleware(HelmetOptions {
		.customHeaders =
			{
				{"Cross-Origin-Embedder-Policy", "require-corp"},
				{"Cross-Origin-Opener-Policy", "same-origin"},
			},
	});

	HttpRequest req;
	HttpResponse res;
	res.setBody("hello", "text/plain");

	middleware(req, res);

	EXPECT_EQ(res.header("Cross-Origin-Embedder-Policy"), "require-corp");
	EXPECT_EQ(res.header("Cross-Origin-Opener-Policy"), "same-origin");
	// 默认头仍在
	EXPECT_EQ(res.header("X-Content-Type-Options"), "nosniff");
}

// ──────────────────────────────────────────────
// 5. 自定义 CSP 能正确覆盖
// ──────────────────────────────────────────────
TEST(HelmetTest, CustomCsp_OverridesDefault)
{
	auto middleware = makeHelmetMiddleware(HelmetOptions {
		.csp = "default-src 'self'; img-src https://trusted.cdn.com",
		.customHeaders = {},
	});

	HttpRequest req;
	HttpResponse res;
	res.setBody("hello", "text/plain");

	middleware(req, res);

	EXPECT_EQ(res.header("Content-Security-Policy"), "default-src 'self'; img-src https://trusted.cdn.com");
}

// ──────────────────────────────────────────────
// 6. 全部关闭 = 只有自定义头
// ──────────────────────────────────────────────
TEST(HelmetTest, AllDisabled_OnlyCustomHeaders)
{
	auto middleware = makeHelmetMiddleware(HelmetOptions {
		.contentTypeNosniff = false,
		.frameDeny = false,
		.hsts = false,
		.xssProtection = false,
		.csp = "",
		.referrerPolicy = "",
		.permissionsPolicy = "",
		.customHeaders = {{"X-Custom", "value"}},
	});

	HttpRequest req;
	HttpResponse res;
	res.setBody("hello", "text/plain");

	middleware(req, res);

	// 安全头全缺
	EXPECT_EQ(res.header("X-Content-Type-Options"), "");
	EXPECT_EQ(res.header("X-Frame-Options"), "");
	EXPECT_EQ(res.header("Strict-Transport-Security"), "");
	EXPECT_EQ(res.header("X-XSS-Protection"), "");
	EXPECT_EQ(res.header("Content-Security-Policy"), "");
	EXPECT_EQ(res.header("Referrer-Policy"), "");
	EXPECT_EQ(res.header("Permissions-Policy"), "");

	// 自定义头存在
	EXPECT_EQ(res.header("X-Custom"), "value");
}

// ──────────────────────────────────────────────
// 7. 不修改 Content-Length（对 body 无副作用）
// ──────────────────────────────────────────────
TEST(HelmetTest, DoesNotModifyBody_ContentLengthPreserved)
{
	auto middleware = makeHelmetMiddleware();

	HttpRequest req;
	HttpResponse res;
	res.setBody("{\"key\":\"value\"}", "application/json");

	auto bodyBefore = res.body();
	middleware(req, res);
	auto bodyAfter = res.body();

	EXPECT_EQ(bodyBefore, bodyAfter);
}
