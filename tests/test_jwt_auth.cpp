/**
 * @file test_jwt_auth.cpp
 * @brief JWT Auth 中间件测试（HS256 签发/验证 + SyncBeforeHandler 中间件行为）
 */

#include "core/JwtAuth.h"
#include "core/Middleware.h"
#include "core/HttpRequest.h"
#include "core/HttpResponse.h"
#include "core/HttpTypes.h"
#include <gtest/gtest.h>
#include <chrono>
#include <string>

using namespace hical;

namespace
{

	/// 刚好 32 字节的测试密钥（RFC 7518 §3.2 要求 HS256 密钥 >= 32 字节）
	constexpr std::string_view kTestSecret = "abcdefghijklmnopqrstuvwxyz123456";
	static_assert(kTestSecret.size() == 32);

} // namespace

// ══════════════════════════════════════════════
// 1. jwtSign / jwtVerify 基础 round-trip
// ══════════════════════════════════════════════
TEST(JwtAuthTest, SignAndVerify_RoundTrip_ReturnsPayload)
{
	boost::json::object payload;
	payload["sub"] = "user123";
	payload["role"] = "admin";

	auto token = jwtSign(payload, kTestSecret);

	// Token 应该是三段式：header.payload.signature
	EXPECT_NE(token.find('.'), std::string::npos);

	auto verified = jwtVerify(token, kTestSecret);
	EXPECT_EQ(verified["sub"].as_string(), "user123");
	EXPECT_EQ(verified["role"].as_string(), "admin");

	// 应该自动添加了 iat 和 exp
	EXPECT_TRUE(verified.contains("iat"));
	EXPECT_TRUE(verified.contains("exp"));
}

// ══════════════════════════════════════════════
// 2. 过期 Token 验证失败
// ══════════════════════════════════════════════
TEST(JwtAuthTest, Verify_ExpiredToken_Throws)
{
	// jwtSign 总是用 now + tokenExpiry 设置 exp，
	// 用负的 tokenExpiry 就能签发一个已过期的 token
	JwtAuthOptions opts;
	opts.secret = std::string(kTestSecret);
	opts.tokenExpiry = std::chrono::seconds {-1};

	boost::json::object payload;
	payload["sub"] = "user123";

	auto token = jwtSign(payload, opts);

	// jwtVerify 应该检测到过期并抛异常
	EXPECT_THROW(jwtVerify(token, opts.secret), std::runtime_error);
}

// ══════════════════════════════════════════════
// 3. 篡改 Token（改 payload 不改签名）验证失败
// ══════════════════════════════════════════════
TEST(JwtAuthTest, Verify_TamperedToken_Throws)
{
	boost::json::object payload;
	payload["sub"] = "user123";

	auto token = jwtSign(payload, kTestSecret);

	// 找到第二个点，修改 payload 部分的一个字符
	auto firstDot = token.find('.');
	auto secondDot = token.find('.', firstDot + 1);
	ASSERT_NE(secondDot, std::string::npos);

	// 篡改 payload 中的一个字符
	auto tampered = token;
	auto pos = firstDot + 3; // payload 中间某处
	tampered[pos] = tampered[pos] == 'A' ? 'B' : 'A';

	EXPECT_THROW(jwtVerify(tampered, kTestSecret), std::runtime_error);
}

// ══════════════════════════════════════════════
// 4. 错误密钥验证失败
// ══════════════════════════════════════════════
TEST(JwtAuthTest, Verify_WrongSecret_Throws)
{
	boost::json::object payload;
	payload["sub"] = "user123";

	auto token = jwtSign(payload, kTestSecret);
	EXPECT_THROW(jwtVerify(token, "wrongsecret1234567890abcdef123456"), std::runtime_error);
}

// ══════════════════════════════════════════════
// 5. jwtVerify(opts) 校验 iss/aud
// ══════════════════════════════════════════════
TEST(JwtAuthTest, Verify_Opts_ValidatesIssuerAndAudience)
{
	JwtAuthOptions signOpts;
	signOpts.secret = std::string(kTestSecret);
	signOpts.issuer = "my-api";
	signOpts.audience = "my-app";

	boost::json::object payload;
	payload["sub"] = "user123";

	auto token = jwtSign(payload, signOpts);

	// 用匹配的 opts 验证 —— 应该通过
	JwtAuthOptions verifyOpts;
	verifyOpts.secret = std::string(kTestSecret);
	verifyOpts.issuer = "my-api";
	verifyOpts.audience = "my-app";

	auto verified = jwtVerify(token, verifyOpts);
	EXPECT_EQ(verified["sub"].as_string(), "user123");
	EXPECT_EQ(verified["iss"].as_string(), "my-api");
	EXPECT_EQ(verified["aud"].as_string(), "my-app");
}

// ══════════════════════════════════════════════
// 6. jwtVerify(opts) iss 不匹配抛异常
// ══════════════════════════════════════════════
TEST(JwtAuthTest, Verify_Opts_IssuerMismatch_Throws)
{
	JwtAuthOptions signOpts;
	signOpts.secret = std::string(kTestSecret);
	signOpts.issuer = "my-api";

	auto token = jwtSign({{"sub", "user123"}}, signOpts);

	JwtAuthOptions verifyOpts;
	verifyOpts.secret = std::string(kTestSecret);
	verifyOpts.issuer = "wrong-issuer";

	EXPECT_THROW(jwtVerify(token, verifyOpts), std::runtime_error);
}

// ══════════════════════════════════════════════
// 7. jwtVerify(opts) aud 不匹配抛异常
// ══════════════════════════════════════════════
TEST(JwtAuthTest, Verify_Opts_AudienceMismatch_Throws)
{
	JwtAuthOptions signOpts;
	signOpts.secret = std::string(kTestSecret);
	signOpts.audience = "my-app";

	auto token = jwtSign({{"sub", "user123"}}, signOpts);

	JwtAuthOptions verifyOpts;
	verifyOpts.secret = std::string(kTestSecret);
	verifyOpts.audience = "wrong-audience";

	EXPECT_THROW(jwtVerify(token, verifyOpts), std::runtime_error);
}

// ══════════════════════════════════════════════
// 8. makeJwtAuthMiddleware 拒绝短密钥
// ══════════════════════════════════════════════
TEST(JwtAuthTest, Middleware_ShortSecret_Throws)
{
	JwtAuthOptions opts;
	opts.secret = "too-short";

	EXPECT_THROW(static_cast<void>(makeJwtAuthMiddleware(opts)), std::invalid_argument);
}

// ══════════════════════════════════════════════
// 9. 中间件：白名单路径跳过验证
// ══════════════════════════════════════════════
TEST(JwtAuthTest, Middleware_SkipPath_ReturnsNullopt)
{
	JwtAuthOptions opts;
	opts.secret = std::string(kTestSecret);
	opts.skipPaths = {"/public/health", "/public/metrics"};

	auto middleware = makeJwtAuthMiddleware(opts);

	// 白名单路径 + 无 Authorization 头 → 仍应放行
	HttpRequest req;
	req.setTarget("/public/health");
	auto result = middleware(req);

	EXPECT_FALSE(result.has_value());
}

// ══════════════════════════════════════════════
// 10. 中间件：缺失 Authorization 头返回 401
// ══════════════════════════════════════════════
TEST(JwtAuthTest, Middleware_MissingAuthHeader_Returns401)
{
	JwtAuthOptions opts;
	opts.secret = std::string(kTestSecret);

	auto middleware = makeJwtAuthMiddleware(opts);

	HttpRequest req;
	req.setTarget("/api/test");
	auto result = middleware(req);

	ASSERT_TRUE(result.has_value());
	EXPECT_EQ(result->statusCode(), HttpStatusCode::hUnauthorized);
}

// ══════════════════════════════════════════════
// 11. 中间件：Authorization 非 Bearer 格式返回 401
// ══════════════════════════════════════════════
TEST(JwtAuthTest, Middleware_NonBearerFormat_Returns401)
{
	JwtAuthOptions opts;
	opts.secret = std::string(kTestSecret);

	auto middleware = makeJwtAuthMiddleware(opts);

	HttpRequest req;
	req.setTarget("/api/test");
	req.setHeader("Authorization", "Basic dXNlcjpwYXNz");

	auto result = middleware(req);

	ASSERT_TRUE(result.has_value());
	EXPECT_EQ(result->statusCode(), HttpStatusCode::hUnauthorized);
}

// ══════════════════════════════════════════════
// 12. 中间件：合法 Token 通过并注入 payload 属性
// ══════════════════════════════════════════════
TEST(JwtAuthTest, Middleware_ValidToken_PassesThrough)
{
	JwtAuthOptions opts;
	opts.secret = std::string(kTestSecret);

	// 签发一个合法 token
	boost::json::object payload;
	payload["sub"] = "user123";
	payload["role"] = "admin";
	auto token = jwtSign(payload, opts.secret);

	auto middleware = makeJwtAuthMiddleware(opts);

	HttpRequest req;
	req.setTarget("/api/test");
	req.setHeader("Authorization", std::string("Bearer ") + token);

	auto result = middleware(req);

	// 应该通过（nullopt 表示不拦截）
	EXPECT_FALSE(result.has_value());

	// payload 应该被注入请求属性
	auto attr = req.getAttribute<boost::json::object>("jwt.payload");
	ASSERT_TRUE(attr.has_value());
	EXPECT_EQ((*attr)["sub"].as_string(), "user123");
	EXPECT_EQ((*attr)["role"].as_string(), "admin");
}

// ══════════════════════════════════════════════
// 13. 中间件：无效 Token 返回 401
// ══════════════════════════════════════════════
TEST(JwtAuthTest, Middleware_InvalidToken_Returns401)
{
	JwtAuthOptions opts;
	opts.secret = std::string(kTestSecret);

	auto middleware = makeJwtAuthMiddleware(opts);

	HttpRequest req;
	req.setTarget("/api/test");
	req.setHeader("Authorization", "Bearer invalid.token.here");

	auto result = middleware(req);

	ASSERT_TRUE(result.has_value());
	EXPECT_EQ(result->statusCode(), HttpStatusCode::hUnauthorized);
}

// ══════════════════════════════════════════════
// 14. 中间件：Bearer 后跟 TAB 也能解析
// ══════════════════════════════════════════════
TEST(JwtAuthTest, Middleware_BearerWithTab_PassesThrough)
{
	JwtAuthOptions opts;
	opts.secret = std::string(kTestSecret);

	boost::json::object payload;
	payload["sub"] = "user123";
	auto token = jwtSign(payload, opts.secret);

	auto middleware = makeJwtAuthMiddleware(opts);

	HttpRequest req;
	req.setTarget("/api/test");
	req.setHeader("Authorization", std::string("Bearer\t") + token);

	auto result = middleware(req);
	EXPECT_FALSE(result.has_value());
}

// ══════════════════════════════════════════════
// 15. jwtSign 支持自定义 claims（iss/aud 等标准字段）
// ══════════════════════════════════════════════
TEST(JwtAuthTest, Sign_CustomClaims_Preserved)
{
	boost::json::object payload;
	payload["sub"] = "user456";
	payload["iss"] = "hical";
	payload["aud"] = "api";
	payload["custom_field"] = 42;
	payload["nested"] = boost::json::object {{"key", "value"}};

	auto token = jwtSign(payload, kTestSecret);
	auto verified = jwtVerify(token, kTestSecret);

	EXPECT_EQ(verified["sub"].as_string(), "user456");
	EXPECT_EQ(verified["iss"].as_string(), "hical");
	EXPECT_EQ(verified["aud"].as_string(), "api");
	EXPECT_EQ(verified["custom_field"].as_int64(), 42);
	EXPECT_TRUE(verified.contains("nested"));
}
