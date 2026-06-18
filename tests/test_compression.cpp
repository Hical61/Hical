/**
 * @file test_compression.cpp
 * @brief gzip 响应压缩测试
 */

#include "core/GzipCompression.h"
#include "core/HttpRequest.h"
#include "core/HttpResponse.h"
#include <zlib.h>
#include <gtest/gtest.h>
#include <array>
#include <string>
#include <string_view>

using namespace hical;

namespace
{

	/**
	 * @brief 用 zlib inflate 解压 gzip 数据
	 */
	std::string gzipDecompress(std::string_view input)
	{
		if (input.empty())
		{
			return {};
		}

		z_stream strm = {};
		strm.zalloc = Z_NULL;
		strm.zfree = Z_NULL;
		strm.opaque = Z_NULL;

		// 15 + 16 = MAX_WBITS + GZIP 格式
		auto ret = inflateInit2(&strm, 15 + 16);
		if (ret != Z_OK)
		{
			throw std::runtime_error("inflateInit2 failed: " + std::to_string(ret));
		}

		strm.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(input.data()));
		strm.avail_in = static_cast<uInt>(input.size());

		std::string output;
		std::array<char, 16384> outBuf {};

		do
		{
			strm.next_out = reinterpret_cast<Bytef*>(outBuf.data());
			strm.avail_out = sizeof(outBuf);

			ret = inflate(&strm, Z_NO_FLUSH);
			if (ret != Z_OK && ret != Z_STREAM_END && ret != Z_BUF_ERROR)
			{
				inflateEnd(&strm);
				throw std::runtime_error("inflate failed: " + std::to_string(ret) + " " + strm.msg);
			}

			auto have = sizeof(outBuf) - strm.avail_out;
			output.append(outBuf.data(), have);
		}
		while (ret != Z_STREAM_END);

		inflateEnd(&strm);
		return output;
	}

} // namespace

// ============ GzipCompression 单元测试 ============

TEST(GzipCompressionTest, AcceptsGzip_ReturnsTrue)
{
	HttpRequest req;
	req.setHeader("Accept-Encoding", "gzip");

	// 把最小阈值设低，确保小 body 也会被压缩
	GzipCompressionOptions opts;
	opts.minSize = 0;
	auto middleware = makeGzipCompressionMiddleware(opts);
	HttpResponse res = HttpResponse::ok("Hello World!");

	middleware(req, res);

	EXPECT_EQ(res.header("Content-Encoding"), "gzip");
	EXPECT_FALSE(res.body().empty());
	EXPECT_NE(res.body(), "Hello World!");

	// 验证解压后内容一致
	auto decompressed = gzipDecompress(res.body());
	EXPECT_EQ(decompressed, "Hello World!");
}

TEST(GzipCompressionTest, NoAcceptGzip_SkipsCompression)
{
	HttpRequest req;
	req.setHeader("Accept-Encoding", "identity");

	auto middleware = makeGzipCompressionMiddleware({});
	HttpResponse res = HttpResponse::ok("Hello World!");

	middleware(req, res);

	// 不应压缩
	EXPECT_TRUE(res.header("Content-Encoding").empty());
	EXPECT_EQ(res.body(), "Hello World!");
}

TEST(GzipCompressionTest, AcceptsGzipWithMultipleEncodings)
{
	HttpRequest req;
	req.setHeader("Accept-Encoding", "deflate, gzip, br");

	GzipCompressionOptions opts;
	opts.minSize = 0;
	auto middleware = makeGzipCompressionMiddleware(opts);
	HttpResponse res = HttpResponse::ok("Hello World!");

	middleware(req, res);

	EXPECT_EQ(res.header("Content-Encoding"), "gzip");
	auto decompressed = gzipDecompress(res.body());
	EXPECT_EQ(decompressed, "Hello World!");
}

TEST(GzipCompressionTest, SmallBody_BelowThreshold_SkipsCompression)
{
	HttpRequest req;
	req.setHeader("Accept-Encoding", "gzip");

	GzipCompressionOptions opts;
	opts.minSize = 1024;
	auto middleware = makeGzipCompressionMiddleware(opts);

	HttpResponse res = HttpResponse::ok("small");

	middleware(req, res);

	// body 只有 5 字节 < 1024，不应压缩
	EXPECT_TRUE(res.header("Content-Encoding").empty());
	EXPECT_EQ(res.body(), "small");
}

TEST(GzipCompressionTest, EmptyBody_SkipsCompression)
{
	HttpRequest req;
	req.setHeader("Accept-Encoding", "gzip");

	auto middleware = makeGzipCompressionMiddleware({});
	HttpResponse res = HttpResponse::ok();

	middleware(req, res);

	EXPECT_TRUE(res.header("Content-Encoding").empty());
}

TEST(GzipCompressionTest, CompressedDataRoundTrip)
{
	HttpRequest req;
	req.setHeader("Accept-Encoding", "gzip");

	auto middleware = makeGzipCompressionMiddleware({});

	std::string original;
	original.reserve(5000);
	for (int i = 0; i < 100; ++i)
	{
		original += "The quick brown fox jumps over the lazy dog. ";
	}

	HttpResponse res = HttpResponse::ok(original);
	middleware(req, res);

	EXPECT_EQ(res.header("Content-Encoding"), "gzip");

	auto decompressed = gzipDecompress(res.body());
	EXPECT_EQ(decompressed, original);
}

TEST(GzipCompressionTest, LargeBody_StreamingCompression)
{
	HttpRequest req;
	req.setHeader("Accept-Encoding", "gzip");

	// minSize=0 确保任何大小都压缩，streamingThreshold=100 强行走流式
	GzipCompressionOptions opts;
	opts.minSize = 0;
	opts.streamingThreshold = 100;
	opts.compressionLevel = 1;
	auto middleware = makeGzipCompressionMiddleware(opts);

	std::string original(200, 'A');
	HttpResponse res = HttpResponse::ok(original);

	middleware(req, res);

	EXPECT_EQ(res.header("Content-Encoding"), "gzip");
	EXPECT_EQ(res.header("Transfer-Encoding"), "chunked");

	auto body = res.body();
	EXPECT_FALSE(body.empty());
	EXPECT_NE(body.find("0\r\n\r\n"), std::string::npos);
}

TEST(GzipCompressionTest, ChunkedBodyResponse_SkipsCompression)
{
	HttpRequest req;
	req.setHeader("Accept-Encoding", "gzip");

	auto middleware = makeGzipCompressionMiddleware({});

	// chunked 响应体（SSE/流式输出不走压缩）
	HttpResponse res = HttpResponse::chunked();
	res.native().body = "some body";

	middleware(req, res);

	// chunked 响应不应被压缩
	EXPECT_TRUE(res.header("Content-Encoding").empty());
}

TEST(GzipCompressionTest, MultipleAcceptEncodings_WithGzip)
{
	HttpRequest req;
	req.setHeader("Accept-Encoding", "br;q=0.8, gzip;q=1.0, deflate;q=0.5");

	GzipCompressionOptions opts;
	opts.minSize = 0;
	auto middleware = makeGzipCompressionMiddleware(opts);
	HttpResponse res = HttpResponse::ok("Hello World!");

	middleware(req, res);

	EXPECT_EQ(res.header("Content-Encoding"), "gzip");
	auto decompressed = gzipDecompress(res.body());
	EXPECT_EQ(decompressed, "Hello World!");
}

TEST(GzipCompressionTest, NoAcceptEncodingHeader_SkipsCompression)
{
	HttpRequest req;
	// 不设 Accept-Encoding 头

	auto middleware = makeGzipCompressionMiddleware({});
	HttpResponse res = HttpResponse::ok("Hello World!");

	middleware(req, res);

	EXPECT_TRUE(res.header("Content-Encoding").empty());
	EXPECT_EQ(res.body(), "Hello World!");
}

TEST(GzipCompressionTest, ExactlyAtThreshold_Compresses)
{
	HttpRequest req;
	req.setHeader("Accept-Encoding", "gzip");

	GzipCompressionOptions opts;
	opts.minSize = 0;
	auto middleware = makeGzipCompressionMiddleware(opts);

	// 刚好 1 字节 body
	HttpResponse res = HttpResponse::ok("X");
	middleware(req, res);

	EXPECT_EQ(res.header("Content-Encoding"), "gzip");
	EXPECT_EQ(gzipDecompress(res.body()), "X");
}
