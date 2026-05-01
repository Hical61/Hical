#include "core/HttpResponse.h"
#include <gtest/gtest.h>

using namespace hical;

TEST(RedirectTest, DefaultStatusIs302)
{
	auto res = HttpResponse::redirect("/new-url");

	EXPECT_EQ(res.statusCode(), HttpStatusCode::hFound);
	EXPECT_EQ(res.header("Location"), "/new-url");
}

TEST(RedirectTest, Status301)
{
	auto res = HttpResponse::redirect("/permanent", HttpStatusCode::hMovedPermanently);

	EXPECT_EQ(res.statusCode(), HttpStatusCode::hMovedPermanently);
	EXPECT_EQ(res.header("Location"), "/permanent");
}

TEST(RedirectTest, Status307)
{
	auto res = HttpResponse::redirect("/temp", HttpStatusCode::hTemporaryRedirect);

	EXPECT_EQ(res.statusCode(), HttpStatusCode::hTemporaryRedirect);
	EXPECT_EQ(res.header("Location"), "/temp");
}

TEST(RedirectTest, Status308)
{
	auto res = HttpResponse::redirect("/perm", HttpStatusCode::hPermanentRedirect);

	EXPECT_EQ(res.statusCode(), HttpStatusCode::hPermanentRedirect);
	EXPECT_EQ(res.header("Location"), "/perm");
}

TEST(RedirectTest, CRLFInjectionRejected)
{
	// Location 含 \r\n 时 setHeader 静默忽略，Location 头不存在
	auto res = HttpResponse::redirect("/evil\r\nSet-Cookie: injected=1");

	EXPECT_EQ(res.statusCode(), HttpStatusCode::hFound);
	EXPECT_EQ(res.header("Location"), "");
}

TEST(RedirectTest, AbsoluteUrl)
{
	auto res = HttpResponse::redirect("https://example.com/page");

	EXPECT_EQ(res.header("Location"), "https://example.com/page");
}
