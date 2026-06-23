/**
 * @file test_health.cpp
 * @brief 健康检查端点注册与行为测试
 */

#include "core/HealthEndpoint.h"
#include "core/Router.h"
#include "core/HttpRequest.h"
#include "core/HttpTypes.h"
#include <gtest/gtest.h>
#include <string>

using namespace hical;

// ──────────────────────────────────────────────
// 1. 注册不抛异常
// ──────────────────────────────────────────────
TEST(HealthEndpointTest, RegisterEndpoints_NoThrow)
{
	Router router;
	EXPECT_NO_THROW(registerHealthEndpoints(router));
}

TEST(HealthEndpointTest, RegisterWithCustomPrefix_NoThrow)
{
	Router router;
	EXPECT_NO_THROW(registerHealthEndpoints(router, {.prefix = "/api/v1", .readyCheck = nullptr}));
}

// ──────────────────────────────────────────────
// 2. GET /health 返回 200 + status: ok
// ──────────────────────────────────────────────
TEST(HealthEndpointTest, HealthEndpoint_Returns200_WithStatusOk)
{
	Router router;
	registerHealthEndpoints(router);

	HttpRequest req;
	req.setMethod(hical::HttpMethod::hGet);
	req.setTarget("/admin/health");

	auto result = router.dispatchSync(req);
	ASSERT_TRUE(result.has_value());
	EXPECT_EQ(result->statusCode(), HttpStatusCode::hOk);

	// body 应包含 "status":"ok"
	auto body = result->body();
	EXPECT_NE(body.find("\"ok\""), std::string::npos);
	EXPECT_NE(body.find("\"status\""), std::string::npos);
}

// ──────────────────────────────────────────────
// 3. GET /ready 默认返回 200 + status: ready
// ──────────────────────────────────────────────
TEST(HealthEndpointTest, ReadyEndpoint_Default_Returns200)
{
	Router router;
	registerHealthEndpoints(router);

	HttpRequest req;
	req.setMethod(hical::HttpMethod::hGet);
	req.setTarget("/admin/ready");

	auto result = router.dispatchSync(req);
	ASSERT_TRUE(result.has_value());
	EXPECT_EQ(result->statusCode(), HttpStatusCode::hOk);

	auto body = result->body();
	EXPECT_NE(body.find("\"ready\""), std::string::npos);
}

// ──────────────────────────────────────────────
// 4. readyCheck 返回 false 时 /ready 返回 503
// ──────────────────────────────────────────────
TEST(HealthEndpointTest, ReadyEndpoint_CheckFalse_Returns503)
{
	Router router;
	registerHealthEndpoints(router,
							{.readyCheck = []() -> bool
							 {
								 return false;
							 }});

	HttpRequest req;
	req.setMethod(hical::HttpMethod::hGet);
	req.setTarget("/admin/ready");

	auto result = router.dispatchSync(req);
	ASSERT_TRUE(result.has_value());
	EXPECT_EQ(result->statusCode(), HttpStatusCode::hServiceUnavailable);

	auto body = result->body();
	EXPECT_NE(body.find("\"not ready\""), std::string::npos);
}

// ──────────────────────────────────────────────
// 5. readyCheck 返回 true 时 /ready 返回 200
// ──────────────────────────────────────────────
TEST(HealthEndpointTest, ReadyEndpoint_CheckTrue_Returns200)
{
	Router router;
	registerHealthEndpoints(router,
							{.readyCheck = []() -> bool
							 {
								 return true;
							 }});

	HttpRequest req;
	req.setMethod(hical::HttpMethod::hGet);
	req.setTarget("/admin/ready");

	auto result = router.dispatchSync(req);
	ASSERT_TRUE(result.has_value());
	EXPECT_EQ(result->statusCode(), HttpStatusCode::hOk);

	auto body = result->body();
	EXPECT_NE(body.find("\"ready\""), std::string::npos);
}

// ──────────────────────────────────────────────
// 6. 自定义前缀时端点在正确路径
// ──────────────────────────────────────────────
TEST(HealthEndpointTest, CustomPrefix_EndpointsAtCorrectPath)
{
	Router router;
	registerHealthEndpoints(router, {.prefix = "/api/v1", .readyCheck = nullptr});

	// health 位于 /api/v1/health
	{
		HttpRequest req;
		req.setMethod(hical::HttpMethod::hGet);
		req.setTarget("/api/v1/health");
		auto result = router.dispatchSync(req);
		ASSERT_TRUE(result.has_value());
		EXPECT_EQ(result->statusCode(), HttpStatusCode::hOk);
	}

	// ready 位于 /api/v1/ready
	{
		HttpRequest req;
		req.setMethod(hical::HttpMethod::hGet);
		req.setTarget("/api/v1/ready");
		auto result = router.dispatchSync(req);
		ASSERT_TRUE(result.has_value());
		EXPECT_EQ(result->statusCode(), HttpStatusCode::hOk);
	}

	// 默认前缀路径应返回 404
	{
		HttpRequest req;
		req.setMethod(hical::HttpMethod::hGet);
		req.setTarget("/admin/health");
		auto result = router.dispatchSync(req);
		EXPECT_FALSE(result.has_value()) << "default path should not be registered";
	}
}
