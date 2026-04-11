#include "core/HttpRequest.h"
#include "core/HttpResponse.h"
#include "core/StaticFiles.h"
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <random>
#include <string>

using namespace hical;
namespace fs = std::filesystem;

// ============ 测试辅助：创建临时目录 ============

static std::string uniqueDirSuffix()
{
	thread_local std::mt19937_64 rng(std::random_device {}());
	std::uniform_int_distribution<uint64_t> dist;
	std::ostringstream oss;
	oss << std::hex << dist(rng);
	return oss.str();
}

class StaticFilesTest : public ::testing::Test
{
protected:
	fs::path tmpDir;

	void SetUp() override
	{
		// 使用唯一后缀避免并行测试进程冲突
		tmpDir = fs::temp_directory_path() / ("hical_static_test_" + uniqueDirSuffix());
		fs::create_directories(tmpDir);

		// 创建测试文件
		writeFile("index.html", "<html><body>Hello</body></html>");
		writeFile("style.css", "body { color: red; }");
		writeFile("app.js", "console.log('hi');");
		writeFile("data.json", "{\"key\": \"value\"}");
		writeFile("image.png", "\x89PNG\r\n");

		// 创建子目录
		fs::create_directories(tmpDir / "sub");
		writeFile("sub/page.html", "<p>subdir</p>");
		writeFile("sub/index.html", "<p>sub index</p>");
	}

	void TearDown() override
	{
		fs::remove_all(tmpDir);
	}

	void writeFile(const std::string& name, const std::string& content)
	{
		std::ofstream f(tmpDir / name, std::ios::binary);
		f << content;
	}

	HttpRequest makeRequest(const std::string& path)
	{
		HttpRequest req;
		req.setMethod(HttpMethod::hGet);
		req.setTarget(path);
		return req;
	}
};

// ============ MIME 类型测试 ============

TEST(MimeTypeTest, CommonTypes)
{
	EXPECT_EQ(detail::mimeType(".html"), "text/html; charset=utf-8");
	EXPECT_EQ(detail::mimeType(".css"), "text/css; charset=utf-8");
	EXPECT_EQ(detail::mimeType(".js"), "application/javascript; charset=utf-8");
	EXPECT_EQ(detail::mimeType(".json"), "application/json; charset=utf-8");
	EXPECT_EQ(detail::mimeType(".png"), "image/png");
	EXPECT_EQ(detail::mimeType(".jpg"), "image/jpeg");
	EXPECT_EQ(detail::mimeType(".svg"), "image/svg+xml");
	EXPECT_EQ(detail::mimeType(".unknown"), "application/octet-stream");
}

// ============ serveStatic 功能测试 ============

TEST_F(StaticFilesTest, ServeHtmlFile)
{
	auto handler = serveStatic(tmpDir.string(), "/static/");
	auto req = makeRequest("/static/index.html");
	auto res = handler(req);

	EXPECT_EQ(static_cast<int>(res.statusCode()), 200);
	EXPECT_NE(res.body().find("Hello"), std::string::npos);
	EXPECT_NE(res.header("Content-Type").find("text/html"), std::string::npos);
}

TEST_F(StaticFilesTest, ServeCssFile)
{
	auto handler = serveStatic(tmpDir.string(), "/static/");
	auto req = makeRequest("/static/style.css");
	auto res = handler(req);

	EXPECT_EQ(static_cast<int>(res.statusCode()), 200);
	EXPECT_NE(res.header("Content-Type").find("text/css"), std::string::npos);
	EXPECT_EQ(res.body(), "body { color: red; }");
}

TEST_F(StaticFilesTest, ServeJsFile)
{
	auto handler = serveStatic(tmpDir.string(), "/static/");
	auto req = makeRequest("/static/app.js");
	auto res = handler(req);

	EXPECT_EQ(static_cast<int>(res.statusCode()), 200);
	EXPECT_NE(res.header("Content-Type").find("javascript"), std::string::npos);
}

TEST_F(StaticFilesTest, ServeJsonFile)
{
	auto handler = serveStatic(tmpDir.string(), "/static/");
	auto req = makeRequest("/static/data.json");
	auto res = handler(req);

	EXPECT_EQ(static_cast<int>(res.statusCode()), 200);
	EXPECT_NE(res.header("Content-Type").find("application/json"), std::string::npos);
}

TEST_F(StaticFilesTest, FileNotFound)
{
	auto handler = serveStatic(tmpDir.string(), "/static/");
	auto req = makeRequest("/static/nonexistent.html");
	auto res = handler(req);

	EXPECT_EQ(static_cast<int>(res.statusCode()), 404);
}

TEST_F(StaticFilesTest, DirectoryServesIndexHtml)
{
	auto handler = serveStatic(tmpDir.string(), "/static/");
	// 访问 /static/sub/ 应返回 sub/index.html
	auto req = makeRequest("/static/sub/");
	req.setParam("path", "sub/");
	auto res = handler(req);

	EXPECT_EQ(static_cast<int>(res.statusCode()), 200);
	EXPECT_NE(res.body().find("sub index"), std::string::npos);
}

TEST_F(StaticFilesTest, SubdirectoryFile)
{
	auto handler = serveStatic(tmpDir.string(), "/static/");
	auto req = makeRequest("/static/sub/page.html");
	auto res = handler(req);

	EXPECT_EQ(static_cast<int>(res.statusCode()), 200);
	EXPECT_NE(res.body().find("subdir"), std::string::npos);
}

TEST_F(StaticFilesTest, ETagIsPresent)
{
	auto handler = serveStatic(tmpDir.string(), "/static/");
	auto req = makeRequest("/static/index.html");
	auto res = handler(req);

	EXPECT_FALSE(res.header("ETag").empty());
	// ETag 应带引号（RFC 7232）
	auto etag = res.header("ETag");
	EXPECT_EQ(etag.front(), '"');
	EXPECT_EQ(etag.back(), '"');
}

TEST_F(StaticFilesTest, NotModifiedWhenETagMatches)
{
	auto handler = serveStatic(tmpDir.string(), "/static/");

	// 首次请求获取 ETag
	auto req1 = makeRequest("/static/index.html");
	auto res1 = handler(req1);
	EXPECT_EQ(static_cast<int>(res1.statusCode()), 200);
	auto etag = res1.header("ETag");

	// 携带 If-None-Match 请求
	auto req2 = makeRequest("/static/index.html");
	req2.setHeader("If-None-Match", etag);
	auto res2 = handler(req2);
	EXPECT_EQ(static_cast<int>(res2.statusCode()), 304);
	EXPECT_TRUE(res2.body().empty());
}

TEST_F(StaticFilesTest, PathTraversalPrevention)
{
	auto handler = serveStatic(tmpDir.string(), "/static/");
	// 尝试路径穿越
	auto req = makeRequest("/static/../../etc/passwd");
	auto res = handler(req);

	// 应返回 403 或 404，不能是 200
	EXPECT_NE(static_cast<int>(res.statusCode()), 200);
}

TEST_F(StaticFilesTest, InvalidRootDirReturns404)
{
	auto handler = serveStatic("/nonexistent/path/xyz", "/static/");
	auto req = makeRequest("/static/any.html");
	auto res = handler(req);

	EXPECT_EQ(static_cast<int>(res.statusCode()), 404);
}

// ============ P0: 大文件限制测试 ============

TEST_F(StaticFilesTest, LargeFileReturns413)
{
	// 写一个 10 字节的文件，但把 maxFileSize 设为 5，触发 413
	writeFile("big.txt", "0123456789");
	auto handler = serveStatic(tmpDir.string(), "/static/", 5);
	auto req = makeRequest("/static/big.txt");
	auto res = handler(req);

	EXPECT_EQ(static_cast<int>(res.statusCode()), 413);
}

TEST_F(StaticFilesTest, FileWithinLimitServedNormally)
{
	writeFile("small.txt", "hi");
	auto handler = serveStatic(tmpDir.string(), "/static/", 1024);
	auto req = makeRequest("/static/small.txt");
	auto res = handler(req);

	EXPECT_EQ(static_cast<int>(res.statusCode()), 200);
	EXPECT_EQ(res.body(), "hi");
}

// ============ P3: 304 响应 prepare_payload 测试 ============

TEST_F(StaticFilesTest, NotModifiedResponseHasEmptyBody)
{
	auto handler = serveStatic(tmpDir.string(), "/static/");

	auto req1 = makeRequest("/static/index.html");
	auto res1 = handler(req1);
	auto etag = res1.header("ETag");

	auto req2 = makeRequest("/static/index.html");
	req2.setHeader("If-None-Match", etag);
	auto res2 = handler(req2);

	EXPECT_EQ(static_cast<int>(res2.statusCode()), 304);
	EXPECT_TRUE(res2.body().empty());
	// 304 应包含 ETag 头
	EXPECT_FALSE(res2.header("ETag").empty());
}
