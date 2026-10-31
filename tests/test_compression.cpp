/**
 * @file test_compression.cpp
 * @brief gzip 响应压缩测试
 */

#include "core/GzipCompression.h"
#include "core/HttpRequest.h"
#include "core/HttpResponse.h"
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_future.hpp>
#include <zlib.h>
#include <gtest/gtest.h>
#include <array>
#include <charconv>
#include <future>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

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
			if (ret != Z_OK && ret != Z_STREAM_END)
			{
				const std::string message = strm.msg ? strm.msg : "解压无进展或 gzip 不完整";
				inflateEnd(&strm);
				throw std::runtime_error("inflate 失败: " + std::to_string(ret) + " " + message);
			}

			auto have = sizeof(outBuf) - strm.avail_out;
			output.append(outBuf.data(), have);
		}
		while (ret != Z_STREAM_END);

		inflateEnd(&strm);
		if (strm.avail_in != 0)
		{
			throw std::runtime_error("gzip 流结束后仍有多余输入");
		}
		return output;
	}

	std::string decodeChunked(std::string_view wire)
	{
		std::string payload;
		for (;;)
		{
			const auto end = wire.find("\r\n");
			if (end == std::string_view::npos)
			{
				throw std::runtime_error("分块长度行不完整");
			}
			size_t length = 0;
			const auto parsed = std::from_chars(wire.data(), wire.data() + end, length, 16);
			if (parsed.ec != std::errc {} || parsed.ptr != wire.data() + end)
			{
				throw std::runtime_error("分块长度无效");
			}
			wire.remove_prefix(end + 2);
			if (length == 0)
			{
				if (wire != "\r\n")
				{
					throw std::runtime_error("分块结尾无效或存在多余输入");
				}
				return payload;
			}
			if (length > wire.size() || wire.size() - length < 2 || wire.substr(length, 2) != "\r\n")
			{
				throw std::runtime_error("分块数据不完整");
			}
			payload.append(wire.substr(0, length));
			wire.remove_prefix(length + 2);
		}
	}

	Awaitable<HttpResponse> responseFor(std::string body)
	{
		co_return HttpResponse::ok(body);
	}

	Awaitable<HttpResponse> invokeAsync(MiddlewareHandler middleware,
										HttpRequest& request,
										std::string body,
										std::thread::id& completion)
	{
		MiddlewareNext next = [body = std::move(body)](HttpRequest&) mutable
		{
			return responseFor(std::move(body));
		};
		auto response = co_await middleware(request, std::move(next));
		completion = std::this_thread::get_id();
		co_return response;
	}

	HttpResponse runAsync(MiddlewareHandler middleware, HttpRequest& request, std::string body)
	{
		boost::asio::io_context io;
		std::thread::id completion;
		auto result = boost::asio::co_spawn(io,
											invokeAsync(std::move(middleware), request, std::move(body), completion),
											boost::asio::use_future);
		io.run();
		auto response = result.get();
		EXPECT_EQ(completion, std::this_thread::get_id());
		return response;
	}

	void expectH1Compression(HttpResponse& response, std::string_view original, const GzipCompressionOptions& options)
	{
		response.native().preparePayload();
		response.native().preparePayload();
		if (original.size() < options.minSize)
		{
			EXPECT_EQ(response.body(), original);
			EXPECT_TRUE(response.header("Content-Encoding").empty());
			return;
		}
		EXPECT_EQ(response.header("Content-Encoding"), "gzip");
		EXPECT_EQ(response.header("Vary"), "Accept-Encoding");
		if (original.size() >= options.streamingThreshold)
		{
			EXPECT_EQ(response.header("Transfer-Encoding"), "chunked");
			EXPECT_TRUE(response.header("Content-Length").empty());
			EXPECT_EQ(gzipDecompress(decodeChunked(response.body())), original);
		}
		else
		{
			EXPECT_TRUE(response.header("Transfer-Encoding").empty());
			EXPECT_EQ(response.header("Content-Length"), std::to_string(response.body().size()));
			EXPECT_EQ(gzipDecompress(response.body()), original);
		}
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

	// minSize=0 确保小正文也压缩，streamingThreshold=100 让 H1 响应使用 chunked 编码。
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

TEST(GzipCompressionTest, LargeBodyFinalPreparationKeepsChunkedWithoutLength)
{
	HttpRequest request;
	request.setHeader("Accept-Encoding", "gzip");
	GzipCompressionOptions options;
	options.minSize = 0;
	options.streamingThreshold = 100;
	auto middleware = makeGzipCompressionMiddleware(options);
	auto response = HttpResponse::ok(std::string(200, 'A'));
	middleware(request, response);
	// HTTP/1 dispatch 在后置中间件完成后还会准备 payload，不能重添与 chunked 冲突的长度。
	response.native().preparePayload();
	response.native().preparePayload();
	EXPECT_EQ(response.header("Transfer-Encoding"), "chunked");
	EXPECT_TRUE(response.header("Content-Length").empty());
}

TEST(GzipCompressionTest, ZeroQualityGzipIsNotUsed)
{
	HttpRequest request;
	request.setHeader("Accept-Encoding", "br;q=0.8, gzip;q=0");
	GzipCompressionOptions options;
	options.minSize = 0;
	auto middleware = makeGzipCompressionMiddleware(options);
	auto response = HttpResponse::ok("gzip was explicitly excluded");
	middleware(request, response);
	EXPECT_TRUE(response.header("Content-Encoding").empty());
	EXPECT_EQ(response.body(), "gzip was explicitly excluded");
}

TEST(GzipCompressionTest, StatusChangeAfterLargeGzipKeepsChunkedFramingWithoutLength)
{
	HttpRequest request;
	request.setHeader("Accept-Encoding", "gzip");
	GzipCompressionOptions options;
	options.minSize = 0;
	options.streamingThreshold = 100;
	auto middleware = makeGzipCompressionMiddleware(options);
	const std::string original(200, 'A');
	auto response = HttpResponse::ok(original);
	middleware(request, response);
	// 后置中间件改状态会使 payload 缓存失效，再次准备正文时仍要保留已有的分块编码。
	response.setStatus(HttpStatusCode::hCreated);
	response.native().preparePayload();
	EXPECT_EQ(response.statusCode(), HttpStatusCode::hCreated);
	EXPECT_EQ(response.header("Transfer-Encoding"), "chunked");
	EXPECT_TRUE(response.header("Content-Length").empty());
	EXPECT_EQ(gzipDecompress(decodeChunked(response.body())), original);
}

TEST(GzipCompressionTest, ExplicitFramingRemovalRestoresRegularBodyAndJsonWriterPreparation)
{
	for (bool json : {false, true})
	{
		HttpRequest request;
		request.setHeader("Accept-Encoding", "gzip");
		GzipCompressionOptions options;
		options.minSize = 0;
		options.streamingThreshold = 100;
		auto middleware = makeGzipCompressionMiddleware(options);
		auto response = HttpResponse::ok(std::string(200, 'A'));
		middleware(request, response);
		// 现有 setter 接受预编码字节；切回普通正文时由调用方显式移除旧的编码元数据。
		response.native().headers.erase("Transfer-Encoding");
		response.native().headers.erase("Content-Encoding");
		if (json)
		{
			auto writer = response.jsonWriter();
			writer.beginObject().key("value").value(1).endObject();
		}
		else
		{
			response.setBody("replacement");
		}
		response.native().preparePayload();
		const std::string expected = json ? R"({"value":1})" : "replacement";
		EXPECT_EQ(response.body(), expected);
		EXPECT_TRUE(response.header("Transfer-Encoding").empty());
		EXPECT_TRUE(response.header("Content-Encoding").empty());
		EXPECT_EQ(response.header("Content-Length"), std::to_string(expected.size()));
	}
}

TEST(GzipCompressionTest, SyncH1ThresholdBoundariesRoundTripAfterFinalPreparation)
{
	GzipCompressionOptions options;
	options.minSize = 32;
	options.streamingThreshold = 128;
	auto middleware = makeGzipCompressionMiddleware(options);
	for (size_t size : {31, 32, 127, 128, 129})
	{
		HttpRequest request;
		request.setHeader("Accept-Encoding", "gzip");
		const std::string original(size, 'a');
		auto response = HttpResponse::ok(original);
		middleware(request, response);
		expectH1Compression(response, original, options);
	}
}

TEST(GzipCompressionTest, AsyncH1ThresholdsAndZeroWorkerOptionKeepOwnerCompletion)
{
	for (size_t workers : {0, 1, 2})
	{
		GzipCompressionOptions options;
		options.minSize = 32;
		options.streamingThreshold = 128;
		options.asyncThreads = workers;
		auto middleware = makeGzipCompressionMiddlewareAsync(options);
		for (size_t size : {31, 32, 127, 128, 129})
		{
			HttpRequest request;
			request.setHeader("Accept-Encoding", "gzip");
			const std::string original(size, static_cast<char>('a' + workers));
			auto response = runAsync(middleware, request, original);
			expectH1Compression(response, original, options);
		}
	}
}

TEST(GzipCompressionTest, VaryMergesWithoutDroppingOriginDuplicatingEncodingOrExpandingStar)
{
	const std::array<std::pair<std::string_view, std::string_view>, 3> cases {
		{{"Origin", "Origin, Accept-Encoding"}, {"Origin, ACCEPT-ENCODING", "Origin, ACCEPT-ENCODING"}, {"*", "*"}}};
	GzipCompressionOptions options;
	options.minSize = 0;
	auto middleware = makeGzipCompressionMiddleware(options);
	for (const auto& [before, after] : cases)
	{
		HttpRequest request;
		request.setHeader("Accept-Encoding", "gzip");
		auto response = HttpResponse::ok("vary representation");
		response.setHeader("Vary", before);
		response.native().headers.insert("Vary", "Accept-Language");
		middleware(request, response);
		EXPECT_EQ(response.header("Vary"), after);
		const auto values = response.native().headers.findAll("Vary");
		ASSERT_EQ(values.size(), 2U);
		EXPECT_EQ(values[1], "Accept-Language");
	}
}

TEST(GzipCompressionTest, IdentityResponseStillVariesWhenGzipWouldBeAvailable)
{
	HttpRequest request;
	request.setHeader("Accept-Encoding", "identity");
	GzipCompressionOptions options;
	options.minSize = 0;
	auto middleware = makeGzipCompressionMiddleware(options);
	auto response = HttpResponse::ok("identity representation");
	middleware(request, response);
	EXPECT_TRUE(response.header("Content-Encoding").empty());
	EXPECT_EQ(response.body(), "identity representation");
	EXPECT_EQ(response.header("Vary"), "Accept-Encoding");
}

TEST(GzipCompressionTest, ExistingContentEncodingIsNotCompressedAgain)
{
	HttpRequest request;
	request.setHeader("Accept-Encoding", "gzip");
	GzipCompressionOptions options;
	options.minSize = 0;
	auto middleware = makeGzipCompressionMiddleware(options);
	auto response = HttpResponse::ok("already gzip encoded");
	middleware(request, response);
	const auto once = response.body();
	middleware(request, response);
	EXPECT_EQ(response.body(), once);
	EXPECT_EQ(gzipDecompress(response.body()), "already gzip encoded");
	EXPECT_EQ(response.header("Vary"), "Accept-Encoding");
}

TEST(GzipCompressionTest, QualityValuesHandleZeroCaseAndPositiveFractions)
{
	GzipCompressionOptions options;
	options.minSize = 0;
	auto middleware = makeGzipCompressionMiddleware(options);
	const std::array<std::pair<std::string_view, bool>, 6> cases {{{"gzip;q=0.000", false},
																   {"GZIP; q=0", false},
																   {"gzip;q=0.001", true},
																   {"GZIP;Q=1.000", true},
																   {"gzip;q=1.5", false},
																   {"gzip;q=invalid", false}}};
	for (const auto& [encoding, compress] : cases)
	{
		HttpRequest request;
		request.setHeader("Accept-Encoding", std::string(encoding));
		auto response = HttpResponse::ok("quality negotiation");
		middleware(request, response);
		EXPECT_EQ(response.header("Content-Encoding") == "gzip", compress) << encoding;
	}
}

TEST(GzipCompressionTest, ConcurrentAsyncBodiesStayIndependentAndReturnToOwner)
{
	constexpr size_t kCount = 6;
	boost::asio::io_context io;
	GzipCompressionOptions options;
	options.minSize = 0;
	options.streamingThreshold = 128;
	options.asyncThreads = 2;
	auto middleware = makeGzipCompressionMiddlewareAsync(options);
	std::array<HttpRequest, kCount> requests;
	std::array<std::string, kCount> originals;
	std::array<std::thread::id, kCount> completions;
	std::vector<std::future<HttpResponse>> results;
	for (size_t index = 0; index < kCount; ++index)
	{
		requests[index].setHeader("Accept-Encoding", "gzip");
		originals[index] = std::string(127 + index * 1024, static_cast<char>('a' + index));
		results.push_back(
			boost::asio::co_spawn(io,
								  invokeAsync(middleware, requests[index], originals[index], completions[index]),
								  boost::asio::use_future));
	}
	io.run();
	for (size_t index = 0; index < kCount; ++index)
	{
		auto response = results[index].get();
		EXPECT_EQ(completions[index], std::this_thread::get_id());
		expectH1Compression(response, originals[index], options);
	}
}

TEST(GzipCompressionTest, LazyAsyncResultKeepsWorkerPoolAfterMiddlewareWrapperIsDestroyed)
{
	boost::asio::io_context io;
	HttpRequest request;
	request.native().protocol = HttpProtocol::hHttp2;
	request.setHeader("Accept-Encoding", "gzip");
	GzipCompressionOptions options;
	options.minSize = 0;
	options.streamingThreshold = 64;
	MiddlewareNext next = [](HttpRequest&)
	{
		return responseFor(std::string(512, 'z'));
	};
	auto middleware = makeGzipCompressionMiddlewareAsync(options);
	auto pending = middleware(request, std::move(next));
	middleware = {};
	auto result = boost::asio::co_spawn(io, std::move(pending), boost::asio::use_future);
	io.run();
	EXPECT_EQ(gzipDecompress(result.get().body()), std::string(512, 'z'));
}
