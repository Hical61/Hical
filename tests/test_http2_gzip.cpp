/**
 * @file test_http2_gzip.cpp
 * @brief 验证 HTTP/2 DATA 中的 gzip 响应可解压
 */

#include "core/GzipCompression.h"
#include "core/http2/Http2Response.h"
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_future.hpp>
#include <zlib.h>
#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <future>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace hical;
using namespace hical::detail;

namespace
{
	using Codec = Http2Codec;

	std::string decompressGzip(std::string_view bytes)
	{
		z_stream stream {};
		if (inflateInit2(&stream, MAX_WBITS + 16) != Z_OK)
		{
			throw std::runtime_error("gzip 测试解压初始化失败");
		}

		struct Cleanup
		{
			z_stream& stream;

			~Cleanup()
			{
				inflateEnd(&stream);
			}
		} cleanup {stream};

		stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(bytes.data()));
		stream.avail_in = static_cast<uInt>(bytes.size());
		std::string output;
		std::array<char, 4096> buffer;
		for (;;)
		{
			stream.next_out = reinterpret_cast<Bytef*>(buffer.data());
			stream.avail_out = static_cast<uInt>(buffer.size());
			const auto result = inflate(&stream, Z_NO_FLUSH);
			const auto produced = buffer.size() - stream.avail_out;
			output.append(buffer.data(), produced);
			if (result == Z_STREAM_END)
			{
				if (stream.avail_in != 0)
				{
					throw std::runtime_error("gzip 流结束后仍有多余数据");
				}
				return output;
			}
			if (result != Z_OK || (produced == 0 && stream.avail_in == 0))
			{
				throw std::runtime_error("gzip DATA 非法或未完整结束");
			}
		}
	}

	struct Recorder : Codec::Listener
	{
		Codec* codec {nullptr};
		uint16_t status {0};
		std::vector<std::pair<std::string, std::string>> headers;
		std::string body;
		size_t ended {0};

		Http2Code onHeader(int32_t, Codec::Header field) override
		{
			headers.emplace_back(field.name, field.value);
			return Http2Code::hNoError;
		}

		Http2Code onEndHeaders(const Codec::HeaderBlock& block) override
		{
			if (block.kind == Codec::HeaderKind::hResponse)
			{
				status = block.statusCode;
			}
			return Http2Code::hNoError;
		}

		Http2Code onData(int32_t id, std::string_view bytes) override
		{
			body.append(bytes);
			EXPECT_FALSE(codec->consume(id, bytes.size()));
			return Http2Code::hNoError;
		}

		void onEndStream(int32_t) override
		{
			++ended;
		}
	};

	struct Pair
	{
		Recorder clientEvents;
		Codec client {Codec::Role::hClient, {}, &clientEvents};
		Codec server {Codec::Role::hServer};
		Http2ServiceBudget service {64};
		Http2Budget clientBudget {Http2Budget::Role::hClient, {}};
		Http2Budget serverBudget {Http2Budget::Role::hServer, {}, &service};
		Http2Output clientOutput {client, clientBudget};
		Http2Output serverOutput {server, serverBudget};

		Pair()
		{
			clientEvents.codec = &client;
		}

		bool pump(Http2Output& output, Codec& peer)
		{
			auto batch = output.nextBatch();
			EXPECT_FALSE(batch.error) << batch.error.message;
			if (batch.error || batch.bytes.empty())
			{
				return false;
			}
			auto received = peer.memRecv(batch.bytes);
			EXPECT_FALSE(received.error) << received.error.message;
			EXPECT_EQ(received.consumed, batch.bytes.size());
			EXPECT_FALSE(output.acknowledge(batch.bytes.size()).error);
			return true;
		}

		void exchange()
		{
			for (size_t count = 0; count < 256; ++count)
			{
				bool progress = pump(clientOutput, server);
				progress = pump(serverOutput, client) || progress;
				if (!progress)
				{
					return;
				}
			}
			ADD_FAILURE() << "gzip 响应未在有限轮次内收敛";
		}

		int32_t open()
		{
			exchange();
			constexpr std::array<Codec::Header, 4> request {
				{{":method", "GET"}, {":scheme", "https"}, {":authority", "localhost"}, {":path", "/gzip"}}};
			auto submitted = client.submitRequest(request);
			EXPECT_FALSE(submitted.error);
			exchange();
			return submitted.streamId;
		}
	};

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

	void expectHttp2Response(HttpResponse response, std::string_view original, bool compressed)
	{
		Pair pair;
		auto id = pair.open();
		auto submitted = Http2Response::submit(pair.serverOutput, id, HttpMethod::hGet, std::move(response));
		ASSERT_FALSE(submitted.error) << submitted.error.message;
		pair.exchange();
		EXPECT_EQ(pair.clientEvents.status, 200);
		EXPECT_EQ(pair.clientEvents.ended, 1U);
		const auto& headers = pair.clientEvents.headers;
		EXPECT_EQ(std::ranges::count(
					  headers,
					  std::make_pair(std::string("content-length"), std::to_string(pair.clientEvents.body.size()))),
				  1);
		EXPECT_TRUE(std::ranges::none_of(headers,
										 [](const auto& field)
										 {
											 return field.first == "transfer-encoding";
										 }));
		if (compressed)
		{
			EXPECT_EQ(decompressGzip(pair.clientEvents.body), original);
			EXPECT_NE(std::ranges::find(headers, std::make_pair(std::string("content-encoding"), std::string("gzip"))),
					  headers.end());
		}
		else
		{
			EXPECT_EQ(pair.clientEvents.body, original);
		}
	}
} // namespace

TEST(Http2GzipTest, SyncThresholdBoundariesPreserveRawDataAndFinalLength)
{
	GzipCompressionOptions options;
	options.minSize = 32;
	options.streamingThreshold = 128;
	auto middleware = makeGzipCompressionMiddleware(options);
	for (size_t size : {31, 32, 127, 128, 129})
	{
		HttpRequest request;
		request.native().protocol = HttpProtocol::hHttp2;
		request.setHeader("Accept-Encoding", "gzip");
		const std::string original(size, 's');
		auto response = HttpResponse::ok(original);
		middleware(request, response);
		expectHttp2Response(std::move(response), original, size >= options.minSize);
	}
}

TEST(Http2GzipTest, AsyncThresholdsAndWorkerCountsProduceDecodableDataOnOwner)
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
			request.native().protocol = HttpProtocol::hHttp2;
			request.setHeader("Accept-Encoding", "gzip");
			const std::string original(size, static_cast<char>('a' + workers));
			auto response = runAsync(middleware, request, original);
			expectHttp2Response(std::move(response), original, size >= options.minSize);
		}
	}
}

TEST(Http2GzipTest, IdentityNegotiationPreservesPayloadAndVaryMetadata)
{
	HttpRequest request;
	request.native().protocol = HttpProtocol::hHttp2;
	request.setHeader("Accept-Encoding", "gzip;q=0");
	GzipCompressionOptions options;
	options.minSize = 0;
	options.streamingThreshold = 32;
	auto middleware = makeGzipCompressionMiddleware(options);
	const std::string original(128, 'i');
	auto response = HttpResponse::ok(original);
	middleware(request, response);
	EXPECT_TRUE(response.header("Content-Encoding").empty());
	EXPECT_EQ(response.header("Vary"), "Accept-Encoding");
	expectHttp2Response(std::move(response), original, false);
}

TEST(Http2GzipTest, VaryKeepsExistingOriginAndDoesNotAddSecondGzipLayer)
{
	HttpRequest request;
	request.native().protocol = HttpProtocol::hHttp2;
	request.setHeader("Accept-Encoding", "gzip");
	GzipCompressionOptions options;
	options.minSize = 0;
	options.streamingThreshold = 64;
	auto middleware = makeGzipCompressionMiddleware(options);
	const std::string original(512, 'v');
	auto response = HttpResponse::ok(original);
	response.setHeader("Vary", "Origin");
	middleware(request, response);
	const auto once = response.body();
	middleware(request, response);
	EXPECT_EQ(response.body(), once);
	EXPECT_EQ(response.header("Vary"), "Origin, Accept-Encoding");
	expectHttp2Response(std::move(response), original, true);
}

TEST(Http2GzipTest, LargeResponseUsesRawGzipDataAndConsumesEntireCompressedInput)
{
	Pair pair;
	auto id = pair.open();
	HttpRequest request;
	request.native().protocol = HttpProtocol::hHttp2;
	request.setHeader("Accept-Encoding", "gzip");
	GzipCompressionOptions options;
	options.minSize = 0;
	options.streamingThreshold = 100;
	auto middleware = makeGzipCompressionMiddleware(options);
	const std::string original(8000, 'z');
	auto response = HttpResponse::ok(original);
	middleware(request, response);
	EXPECT_EQ(response.header("Content-Encoding"), "gzip");
	EXPECT_TRUE(response.header("Transfer-Encoding").empty());
	auto submitted = Http2Response::submit(pair.serverOutput, id, HttpMethod::hGet, std::move(response));
	ASSERT_FALSE(submitted.error) << submitted.error.message;
	pair.exchange();
	EXPECT_EQ(pair.clientEvents.status, 200);
	EXPECT_EQ(pair.clientEvents.ended, 1U);
	ASSERT_GE(pair.clientEvents.body.size(), 2U);
	EXPECT_EQ(static_cast<unsigned char>(pair.clientEvents.body[0]), 0x1f);
	EXPECT_EQ(static_cast<unsigned char>(pair.clientEvents.body[1]), 0x8b);
	EXPECT_EQ(decompressGzip(pair.clientEvents.body), original);
	EXPECT_TRUE(std::ranges::none_of(pair.clientEvents.headers,
									 [](const auto& field)
									 {
										 return field.first == "transfer-encoding";
									 }));
}
