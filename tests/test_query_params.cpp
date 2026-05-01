#include "core/HttpRequest.h"
#include <gtest/gtest.h>

using namespace hical;

TEST(QueryParamTest, BasicParams)
{
	HttpRequest req;
	req.setTarget("/api/users?page=1&size=20");

	EXPECT_EQ(req.queryParam("page").value(), "1");
	EXPECT_EQ(req.queryParam("size").value(), "20");
	EXPECT_TRUE(req.hasQueryParam("page"));
	EXPECT_FALSE(req.hasQueryParam("other"));
}

TEST(QueryParamTest, MultipleValues)
{
	HttpRequest req;
	req.setTarget("/search?tag=a&tag=b&tag=c");

	auto& params = req.queryParams();
	EXPECT_EQ(params.count("tag"), 3u);
}

TEST(QueryParamTest, UrlDecodedValues)
{
	HttpRequest req;
	req.setTarget("/search?name=%E4%B8%AD%E6%96%87&space=hello+world");

	auto name = req.queryParam("name");
	ASSERT_TRUE(name.has_value());
	EXPECT_EQ(name.value(), "\xe4\xb8\xad\xe6\x96\x87");

	auto space = req.queryParam("space");
	ASSERT_TRUE(space.has_value());
	EXPECT_EQ(space.value(), "hello world");
}

TEST(QueryParamTest, EmptyValue)
{
	HttpRequest req;
	req.setTarget("/api?key=&other=val");

	EXPECT_TRUE(req.hasQueryParam("key"));
	EXPECT_EQ(req.queryParam("key").value(), "");
	EXPECT_EQ(req.queryParam("other").value(), "val");
}

TEST(QueryParamTest, NoQueryString)
{
	HttpRequest req;
	req.setTarget("/api/users");

	EXPECT_FALSE(req.hasQueryParam("page"));
	EXPECT_TRUE(req.queryParams().empty());
	EXPECT_FALSE(req.queryParam("page").has_value());
}

TEST(QueryParamTest, CachedParsing)
{
	HttpRequest req;
	req.setTarget("/api?a=1&b=2");

	const auto& p1 = req.queryParams();
	const auto& p2 = req.queryParams();
	EXPECT_EQ(&p1, &p2);
}

TEST(QueryParamTest, KeyWithoutEquals)
{
	HttpRequest req;
	req.setTarget("/api?flag&key=val");

	EXPECT_TRUE(req.hasQueryParam("flag"));
	EXPECT_EQ(req.queryParam("flag").value(), "");
	EXPECT_EQ(req.queryParam("key").value(), "val");
}
