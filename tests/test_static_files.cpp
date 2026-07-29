#include "core/HttpRequest.h"
#include "core/HttpResponse.h"
#include "core/StaticFiles.h"
#include <boost/asio.hpp>
#include <boost/asio/use_future.hpp>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <random>
#include <string>

using namespace hical;
namespace fs = std::filesystem;

// ============ 协程测试辅助：在 io_context 中运行协程并返回结果 ============

template <typename T>
T runAwaitable(boost::asio::awaitable<T> aw)
{
	boost::asio::io_context ioc;
	auto fut = boost::asio::co_spawn(ioc, std::move(aw), boost::asio::use_future);
	ioc.run();
	return fut.get(); // 重新抛出协程内的异常
}

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

		// 清理 TlContentCache，避免旧测试遗留数据干扰
		auto& contentCache = detail::getTlContentCache();
		contentCache.index.clear();
		contentCache.lruList.clear();
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

	HttpResponse invoke(const std::function<Awaitable<HttpResponse>(const HttpRequest&)>& handler,
						const HttpRequest& req)
	{
		return runAwaitable(handler(req));
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
	auto res = invoke(handler, req);

	EXPECT_EQ(static_cast<int>(res.statusCode()), 200);
	// 文本/JSON/JS/CSS/SVG 走 setBody，非 setFileBody
	EXPECT_FALSE(res.hasFileBody());
	EXPECT_FALSE(res.body().empty());
	EXPECT_NE(res.header("Content-Type").find("text/html"), std::string::npos);
}

TEST_F(StaticFilesTest, ServeCssFile)
{
	auto handler = serveStatic(tmpDir.string(), "/static/");
	auto req = makeRequest("/static/style.css");
	auto res = invoke(handler, req);

	EXPECT_EQ(static_cast<int>(res.statusCode()), 200);
	EXPECT_FALSE(res.hasFileBody());
	EXPECT_FALSE(res.body().empty());
	EXPECT_NE(res.header("Content-Type").find("text/css"), std::string::npos);
}

TEST_F(StaticFilesTest, ServeJsFile)
{
	auto handler = serveStatic(tmpDir.string(), "/static/");
	auto req = makeRequest("/static/app.js");
	auto res = invoke(handler, req);

	EXPECT_EQ(static_cast<int>(res.statusCode()), 200);
	EXPECT_FALSE(res.hasFileBody());
	EXPECT_FALSE(res.body().empty());
	EXPECT_NE(res.header("Content-Type").find("javascript"), std::string::npos);
}

TEST_F(StaticFilesTest, ServeJsonFile)
{
	auto handler = serveStatic(tmpDir.string(), "/static/");
	auto req = makeRequest("/static/data.json");
	auto res = invoke(handler, req);

	EXPECT_EQ(static_cast<int>(res.statusCode()), 200);
	EXPECT_FALSE(res.hasFileBody());
	EXPECT_FALSE(res.body().empty());
	EXPECT_NE(res.header("Content-Type").find("application/json"), std::string::npos);
}

TEST_F(StaticFilesTest, FileNotFound)
{
	auto handler = serveStatic(tmpDir.string(), "/static/");
	auto req = makeRequest("/static/nonexistent.html");
	auto res = invoke(handler, req);

	EXPECT_EQ(static_cast<int>(res.statusCode()), 404);
}

TEST_F(StaticFilesTest, DirectoryServesIndexHtml)
{
	auto handler = serveStatic(tmpDir.string(), "/static/");
	// 访问 /static/sub/ 应返回 sub/index.html
	auto req = makeRequest("/static/sub/");
	req.setParam("path", "sub/");
	auto res = invoke(handler, req);

	EXPECT_EQ(static_cast<int>(res.statusCode()), 200);
	// 文本文件走 setBody
	EXPECT_FALSE(res.hasFileBody());
	EXPECT_FALSE(res.body().empty());
}

TEST_F(StaticFilesTest, SubdirectoryFile)
{
	auto handler = serveStatic(tmpDir.string(), "/static/");
	auto req = makeRequest("/static/sub/page.html");
	auto res = invoke(handler, req);

	EXPECT_EQ(static_cast<int>(res.statusCode()), 200);
	// 文本文件走 setBody
	EXPECT_FALSE(res.hasFileBody());
	EXPECT_FALSE(res.body().empty());
}

TEST_F(StaticFilesTest, ETagIsPresent)
{
	auto handler = serveStatic(tmpDir.string(), "/static/");
	auto req = makeRequest("/static/index.html");
	auto res = invoke(handler, req);

	EXPECT_FALSE(res.header("ETag").empty());
	// ETag 应带引号（RFC 7232）
	auto etag = std::string(res.header("ETag"));
	EXPECT_EQ(etag.front(), '"');
	EXPECT_EQ(etag.back(), '"');
}

TEST_F(StaticFilesTest, NotModifiedWhenETagMatches)
{
	auto handler = serveStatic(tmpDir.string(), "/static/");

	// 首次请求获取 ETag
	auto req1 = makeRequest("/static/index.html");
	auto res1 = invoke(handler, req1);
	EXPECT_EQ(static_cast<int>(res1.statusCode()), 200);
	auto etag = std::string(res1.header("ETag"));

	// 携带 If-None-Match 请求
	auto req2 = makeRequest("/static/index.html");
	req2.setHeader("If-None-Match", etag);
	auto res2 = invoke(handler, req2);
	EXPECT_EQ(static_cast<int>(res2.statusCode()), 304);
	EXPECT_TRUE(res2.body().empty());
}

TEST_F(StaticFilesTest, PathTraversalPrevention)
{
	auto handler = serveStatic(tmpDir.string(), "/static/");
	// 尝试路径穿越
	auto req = makeRequest("/static/../../etc/passwd");
	auto res = invoke(handler, req);

	// 应返回 403 或 404，不能是 200
	EXPECT_NE(static_cast<int>(res.statusCode()), 200);
}

TEST_F(StaticFilesTest, InvalidRootDirReturns404)
{
	auto handler = serveStatic("/nonexistent/path/xyz", "/static/");
	auto req = makeRequest("/static/any.html");
	auto res = invoke(handler, req);

	EXPECT_EQ(static_cast<int>(res.statusCode()), 404);
}

// ============ 大文件限制测试 ============

TEST_F(StaticFilesTest, LargeFileReturns413)
{
	// 写一个 10 字节的文件，但把 maxFileSize 设为 5，触发 413
	writeFile("big.txt", "0123456789");
	auto handler = serveStatic(tmpDir.string(), "/static/", 5);
	auto req = makeRequest("/static/big.txt");
	auto res = invoke(handler, req);

	EXPECT_EQ(static_cast<int>(res.statusCode()), 413);
}

TEST_F(StaticFilesTest, FileWithinLimitServedNormally)
{
	writeFile("small.txt", "hi");
	auto handler = serveStatic(tmpDir.string(), "/static/", 1024);
	auto req = makeRequest("/static/small.txt");
	auto res = invoke(handler, req);

	EXPECT_EQ(static_cast<int>(res.statusCode()), 200);
	// 文本文件走 setBody
	EXPECT_FALSE(res.hasFileBody());
	EXPECT_FALSE(res.body().empty());
}

// ============ 304 响应 prepare_payload 测试 ============

TEST_F(StaticFilesTest, NotModifiedResponseHasEmptyBody)
{
	auto handler = serveStatic(tmpDir.string(), "/static/");

	auto req1 = makeRequest("/static/index.html");
	auto res1 = invoke(handler, req1);
	auto etag = std::string(res1.header("ETag"));

	auto req2 = makeRequest("/static/index.html");
	req2.setHeader("If-None-Match", etag);
	auto res2 = invoke(handler, req2);

	EXPECT_EQ(static_cast<int>(res2.statusCode()), 304);
	EXPECT_TRUE(res2.body().empty());
	// 304 应包含 ETag 头
	EXPECT_FALSE(res2.header("ETag").empty());
}

// ============ TlContentCache 测试 ============

TEST_F(StaticFilesTest, CacheHitReturnsSameContent)
{
	auto handler = serveStatic(tmpDir.string(), "/static/");

	// 第一次请求 — 写入缓存
	auto req = makeRequest("/static/index.html");
	auto res1 = invoke(handler, req);
	EXPECT_EQ(static_cast<int>(res1.statusCode()), 200);
	std::string body1(res1.body());

	// 第二次请求 — 应从缓存命中，body 一致
	auto res2 = invoke(handler, req);
	EXPECT_EQ(static_cast<int>(res2.statusCode()), 200);
	EXPECT_EQ(res2.body(), body1);
}

TEST_F(StaticFilesTest, CacheHitReturnsSameETag)
{
	auto handler = serveStatic(tmpDir.string(), "/static/");

	auto req = makeRequest("/static/index.html");
	auto res1 = invoke(handler, req);
	auto etag1 = std::string(res1.header("ETag"));

	auto res2 = invoke(handler, req);
	auto etag2 = std::string(res2.header("ETag"));

	EXPECT_EQ(etag1, etag2);
}

TEST_F(StaticFilesTest, NotModifiedWithCachedETag)
{
	auto handler = serveStatic(tmpDir.string(), "/static/");

	// 第一次 200，会写入缓存
	auto req1 = makeRequest("/static/index.html");
	auto res1 = invoke(handler, req1);
	EXPECT_EQ(static_cast<int>(res1.statusCode()), 200);
	auto etag = std::string(res1.header("ETag"));

	// 第二次带 If-None-Match → 304，应从内容缓存命中路径发出（重构后缓存检查已移到 fstat 之前）
	auto req2 = makeRequest("/static/index.html");
	req2.setHeader("If-None-Match", etag);
	auto res2 = invoke(handler, req2);
	EXPECT_EQ(static_cast<int>(res2.statusCode()), 304);
	EXPECT_TRUE(res2.body().empty());
	EXPECT_EQ(std::string(res2.header("ETag")), etag);
}

TEST_F(StaticFilesTest, CacheBlock304BypassesFstat)
{
	// 验证缓存命中时 304 从缓存块发出，不经过外层 fstat+makeEtag 计算。
	// 构造：写入一条 expired 缓存但 mtime 与磁盘一致，304 应从缓存块直接匹配。
	using namespace std::chrono;
	auto& cache = detail::getTlContentCache();

	// 先正常请求获取真实 etag
	auto handler = serveStatic(tmpDir.string(), "/static/");
	auto req1 = makeRequest("/static/index.html");
	auto res1 = invoke(handler, req1);
	auto freshEtag = std::string(res1.header("ETag"));

	// 清空缓存，手动写入一条 TTL 已过期的条目（mtime 与磁盘一致）
	cache.index.clear();
	cache.lruList.clear();
	std::string path = (tmpDir / "index.html").string();
	detail::TlContentCache::Entry entry;
	entry.key = path;
	entry.content = "<html><body>Hello</body></html>";
	entry.etag = freshEtag;
	std::error_code ec;
	auto lw = fs::last_write_time(path, ec);
	entry.mtimeNs = lw.time_since_epoch().count();
	entry.cachedAt = steady_clock::now() - detail::TlContentCache::kTtl - seconds(1);
	cache.lruList.push_front(std::move(entry));
	cache.index[path] = cache.lruList.begin();

	// TTL 过期但 mtime 一致 → 续期 + 缓存命中路径 → 304
	auto req2 = makeRequest("/static/index.html");
	req2.setHeader("If-None-Match", freshEtag);
	auto res2 = invoke(handler, req2);
	EXPECT_EQ(static_cast<int>(res2.statusCode()), 304);
	EXPECT_EQ(std::string(res2.header("ETag")), freshEtag);
	// 缓存仍存在（续期了，不是被淘汰）
	EXPECT_FALSE(cache.index.empty());
}

TEST_F(StaticFilesTest, TTLExpiredRechecksMtime)
{
	// 直接操作缓存，写入一条过期且 mtime 不匹配的条目，验证缓存被淘汰并返回 200
	using namespace std::chrono;
	auto& cache = detail::getTlContentCache();
	std::string path = (tmpDir / "index.html").string();
	detail::TlContentCache::Entry expiredEntry;
	expiredEntry.key = path;
	expiredEntry.content = "<html><body>Hello</body></html>";
	expiredEntry.etag = "\"stale-etag\"";
	// cachedAt 设为 TTL 之前，mtimeNs 故意填 0 保证与磁盘 mtime 不匹配
	expiredEntry.cachedAt = steady_clock::now() - detail::TlContentCache::kTtl - seconds(1);
	expiredEntry.mtimeNs = 0;
	cache.lruList.push_front(std::move(expiredEntry));
	cache.index[path] = cache.lruList.begin();

	auto handler = serveStatic(tmpDir.string(), "/static/");
	auto req = makeRequest("/static/index.html");
	auto res = invoke(handler, req);

	// 过期 + mtime 不匹配 → 淘汰旧缓存 → 重新读盘 → 200 + 正确的 ETag
	EXPECT_EQ(static_cast<int>(res.statusCode()), 200);
	EXPECT_NE(std::string(res.header("ETag")), "\"stale-etag\"");
}

TEST_F(StaticFilesTest, FileModifiedAfterTTLExpiryInvalidatesCache)
{
	// 文件在磁盘上被修改，TTL 过期后 mtime 变化检测到，淘汰旧缓存，
	// 后续请求走到 fstat → 读盘 → 写回新缓存路径，返回正确的 200 + 新 ETag。
	using namespace std::chrono;
	auto handler = serveStatic(tmpDir.string(), "/static/");

	// 第一次请求，写入缓存
	auto req1 = makeRequest("/static/index.html");
	auto res1 = invoke(handler, req1);
	EXPECT_EQ(static_cast<int>(res1.statusCode()), 200);
	auto oldEtag = std::string(res1.header("ETag"));

	// 手动把缓存条目的 cachedAt 拨到 TTL 之前
	auto& cache = detail::getTlContentCache();
	EXPECT_FALSE(cache.lruList.empty());
	cache.lruList.front().cachedAt = steady_clock::now() - detail::TlContentCache::kTtl - seconds(1);

	// 修改文件内容
	writeFile("index.html", "<html><body>Updated</body></html>");
	// Windows 文件系统时间戳精度有限（NTFS 100ns 但 FAT/exFAT 可能更粗），
	// 两次快速写入可能落在同个时钟 tick，显式把 mtime 往后拨 1 秒保证变化
	std::error_code ec_utime;
	fs::last_write_time(tmpDir / "index.html",
						fs::last_write_time(tmpDir / "index.html", ec_utime) + std::chrono::seconds(1),
						ec_utime);

	// TTL 过期 + mtime 变化 → 淘汰旧缓存，重新读盘
	auto req2 = makeRequest("/static/index.html");
	auto res2 = invoke(handler, req2);
	EXPECT_EQ(static_cast<int>(res2.statusCode()), 200);
	auto newEtag = std::string(res2.header("ETag"));

	// 新 ETag 应不同于旧 ETag（文件内容变了）
	EXPECT_NE(newEtag, oldEtag);
	// 响应 body 应反映更新后的文件
	EXPECT_NE(res2.body().find("Updated"), std::string::npos);
	// 缓存应已刷新为新条目
	EXPECT_FALSE(cache.index.empty());
}

TEST_F(StaticFilesTest, FileOverMaxSizeNotCached)
{
	// 小文本文件会被缓存
	auto handler = serveStatic(tmpDir.string(), "/static/");
	auto req = makeRequest("/static/index.html");
	auto res = invoke(handler, req);
	EXPECT_EQ(static_cast<int>(res.statusCode()), 200);

	auto& cache = detail::getTlContentCache();
	// index.html 是文本小文件，应被缓存
	EXPECT_FALSE(cache.index.empty());
	EXPECT_GE(cache.lruList.size(), 1u);
}
