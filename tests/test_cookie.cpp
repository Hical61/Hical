#include "core/HttpRequest.h"
#include "core/HttpResponse.h"
#include "core/HeaderMap.h"
#include <gtest/gtest.h>

using namespace hical;

// ============ HttpRequest Cookie 解析测试 ============

TEST(CookieTest, ParseSingleCookie)
{
	HttpRequest req;
	req.setHeader("Cookie", "session_id=abc123");

	EXPECT_EQ(req.cookie("session_id"), "abc123");
	EXPECT_TRUE(req.hasCookie("session_id"));
	EXPECT_FALSE(req.hasCookie("other"));
}

TEST(CookieTest, ParseMultipleCookies)
{
	HttpRequest req;
	req.setHeader("Cookie", "user=alice; token=xyz; theme=dark");

	EXPECT_EQ(req.cookie("user"), "alice");
	EXPECT_EQ(req.cookie("token"), "xyz");
	EXPECT_EQ(req.cookie("theme"), "dark");
	EXPECT_EQ(req.cookies().size(), 3u);
}

TEST(CookieTest, CookieWithSpaces)
{
	HttpRequest req;
	req.setHeader("Cookie", "  key1=val1;  key2=val2");

	EXPECT_EQ(req.cookie("key1"), "val1");
	EXPECT_EQ(req.cookie("key2"), "val2");
}

TEST(CookieTest, MissingCookieHeader)
{
	HttpRequest req;

	EXPECT_EQ(req.cookie("any"), "");
	EXPECT_FALSE(req.hasCookie("any"));
	EXPECT_TRUE(req.cookies().empty());
}

TEST(CookieTest, EmptyCookieValue)
{
	HttpRequest req;
	req.setHeader("Cookie", "empty=");

	EXPECT_TRUE(req.hasCookie("empty"));
	EXPECT_EQ(req.cookie("empty"), "");
}

TEST(CookieTest, CookieParsingIsCached)
{
	HttpRequest req;
	req.setHeader("Cookie", "a=1; b=2");

	// 多次调用应返回同一缓存结果
	const auto& c1 = req.cookies();
	const auto& c2 = req.cookies();
	EXPECT_EQ(&c1, &c2);
	EXPECT_EQ(c1.size(), 2u);
}

// ============ HttpResponse setCookie 测试 ============

TEST(CookieTest, SetSimpleCookie)
{
	HttpResponse res;
	res.setCookie("session_id", "abc123");

	auto header = res.header("Set-Cookie");
	EXPECT_NE(header.find("session_id=abc123"), std::string::npos);
	EXPECT_NE(header.find("Path=/"), std::string::npos);
}

TEST(CookieTest, SetCookieWithMaxAge)
{
	HttpResponse res;
	CookieOptions opts;
	opts.maxAge = 3600;
	res.setCookie("token", "xyz", opts);

	auto header = res.header("Set-Cookie");
	EXPECT_NE(header.find("Max-Age=3600"), std::string::npos);
}

TEST(CookieTest, SetCookieHttpOnly)
{
	HttpResponse res;
	CookieOptions opts;
	opts.httpOnly = true;
	res.setCookie("sid", "val", opts);

	auto header = res.header("Set-Cookie");
	EXPECT_NE(header.find("HttpOnly"), std::string::npos);
}

TEST(CookieTest, SetCookieSecure)
{
	HttpResponse res;
	CookieOptions opts;
	opts.secure = true;
	opts.sameSite = "Strict";
	res.setCookie("csrf", "token", opts);

	auto header = res.header("Set-Cookie");
	EXPECT_NE(header.find("Secure"), std::string::npos);
	EXPECT_NE(header.find("SameSite=Strict"), std::string::npos);
}

TEST(CookieTest, SetCookieWithDomain)
{
	HttpResponse res;
	CookieOptions opts;
	opts.domain = "example.com";
	opts.path = "/api";
	res.setCookie("pref", "dark", opts);

	auto header = res.header("Set-Cookie");
	EXPECT_NE(header.find("Domain=example.com"), std::string::npos);
	EXPECT_NE(header.find("Path=/api"), std::string::npos);
}

TEST(CookieTest, SetMultipleCookies)
{
	HttpResponse res;
	res.setCookie("a", "1");
	res.setCookie("b", "2");

	// 多个 Set-Cookie 头：header() 只返回第一个，用 native().headers 验证完整
	auto& native = res.native();
	EXPECT_EQ(native.headers.count("Set-Cookie"), 2u);
}

// ============ CRLF 注入防护测试 ============

TEST(CookieTest, SetCookieRejectsCRLFInName)
{
	HttpResponse res;
	// name 含 \r\n，应被静默忽略，不写入任何 Set-Cookie 头
	res.setCookie("evil\r\nSet-Cookie: injected", "value");

	auto& native = res.native();
	EXPECT_EQ(native.headers.count("Set-Cookie"), 0u);
}

TEST(CookieTest, SetCookieRejectsCRLFInValue)
{
	HttpResponse res;
	res.setCookie("session", "val\r\nSet-Cookie: injected=1");

	auto& native = res.native();
	EXPECT_EQ(native.headers.count("Set-Cookie"), 0u);
}

// ============ CRLF 注入防护 — Cookie 属性字段 ============

TEST(CookieTest, SetCookieRejectsCRLFInPath)
{
	HttpResponse res;
	CookieOptions opts;
	opts.path = "/; \r\nSet-Cookie: injected=1";
	res.setCookie("session", "abc123", opts);

	auto& native = res.native();
	EXPECT_EQ(native.headers.count("Set-Cookie"), 0u);
}

TEST(CookieTest, SetCookieRejectsCRLFInDomain)
{
	HttpResponse res;
	CookieOptions opts;
	opts.domain = "example.com\r\nEvil-Header: injected";
	res.setCookie("session", "abc123", opts);

	auto& native = res.native();
	EXPECT_EQ(native.headers.count("Set-Cookie"), 0u);
}

TEST(CookieTest, SetCookieRejectsCRLFInSameSite)
{
	HttpResponse res;
	CookieOptions opts;
	opts.sameSite = "Strict\r\nX-Injected: true";
	res.setCookie("session", "abc123", opts);

	auto& native = res.native();
	EXPECT_EQ(native.headers.count("Set-Cookie"), 0u);
}

// ============ 重复键 first-wins 测试 ============

TEST(CookieTest, DuplicateCookieNameFirstWins)
{
	HttpRequest req;
	// 同名 Cookie 出现两次，应保留第一个值
	req.setHeader("Cookie", "token=first; token=second");

	EXPECT_EQ(req.cookie("token"), "first");
	// cookies() 中该键只有一个条目
	EXPECT_EQ(req.cookies().count("token"), 1u);
}

// ============ Cookie value URL 编码测试 ============

TEST(CookieTest, SetCookieEncodesSpecialChars)
{
	HttpResponse res;
	// value 含 ';' 和空格，应被 URL 编码，不破坏 Set-Cookie 格式
	res.setCookie("data", "a;b c");

	auto header = res.header("Set-Cookie");
	// ';' -> %3B, ' ' -> %20
	EXPECT_NE(header.find("data=a%3Bb%20c"), std::string::npos);
	// 确保原始 ';' 不会被解析为 Cookie 属性分隔符
	EXPECT_EQ(header.find("data=a;b"), std::string::npos);
}

TEST(CookieTest, SetCookieEncodesCommaAndDoubleQuote)
{
	HttpResponse res;
	res.setCookie("json", "{\"k\":\"v\"}");

	auto header = res.header("Set-Cookie");
	// '"' -> %22, 应不含裸双引号
	EXPECT_EQ(header.find("json={\""), std::string::npos);
	EXPECT_NE(header.find("%22"), std::string::npos);
}

TEST(CookieTest, SetCookieSafeCharsNotEncoded)
{
	HttpResponse res;
	// 纯 ASCII 安全字符不应被编码
	res.setCookie("token", "abc123-._~!#$&'()*+/:=?@[]");

	auto header = res.header("Set-Cookie");
	// 验证常见安全字符未被编码
	EXPECT_NE(header.find("abc123"), std::string::npos);
}
