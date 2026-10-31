/**
 * @file test_http2_response.cpp
 * @brief HTTP/2 响应、JSON、无正文语义和发送错误测试
 */

#include "core/http2/Http2Response.h"
#include "core/MetaJson.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <map>
#include <string>
#include <vector>

using namespace hical;
using namespace hical::detail;

namespace
{
	using Codec = Http2Codec;

	struct ResponseDto
	{
		std::string name;
		int count;

		HICAL_JSON(ResponseDto, name, count)
	};

	struct Message
	{
		uint16_t status {0};
		std::vector<std::pair<std::string, std::string>> fields;
		std::string body;
		size_t ended {0};
		size_t dataFrames {0};
		size_t trailerBlocks {0};

		std::vector<std::string> values(std::string_view name) const
		{
			std::vector<std::string> result;
			for (const auto& [field, value] : fields)
			{
				if (field == name)
				{
					result.push_back(value);
				}
			}
			return result;
		}
	};

	struct Recorder : Codec::Listener
	{
		Codec* codec {nullptr};
		std::map<int32_t, Message> messages;
		std::vector<NetworkError> errors;

		Http2Code onHeader(int32_t id, Codec::Header field) override
		{
			messages[id].fields.emplace_back(field.name, field.value);
			return Http2Code::hNoError;
		}

		Http2Code onEndHeaders(const Codec::HeaderBlock& block) override
		{
			if (block.kind == Codec::HeaderKind::hResponse)
			{
				messages[block.streamId].status = block.statusCode;
			}
			if (block.kind == Codec::HeaderKind::hTrailers)
			{
				++messages[block.streamId].trailerBlocks;
			}
			return Http2Code::hNoError;
		}

		Http2Code onData(int32_t id, std::string_view bytes) override
		{
			messages[id].body.append(bytes);
			EXPECT_FALSE(codec->consume(id, bytes.size()));
			return Http2Code::hNoError;
		}

		void onEndStream(int32_t id) override
		{
			++messages[id].ended;
		}

		void onFrameReceived(const Codec::Frame& frame) override
		{
			if (frame.type == Codec::FrameType::hData)
			{
				++messages[frame.streamId].dataFrames;
			}
		}

		void onError(const NetworkError& error) override
		{
			errors.push_back(error);
		}
	};

	struct Pair
	{
		Http2Options options;
		Recorder clientEvents;
		Recorder serverEvents;
		Http2ServiceBudget service {4096};
		Codec client;
		Codec server;
		Http2Budget clientBudget;
		Http2Budget serverBudget;
		Http2Output clientOutput;
		Http2Output serverOutput;
		std::vector<int32_t> written;

		explicit Pair(Http2Options settings = {})
			: options(settings)
			, client(Codec::Role::hClient, options, &clientEvents)
			, server(Codec::Role::hServer, options, &serverEvents)
			, clientBudget(Http2Budget::Role::hClient, options)
			, serverBudget(Http2Budget::Role::hServer, options, &service)
			, clientOutput(client, clientBudget, options)
			, serverOutput(server, serverBudget, options)
		{
			clientEvents.codec = &client;
			serverEvents.codec = &server;
		}

		bool pump(Http2Output& output, Codec& peer, bool serverSide)
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
			auto acknowledged = output.acknowledge(batch.bytes.size());
			EXPECT_FALSE(acknowledged.error);
			if (serverSide)
			{
				written.insert(written.end(), acknowledged.endedStreams.begin(), acknowledged.endedStreams.end());
			}
			return true;
		}

		void exchange()
		{
			for (size_t count = 0; count < 256; ++count)
			{
				bool progress = pump(clientOutput, server, false);
				progress = pump(serverOutput, client, true) || progress;
				if (!progress)
				{
					return;
				}
			}
			ADD_FAILURE() << "响应输出未在有限轮次内收敛";
		}

		int32_t open(HttpMethod method = HttpMethod::hGet)
		{
			exchange();
			std::array<Codec::Header, 4> fields {{{":method", httpMethodToString(method)},
												  {":scheme", "https"},
												  {":authority", "localhost"},
												  {":path", "/"}}};
			auto result = client.submitRequest(fields);
			EXPECT_FALSE(result.error);
			exchange();
			return result.streamId;
		}

		Message& send(HttpResponse response, HttpMethod method = HttpMethod::hGet)
		{
			auto id = open(method);
			auto result = Http2Response::submit(serverOutput, id, method, std::move(response), options);
			EXPECT_FALSE(result.error) << result.error.message;
			exchange();
			auto& message = clientEvents.messages[id];
			EXPECT_EQ(message.ended, 1U);
			EXPECT_EQ(std::ranges::count(written, id), 1);
			EXPECT_TRUE(clientEvents.errors.empty());
			return message;
		}
	};

	void expectLength(const Message& message, size_t length)
	{
		EXPECT_EQ(message.values("content-length"), (std::vector<std::string> {std::to_string(length)}));
	}
} // namespace

TEST(Http2ResponseTest, OrdinaryBinaryBodyAndTemporaryHeadersAreOwnedUntilPhysicalEnd)
{
	Pair pair;
	auto id = pair.open();
	constexpr char kBody[] = "payload\0\r\nHTTP/1.1 tail";
	const std::string body(kBody, sizeof(kBody) - 1);
	{
		auto response = HttpResponse::ok(body);
		response.setHeader("X-Temporary", std::string(80, 'v'));
		auto result = Http2Response::submit(pair.serverOutput, id, HttpMethod::hGet, std::move(response), pair.options);
		ASSERT_FALSE(result.error);
		ASSERT_TRUE(result.producer);
		EXPECT_EQ(result.producer->queuedBytes(), body.size());
		EXPECT_TRUE(response.native().body.empty());
	}
	pair.exchange();
	const auto& message = pair.clientEvents.messages[id];
	EXPECT_EQ(message.status, 200);
	EXPECT_EQ(message.body, body);
	EXPECT_EQ(message.values("x-temporary"), (std::vector<std::string> {std::string(80, 'v')}));
	expectLength(message, body.size());
	EXPECT_EQ(message.ended, 1U);
	EXPECT_EQ(std::ranges::count(pair.written, id), 1);
	EXPECT_FALSE(pair.serverOutput.hasPendingOutput());
	EXPECT_EQ(pair.serverOutput.queuedBytes(), 0U);
}

TEST(Http2ResponseTest, BoostJsonUsesOriginalJsonBytesWithoutHttp1Framing)
{
	Pair pair;
	const boost::json::value value = {{"name", "json"}, {"count", 3}};
	auto& message = pair.send(HttpResponse::json(value));
	EXPECT_EQ(message.status, 200);
	EXPECT_EQ(boost::json::parse(message.body), value);
	EXPECT_EQ(message.values("content-type"), (std::vector<std::string> {"application/json"}));
	expectLength(message, message.body.size());
}

TEST(Http2ResponseTest, CompileTimeJsonPreservesDtoAndContentType)
{
	Pair pair;
	auto& message = pair.send(HttpResponse::jsonFrom(ResponseDto {"compiled", 7}));
	EXPECT_EQ(message.body, R"({"name":"compiled","count":7})");
	EXPECT_EQ(message.values("content-type"), (std::vector<std::string> {"application/json"}));
	expectLength(message, message.body.size());
}

TEST(Http2ResponseTest, JsonWriterFinalBytesReplaceAnAlreadyPreparedContentLength)
{
	Pair pair;
	auto response = HttpResponse::ok(std::string(100, 'x'));
	ASSERT_EQ(response.header("Content-Length"), "100");
	{
		auto writer = response.jsonWriter();
		writer.beginObject().key("ready").value(true).endObject();
	}
	ASSERT_EQ(response.header("Content-Length"), "100");
	auto& message = pair.send(std::move(response));
	EXPECT_EQ(message.body, R"({"ready":true})");
	expectLength(message, message.body.size());
}

TEST(Http2ResponseTest, JsonWriterWithoutPreparedPayloadStillHasAccurateLength)
{
	Pair pair;
	HttpResponse response;
	{
		auto writer = response.jsonWriter();
		writer.beginArray().value(1).value("two").endArray();
	}
	auto& message = pair.send(std::move(response));
	EXPECT_EQ(message.body, R"([1,"two"])");
	expectLength(message, message.body.size());
}

TEST(Http2ResponseTest, RedirectAndErrorFactoriesKeepStatusAndEndEveryResponse)
{
	Pair pair;
	auto& redirect = pair.send(HttpResponse::redirect("/next", HttpStatusCode::hTemporaryRedirect));
	EXPECT_EQ(redirect.status, 307);
	EXPECT_EQ(redirect.values("location"), (std::vector<std::string> {"/next"}));
	EXPECT_TRUE(redirect.body.empty());
	expectLength(redirect, 0);
	auto& missing = pair.send(HttpResponse::notFound());
	EXPECT_EQ(missing.status, 404);
	EXPECT_EQ(missing.body, "Not Found");
	auto& bad = pair.send(HttpResponse::badRequest("invalid input"));
	EXPECT_EQ(bad.status, 400);
	EXPECT_EQ(bad.body, "invalid input");
}

TEST(Http2ResponseTest, MultipleSetCookiesRemainSeparateAndNamesBecomeLowercase)
{
	Pair pair;
	HttpResponse response;
	response.setCookie("one", "1");
	response.setCookie("two", "2");
	auto expectedViews = response.native().headers.findAll("Set-Cookie");
	std::vector<std::string> expected(expectedViews.begin(), expectedViews.end());
	auto& message = pair.send(std::move(response));
	EXPECT_EQ(message.values("set-cookie"), expected);
	EXPECT_EQ(message.values("Set-Cookie").size(), 0U);
}

TEST(Http2ResponseTest, HeadReportsFinalRepresentationLengthWithoutDataOrTrailers)
{
	Pair pair;
	auto response = HttpResponse::ok("head representation");
	response.setHeader("Content-Length", "999");
	auto& message = pair.send(std::move(response), HttpMethod::hHead);
	EXPECT_EQ(message.status, 200);
	expectLength(message, std::string_view("head representation").size());
	EXPECT_TRUE(message.body.empty());
	EXPECT_EQ(message.dataFrames, 0U);
	EXPECT_EQ(message.trailerBlocks, 0U);
}

TEST(Http2ResponseTest, EmptyHeadRepresentationUsesZeroLength)
{
	Pair pair;
	auto& message = pair.send(HttpResponse {}, HttpMethod::hHead);
	expectLength(message, 0);
	EXPECT_EQ(message.dataFrames, 0U);
}

TEST(Http2ResponseTest, NoContentDropsOldBodyAndAllContentLengthFields)
{
	Pair pair;
	auto response = HttpResponse::ok("must not be sent");
	response.native().headers.insert("CONTENT-LENGTH", "another old value");
	response.setStatus(HttpStatusCode::hNoContent);
	auto& message = pair.send(std::move(response));
	EXPECT_EQ(message.status, 204);
	EXPECT_TRUE(message.body.empty());
	EXPECT_TRUE(message.values("content-length").empty());
	EXPECT_EQ(message.dataFrames, 0U);
	EXPECT_EQ(message.trailerBlocks, 0U);
}

TEST(Http2ResponseTest, NotModifiedDoesNotInventRepresentationLengthFromResidualBody)
{
	Pair pair;
	HttpResponse response;
	response.setStatus(HttpStatusCode::hNotModified);
	response.native().body = "residual body is not representation metadata";
	auto& message = pair.send(std::move(response));
	EXPECT_EQ(message.status, 304);
	EXPECT_TRUE(message.body.empty());
	EXPECT_TRUE(message.values("content-length").empty());
	EXPECT_EQ(message.dataFrames, 0U);
	EXPECT_EQ(message.trailerBlocks, 0U);
}

TEST(Http2ResponseTest, NotModifiedPreservesExplicitValidRepresentationLength)
{
	Pair pair;
	HttpResponse response;
	response.setStatus(HttpStatusCode::hNotModified);
	response.setHeader("Content-Length", "12345");
	auto& message = pair.send(std::move(response));
	expectLength(message, 12345);
	EXPECT_TRUE(message.body.empty());
	EXPECT_EQ(message.dataFrames, 0U);
}

TEST(Http2ResponseTest, ConnectionSpecificFieldsAreRejectedBeforeSubmittingHeaders)
{
	for (auto field : {"Connection", "Keep-Alive", "Proxy-Connection", "Transfer-Encoding", "Upgrade", "TE"})
	{
		Pair pair;
		auto id = pair.open();
		auto response = HttpResponse::ok("body");
		response.setHeader(field, "trailers");
		auto result = Http2Response::submit(pair.serverOutput, id, HttpMethod::hGet, std::move(response), pair.options);
		EXPECT_TRUE(result.error) << field;
		EXPECT_FALSE(result.producer);
		EXPECT_EQ(pair.serverOutput.queuedBytes(), 0U);
		pair.exchange();
		EXPECT_FALSE(pair.clientEvents.messages.contains(id));
	}
}

TEST(Http2ResponseTest, InvalidNativeHeaderNamesValuesAndPseudoHeadersCannotReachWire)
{
	const std::array<std::pair<std::string, std::string>, 4> invalid {
		{{"bad name", "value"}, {"x-test", "value\r\nX-Other: injected"}, {":status", "201"}, {"", "empty name"}}};
	for (const auto& [name, value] : invalid)
	{
		Pair pair;
		auto id = pair.open();
		HttpResponse response;
		response.native().headers.insert(name, value);
		auto result = Http2Response::submit(pair.serverOutput, id, HttpMethod::hGet, std::move(response), pair.options);
		EXPECT_TRUE(result.error);
		EXPECT_FALSE(result.producer);
		pair.exchange();
		EXPECT_FALSE(pair.clientEvents.messages.contains(id));
	}
}

TEST(Http2ResponseTest, InvalidNumericStatusReturnsErrorWithoutOpeningProducer)
{
	for (uint16_t status : {0, 99, 600, 999, 65535})
	{
		Pair pair;
		auto id = pair.open();
		HttpResponse response;
		response.setStatus(static_cast<HttpStatusCode>(status));
		auto result = Http2Response::submit(pair.serverOutput, id, HttpMethod::hGet, std::move(response), pair.options);
		EXPECT_EQ(result.error.code, ErrorCode::hInvalidConfiguration);
		EXPECT_FALSE(result.producer);
		EXPECT_EQ(pair.serverOutput.queuedBytes(), 0U);
	}
}

TEST(Http2ResponseTest, StandaloneInformationalStatusBecomesFiniteInternalErrorResponse)
{
	for (uint16_t status : {100, 101, 103, 199})
	{
		Pair pair;
		auto response = HttpResponse::ok("informational cannot be final");
		response.setStatus(static_cast<HttpStatusCode>(status));
		auto& message = pair.send(std::move(response));
		EXPECT_EQ(message.status, 500);
		EXPECT_EQ(message.body, "Internal Server Error");
		expectLength(message, message.body.size());
	}
}

TEST(Http2ResponseTest, HeadFileUsesDeclaredLengthWithoutOpeningWhileCollectedChunksUseRawData)
{
	Pair pair;
	HttpResponse file;
	file.setFileBody("not-opened-by-response-helper", 0, 1024, "application/octet-stream");
	auto& fileMessage = pair.send(std::move(file), HttpMethod::hHead);
	EXPECT_EQ(fileMessage.status, 200);
	EXPECT_TRUE(fileMessage.body.empty());
	expectLength(fileMessage, 1024);
	auto chunks = HttpResponse::chunked();
	chunks.chunkedBody().write("must not serialize HTTP/1 chunk framing");
	chunks.chunkedBody().end();
	auto& chunkMessage = pair.send(std::move(chunks));
	EXPECT_EQ(chunkMessage.status, 200);
	EXPECT_EQ(chunkMessage.body, "must not serialize HTTP/1 chunk framing");
	expectLength(chunkMessage, chunkMessage.body.size());
}

TEST(Http2ResponseTest, HeaderBudgetIncludesPseudoHeaderAndFieldOverhead)
{
	constexpr size_t kExactHeaderBytes = 139;
	Pair pair;
	auto strict = pair.options;
	strict.maxHeaderFields = 3;
	for (auto limit : {kExactHeaderBytes - 1, kExactHeaderBytes})
	{
		strict.maxHeaderBytes = limit;
		auto id = pair.open();
		HttpResponse response;
		response.setHeader("X-Test", "twelve bytes");
		auto result = Http2Response::submit(pair.serverOutput, id, HttpMethod::hGet, std::move(response), strict);
		if (limit < kExactHeaderBytes)
		{
			EXPECT_EQ(result.error.code, ErrorCode::hResourceLimitExceeded);
			EXPECT_FALSE(result.producer);
			EXPECT_EQ(pair.serverOutput.queuedBytes(), 0U);
		}
		else
		{
			EXPECT_FALSE(result.error);
			pair.exchange();
			expectLength(pair.clientEvents.messages[id], 0);
			EXPECT_EQ(pair.clientEvents.messages[id].ended, 1U);
		}
	}
}

TEST(Http2ResponseTest, FieldCountBudgetDoesNotSilentlyDropHeaders)
{
	Pair pair;
	auto strict = pair.options;
	strict.maxHeaderFields = 2;
	auto id = pair.open();
	HttpResponse response;
	response.setHeader("x-extra", "value");
	auto result = Http2Response::submit(pair.serverOutput, id, HttpMethod::hGet, std::move(response), strict);
	EXPECT_EQ(result.error.code, ErrorCode::hResourceLimitExceeded);
	EXPECT_FALSE(result.producer);
	pair.exchange();
	EXPECT_FALSE(pair.clientEvents.messages.contains(id));
}

TEST(Http2ResponseTest, PayloadBudgetFailureCancelsProducerAndLeavesOtherStreamUsable)
{
	Http2Options options;
	options.maxHeaderBytes = 512;
	options.maxConnectionSendBytes = 4096;
	options.maxOutputBatchBytes = 256;
	options.maxFilePrefetchBytes = 256;
	Pair pair(options);
	auto id = pair.open();
	auto available = options.maxConnectionSendBytes - pair.serverBudget.usage().sendBytes;
	auto response = HttpResponse::ok(std::string(available + 1, 'x'));
	auto result = Http2Response::submit(pair.serverOutput, id, HttpMethod::hGet, std::move(response), options);
	EXPECT_EQ(result.error.code, ErrorCode::hResourceLimitExceeded);
	EXPECT_FALSE(result.producer);
	EXPECT_EQ(pair.serverOutput.queuedBytes(), 0U);
	// helper 只返回错误；复位流和唤醒等待者由上层会话处理，不直接驱动 codec。
	ASSERT_FALSE(pair.server.reset(id, Http2Code::hInternalError));
	pair.exchange();
	ASSERT_EQ(pair.clientEvents.errors.size(), 1U);
	const auto& resetError = pair.clientEvents.errors.front();
	EXPECT_EQ(resetError.code, ErrorCode::hHttp2StreamError);
	ASSERT_TRUE(resetError.http2);
	EXPECT_EQ(resetError.http2->streamId, id);
	EXPECT_EQ(resetError.http2->code, static_cast<uint32_t>(Http2Code::hInternalError));
	pair.clientEvents.errors.clear();
	auto& healthy = pair.send(HttpResponse::ok("still works"));
	EXPECT_EQ(healthy.status, 200);
	EXPECT_EQ(healthy.body, "still works");
	EXPECT_FALSE(pair.serverOutput.hasPendingOutput());
}
