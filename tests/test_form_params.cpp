#include "core/HttpRequest.h"
#include <gtest/gtest.h>

using namespace hical;

TEST(FormParamTest, BasicFormParams)
{
	HttpRequest req;
	req.setMethod(HttpMethod::hPost);
	req.setHeader("Content-Type", "application/x-www-form-urlencoded");
	req.setBody("name=John&age=30");

	EXPECT_EQ(req.formParam("name").value(), "John");
	EXPECT_EQ(req.formParam("age").value(), "30");
	EXPECT_TRUE(req.hasFormParam("name"));
	EXPECT_FALSE(req.hasFormParam("other"));
}

TEST(FormParamTest, UrlEncodedValues)
{
	HttpRequest req;
	req.setMethod(HttpMethod::hPost);
	req.setHeader("Content-Type", "application/x-www-form-urlencoded");
	req.setBody("msg=hello+world&special=%26%3D");

	EXPECT_EQ(req.formParam("msg").value(), "hello world");
	EXPECT_EQ(req.formParam("special").value(), "&=");
}

TEST(FormParamTest, WrongContentType)
{
	HttpRequest req;
	req.setMethod(HttpMethod::hPost);
	req.setHeader("Content-Type", "application/json");
	req.setBody("name=John");

	EXPECT_FALSE(req.hasFormParam("name"));
	EXPECT_TRUE(req.formParams().empty());
}

TEST(FormParamTest, EmptyBody)
{
	HttpRequest req;
	req.setMethod(HttpMethod::hPost);
	req.setHeader("Content-Type", "application/x-www-form-urlencoded");

	EXPECT_TRUE(req.formParams().empty());
}

TEST(FormParamTest, ContentTypeWithCharset)
{
	HttpRequest req;
	req.setMethod(HttpMethod::hPost);
	req.setHeader("Content-Type", "application/x-www-form-urlencoded; charset=UTF-8");
	req.setBody("key=value");

	EXPECT_EQ(req.formParam("key").value(), "value");
}

TEST(FormParamTest, IndependentOfQueryParams)
{
	HttpRequest req;
	req.setMethod(HttpMethod::hPost);
	req.setTarget("/api?q=search");
	req.setHeader("Content-Type", "application/x-www-form-urlencoded");
	req.setBody("field=data");

	EXPECT_EQ(req.queryParam("q").value(), "search");
	EXPECT_FALSE(req.hasQueryParam("field"));
	EXPECT_EQ(req.formParam("field").value(), "data");
	EXPECT_FALSE(req.hasFormParam("q"));
}

TEST(FormParamTest, CachedParsing)
{
	HttpRequest req;
	req.setMethod(HttpMethod::hPost);
	req.setHeader("Content-Type", "application/x-www-form-urlencoded");
	req.setBody("a=1&b=2");

	const auto& p1 = req.formParams();
	const auto& p2 = req.formParams();
	EXPECT_EQ(&p1, &p2);
}
