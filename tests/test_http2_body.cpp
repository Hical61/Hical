/**
 * @file test_http2_body.cpp
 * @brief HTTP/2 分块正文、尾字段和流结束顺序测试
 */

#include "core/http2/Http2Response.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <map>
#include <string>
#include <utility>
#include <vector>

using namespace hical;
using namespace hical::detail;

namespace
{
	using Codec = Http2Codec;
	using Fields = std::vector<std::pair<std::string, std::string>>;

	struct Block
	{
		Codec::HeaderBlock metadata;
		Fields fields;
	};

	struct Message
	{
		std::string body;
		std::vector<Block> blocks;
		std::vector<Codec::Frame> frames;
		std::string order;
		size_t ended {0};
	};

	struct Recorder : Codec::Listener
	{
		Codec* codec {nullptr};
		Http2Output* output {nullptr};
		std::map<int32_t, Message> messages;
		std::map<int32_t, Fields> pending;
		std::vector<NetworkError> errors;

		Http2Code onHeader(int32_t id, Codec::Header field) override
		{
			pending[id].emplace_back(field.name, field.value);
			return Http2Code::hNoError;
		}

		Http2Code onEndHeaders(const Codec::HeaderBlock& block) override
		{
			auto& message = messages[block.streamId];
			message.blocks.push_back({block, std::move(pending[block.streamId])});
			message.order += block.kind == Codec::HeaderKind::hTrailers ? 'T' : 'H';
			return Http2Code::hNoError;
		}

		Http2Code onData(int32_t id, std::string_view bytes) override
		{
			auto& message = messages[id];
			message.body.append(bytes);
			message.order += 'D';
			EXPECT_FALSE(codec->consume(id, bytes.size()));
			return Http2Code::hNoError;
		}

		void onEndStream(int32_t id) override
		{
			++messages[id].ended;
			messages[id].order += 'E';
		}

		void onFrameReceived(const Codec::Frame& frame) override
		{
			messages[frame.streamId].frames.push_back(frame);
			if (output && frame.type == Codec::FrameType::hRstStream)
			{
				output->cancelStream(frame.streamId);
			}
		}

		void onStreamClose(int32_t id, Http2Code code) override
		{
			if (output)
			{
				output->onStreamClose(id, code);
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
			clientEvents.output = &clientOutput;
			serverEvents.codec = &server;
			serverEvents.output = &serverOutput;
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
			for (size_t count = 0; count < 1024; ++count)
			{
				bool progress = pump(clientOutput, server, false);
				progress = pump(serverOutput, client, true) || progress;
				if (!progress)
				{
					return;
				}
			}
			ADD_FAILURE() << "收集式 body 输出未在有限轮次内收敛";
		}

		int32_t open(HttpMethod method = HttpMethod::hGet)
		{
			exchange();
			std::array<Codec::Header, 4> request {{{":method", httpMethodToString(method)},
												   {":scheme", "https"},
												   {":authority", "localhost"},
												   {":path", "/chunks"}}};
			auto submitted = client.submitRequest(request);
			EXPECT_FALSE(submitted.error);
			exchange();
			return submitted.streamId;
		}
	};

	bool hasField(const Fields& fields, std::string_view name, std::string_view value)
	{
		return std::ranges::any_of(fields,
								   [&](const auto& field)
								   {
									   return field.first == name && field.second == value;
								   });
	}
} // namespace

TEST(Http2BodyTest, CollectedChunksBecomeRawDataWithFinalPayloadLength)
{
	Pair pair;
	auto id = pair.open();
	auto response = HttpResponse::chunked();
	response.chunkedBody().write("alpha\r\n");
	response.chunkedBody().write("beta");
	response.chunkedBody().end();
	response.native().body = "unused body must not become DATA";
	// preparePayload 会为 ChunkedBody 加上 Transfer-Encoding；H2 映射时应移除它。
	response.native().preparePayload();
	ASSERT_EQ(response.header("Transfer-Encoding"), "chunked");
	response.setHeader("Content-Length", "999");
	auto submitted = Http2Response::submit(pair.serverOutput, id, HttpMethod::hGet, std::move(response));
	ASSERT_FALSE(submitted.error) << submitted.error.message;
	EXPECT_FALSE(response.native().hasChunkedBody());
	pair.exchange();
	const auto& message = pair.clientEvents.messages[id];
	ASSERT_FALSE(message.blocks.empty());
	EXPECT_EQ(message.blocks.front().metadata.statusCode, 200);
	EXPECT_EQ(message.body, "alpha\r\nbeta");
	EXPECT_TRUE(hasField(message.blocks.front().fields, "content-length", "11"));
	EXPECT_FALSE(hasField(message.blocks.front().fields, "transfer-encoding", "chunked"));
	EXPECT_EQ(message.ended, 1U);
	EXPECT_EQ(std::ranges::count(pair.written, id), 1);
}

TEST(Http2BodyTest, NativeTrailersRemainSeparateFromInitialFieldsAndEndAfterData)
{
	Pair pair;
	auto id = pair.open();
	auto response = HttpResponse::ok("payload");
	response.setHeader("X-Checksum", "initial");
	response.setTrailer("X-Checksum", "final");
	auto submitted = Http2Response::submit(pair.serverOutput, id, HttpMethod::hGet, std::move(response));
	ASSERT_FALSE(submitted.error) << submitted.error.message;
	pair.exchange();
	const auto& message = pair.clientEvents.messages[id];
	ASSERT_EQ(message.blocks.size(), 2U);
	EXPECT_TRUE(hasField(message.blocks[0].fields, "x-checksum", "initial"));
	EXPECT_FALSE(hasField(message.blocks[0].fields, "x-checksum", "final"));
	EXPECT_EQ(message.blocks[1].metadata.kind, Codec::HeaderKind::hTrailers);
	EXPECT_TRUE(message.blocks[1].metadata.endStream);
	EXPECT_TRUE(hasField(message.blocks[1].fields, "x-checksum", "final"));
	EXPECT_EQ(message.body, "payload");
	EXPECT_EQ(message.order, "HDTE");
	EXPECT_EQ(message.ended, 1U);
	EXPECT_EQ(std::ranges::count(pair.written, id), 1);
}

TEST(Http2BodyTest, ChunkedAndNativeTrailerSourcesShareOneFinalHeaderBlock)
{
	Pair pair;
	auto id = pair.open();
	auto response = HttpResponse::chunked();
	response.chunkedBody().write("one");
	response.chunkedBody().write("two");
	response.chunkedBody().end({{"X-Chunk-Result", "done"}});
	response.setTrailer("X-Native-Result", "also done");
	auto submitted = Http2Response::submit(pair.serverOutput, id, HttpMethod::hGet, std::move(response));
	ASSERT_FALSE(submitted.error) << submitted.error.message;
	pair.exchange();
	const auto& message = pair.clientEvents.messages[id];
	ASSERT_EQ(message.blocks.size(), 2U);
	EXPECT_EQ(message.blocks.front().metadata.statusCode, 200);
	EXPECT_EQ(message.body, "onetwo");
	const auto& trailers = message.blocks.back();
	EXPECT_EQ(trailers.metadata.kind, Codec::HeaderKind::hTrailers);
	EXPECT_TRUE(hasField(trailers.fields, "x-chunk-result", "done"));
	EXPECT_TRUE(hasField(trailers.fields, "x-native-result", "also done"));
	EXPECT_EQ(message.ended, 1U);
	EXPECT_EQ(std::ranges::count(pair.written, id), 1);
}

TEST(Http2BodyTest, EmptyCollectedBodyEndsInInitialHeadersWithoutData)
{
	Pair pair;
	auto id = pair.open();
	auto response = HttpResponse::chunked();
	response.chunkedBody().write("");
	response.chunkedBody().end();
	auto submitted = Http2Response::submit(pair.serverOutput, id, HttpMethod::hGet, std::move(response));
	ASSERT_FALSE(submitted.error);
	EXPECT_FALSE(submitted.producer);
	EXPECT_FALSE(response.native().hasChunkedBody());
	pair.exchange();
	const auto& message = pair.clientEvents.messages[id];
	ASSERT_EQ(message.blocks.size(), 1U);
	EXPECT_TRUE(message.blocks[0].metadata.endStream);
	EXPECT_TRUE(hasField(message.blocks[0].fields, "content-length", "0"));
	EXPECT_EQ(message.order, "HE");
	EXPECT_EQ(message.ended, 1U);
	EXPECT_EQ(std::ranges::count(pair.written, id), 1);
}

TEST(Http2BodyTest, EmptyBodyWithTrailersEndsWithoutDataEvenAtZeroWindow)
{
	Http2Options options;
	options.initialStreamWindow = 0;
	Pair pair(options);
	auto id = pair.open();
	auto response = HttpResponse::chunked();
	response.chunkedBody().end({{"x-final", "empty"}});
	auto submitted = Http2Response::submit(pair.serverOutput, id, HttpMethod::hGet, std::move(response), options);
	ASSERT_FALSE(submitted.error);
	ASSERT_TRUE(submitted.producer);
	pair.exchange();
	const auto& message = pair.clientEvents.messages[id];
	ASSERT_EQ(message.blocks.size(), 2U);
	EXPECT_EQ(message.order, "HTE");
	EXPECT_TRUE(hasField(message.blocks[0].fields, "content-length", "0"));
	EXPECT_TRUE(hasField(message.blocks[1].fields, "x-final", "empty"));
	EXPECT_EQ(message.ended, 1U);
}

TEST(Http2BodyTest, HundredsOfSmallChunksAreBoundedAggregatedAndReleaseOriginalOwner)
{
	Pair pair;
	auto id = pair.open();
	auto response = HttpResponse::chunked();
	std::string expected;
	for (size_t index = 0; index < 300; ++index)
	{
		const auto piece = std::to_string(index) + ",";
		response.chunkedBody().write(piece);
		expected += piece;
	}
	response.chunkedBody().end({{"x-final", "many"}});
	auto submitted = Http2Response::submit(pair.serverOutput, id, HttpMethod::hGet, std::move(response));
	ASSERT_FALSE(submitted.error);
	ASSERT_TRUE(submitted.producer);
	EXPECT_FALSE(response.native().hasChunkedBody());
	EXPECT_GE(submitted.producer->queuedBytes(), expected.size());
	pair.exchange();
	const auto& message = pair.clientEvents.messages[id];
	ASSERT_EQ(message.blocks.size(), 2U);
	EXPECT_EQ(message.body, expected);
	EXPECT_TRUE(hasField(message.blocks[0].fields, "content-length", std::to_string(expected.size())));
	EXPECT_TRUE(hasField(message.blocks[1].fields, "x-final", "many"));
	EXPECT_EQ(message.ended, 1U);
	EXPECT_EQ(pair.serverOutput.queuedBytes(), 0U);
}

TEST(Http2BodyTest, UnfinishedCollectionReturnsFiniteErrorWithoutOpeningOutput)
{
	Pair pair;
	auto id = pair.open();
	auto response = HttpResponse::chunked();
	response.chunkedBody().write("not ended");
	auto submitted = Http2Response::submit(pair.serverOutput, id, HttpMethod::hGet, std::move(response));
	EXPECT_EQ(submitted.error.code, ErrorCode::hInvalidConfiguration);
	EXPECT_FALSE(submitted.producer);
	EXPECT_EQ(pair.serverOutput.queuedBytes(), 0U);
	EXPECT_FALSE(pair.serverOutput.hasPendingOutput());
	pair.exchange();
	EXPECT_FALSE(pair.clientEvents.messages.contains(id));
}

TEST(Http2BodyTest, BodylessResponsesValidateButNeverSendCollectedDataOrTrailers)
{
	for (auto status : {HttpStatusCode::hOk, HttpStatusCode::hNoContent, HttpStatusCode::hNotModified})
	{
		Pair pair;
		const auto method = status == HttpStatusCode::hOk ? HttpMethod::hHead : HttpMethod::hGet;
		auto id = pair.open(method);
		auto response = HttpResponse::chunked();
		response.chunkedBody().write("one");
		response.chunkedBody().write("two");
		response.chunkedBody().end({{"x-final", "omitted"}});
		response.setStatus(status);
		if (status == HttpStatusCode::hNotModified)
		{
			response.setHeader("Content-Length", "123");
		}
		auto submitted = Http2Response::submit(pair.serverOutput, id, method, std::move(response));
		ASSERT_FALSE(submitted.error);
		EXPECT_FALSE(submitted.producer);
		EXPECT_FALSE(response.native().hasChunkedBody());
		pair.exchange();
		const auto& message = pair.clientEvents.messages[id];
		ASSERT_EQ(message.blocks.size(), 1U);
		EXPECT_EQ(message.blocks[0].metadata.statusCode, static_cast<uint16_t>(status));
		EXPECT_TRUE(message.blocks[0].metadata.endStream);
		EXPECT_TRUE(message.body.empty());
		EXPECT_EQ(message.order, "HE");
		if (status == HttpStatusCode::hOk)
		{
			EXPECT_TRUE(hasField(message.blocks[0].fields, "content-length", "6"));
		}
		if (status == HttpStatusCode::hNotModified)
		{
			EXPECT_TRUE(hasField(message.blocks[0].fields, "content-length", "123"));
		}
		if (status == HttpStatusCode::hNoContent)
		{
			EXPECT_TRUE(std::ranges::none_of(message.blocks[0].fields,
											 [](const auto& field)
											 {
												 return field.first == "content-length";
											 }));
		}
	}
}

TEST(Http2BodyTest, ForbiddenFieldsFromEitherTrailerSourceAreRejectedBeforeHeaders)
{
	for (bool native : {false, true})
	{
		for (auto name : {":status", "connection", "content-length", "te", "authorization", "set-cookie"})
		{
			Pair pair;
			auto id = pair.open();
			auto response = HttpResponse::chunked();
			response.chunkedBody().write("body");
			response.chunkedBody().end(native ? std::vector<ChunkExtension> {}
											  : std::vector<ChunkExtension> {{name, "value"}});
			if (native)
			{
				response.native().trailers.insert(name, "value");
			}
			auto submitted = Http2Response::submit(pair.serverOutput, id, HttpMethod::hGet, std::move(response));
			EXPECT_TRUE(submitted.error) << name;
			EXPECT_FALSE(submitted.producer);
			EXPECT_EQ(pair.serverOutput.queuedBytes(), 0U);
			pair.exchange();
			EXPECT_FALSE(pair.clientEvents.messages.contains(id));
		}
	}
}

TEST(Http2BodyTest, BodylessResponsesDoNotBypassTrailerValidation)
{
	Pair pair;
	auto id = pair.open(HttpMethod::hHead);
	auto response = HttpResponse::chunked();
	response.chunkedBody().write("body");
	response.chunkedBody().end({{"content-length", "4"}});
	auto submitted = Http2Response::submit(pair.serverOutput, id, HttpMethod::hHead, std::move(response));
	EXPECT_TRUE(submitted.error);
	EXPECT_FALSE(submitted.producer);
	pair.exchange();
	EXPECT_FALSE(pair.clientEvents.messages.contains(id));
}

TEST(Http2BodyTest, HeaderAndTrailerByteBudgetsAreIndependentWhileMergedTrailersShareOneLimit)
{
	for (bool overflow : {false, true})
	{
		Http2Options options;
		options.maxHeaderBytes = 256;
		Pair pair(options);
		auto id = pair.open();
		auto response = HttpResponse::ok("body");
		response.setTrailer("x-proof", std::string(120, 'p'));
		if (overflow)
		{
			response.setTrailer("x-overflow", std::string(120, 'q'));
		}
		auto submitted = Http2Response::submit(pair.serverOutput, id, HttpMethod::hGet, std::move(response), options);
		if (overflow)
		{
			EXPECT_EQ(submitted.error.code, ErrorCode::hResourceLimitExceeded);
			EXPECT_FALSE(submitted.producer);
			EXPECT_EQ(pair.serverOutput.queuedBytes(), 0U);
		}
		else
		{
			ASSERT_FALSE(submitted.error);
			pair.exchange();
			const auto& message = pair.clientEvents.messages[id];
			ASSERT_EQ(message.blocks.size(), 2U);
			EXPECT_TRUE(hasField(message.blocks[1].fields, "x-proof", std::string(120, 'p')));
			EXPECT_EQ(message.body, "body");
		}
	}
}

TEST(Http2BodyTest, MergedTrailerFieldCountAndControlCharactersAreStrictlyChecked)
{
	for (bool invalidValue : {false, true})
	{
		Http2Options options;
		options.maxHeaderFields = 3;
		Pair pair;
		auto id = pair.open();
		auto response = HttpResponse::chunked();
		response.chunkedBody().write("body");
		response.setTrailer("x-one", "1");
		response.setTrailer("x-two", "2");
		response.chunkedBody().end(invalidValue ? std::vector<ChunkExtension> {{"x-three", "bad\r\nvalue"}}
												: std::vector<ChunkExtension> {{"x-three", "3"}, {"x-four", "4"}});
		auto submitted = Http2Response::submit(pair.serverOutput, id, HttpMethod::hGet, std::move(response), options);
		EXPECT_TRUE(submitted.error);
		if (!invalidValue)
		{
			EXPECT_EQ(submitted.error.code, ErrorCode::hResourceLimitExceeded);
		}
		EXPECT_FALSE(submitted.producer);
		EXPECT_EQ(pair.serverOutput.queuedBytes(), 0U);
	}
}

TEST(Http2BodyTest, DuplicateOrdinaryTrailerValuesRemainSeparate)
{
	Pair pair;
	auto id = pair.open();
	auto response = HttpResponse::chunked();
	response.chunkedBody().write("body");
	response.setTrailer("x-proof", "native");
	response.chunkedBody().end({{"x-proof", "chunked"}});
	auto submitted = Http2Response::submit(pair.serverOutput, id, HttpMethod::hGet, std::move(response));
	ASSERT_FALSE(submitted.error);
	pair.exchange();
	const auto& message = pair.clientEvents.messages[id];
	ASSERT_EQ(message.blocks.size(), 2U);
	EXPECT_TRUE(hasField(message.blocks[1].fields, "x-proof", "native"));
	EXPECT_TRUE(hasField(message.blocks[1].fields, "x-proof", "chunked"));
	EXPECT_EQ(std::ranges::count_if(message.blocks[1].fields,
									[](const auto& field)
									{
										return field.first == "x-proof";
									}),
			  2);
}

TEST(Http2BodyTest, OrdinaryTransferEncodingAndNonChunkedTypedEncodingRemainRejected)
{
	for (bool typed : {false, true})
	{
		Pair pair;
		auto id = pair.open();
		auto response = typed ? HttpResponse::chunked() : HttpResponse::ok("body");
		if (typed)
		{
			response.chunkedBody().write("body");
			response.chunkedBody().end();
		}
		response.setHeader("Transfer-Encoding", typed ? "gzip" : "chunked");
		auto submitted = Http2Response::submit(pair.serverOutput, id, HttpMethod::hGet, std::move(response));
		EXPECT_TRUE(submitted.error);
		EXPECT_FALSE(submitted.producer);
		pair.exchange();
		EXPECT_FALSE(pair.clientEvents.messages.contains(id));
	}
}

TEST(Http2BodyTest, AggregationBudgetFailureCancelsOutputAndKeepsSourceForCaller)
{
	Http2Options options;
	options.maxHeaderBytes = 512;
	options.maxConnectionSendBytes = 4096;
	options.maxOutputBatchBytes = 256;
	options.maxFilePrefetchBytes = 256;
	Pair pair(options);
	auto id = pair.open();
	auto response = HttpResponse::chunked();
	response.chunkedBody().write(std::string(2000, 'a'));
	response.chunkedBody().write(std::string(2000, 'b'));
	response.chunkedBody().end();
	auto submitted = Http2Response::submit(pair.serverOutput, id, HttpMethod::hGet, std::move(response), options);
	EXPECT_EQ(submitted.error.code, ErrorCode::hResourceLimitExceeded);
	EXPECT_FALSE(submitted.producer);
	EXPECT_TRUE(response.native().hasChunkedBody());
	EXPECT_EQ(pair.serverOutput.queuedBytes(), 0U);
	ASSERT_FALSE(pair.server.reset(id, Http2Code::hInternalError));
	pair.exchange();
	auto healthy = pair.open();
	auto good = HttpResponse::ok("survives");
	auto accepted = Http2Response::submit(pair.serverOutput, healthy, HttpMethod::hGet, std::move(good), options);
	ASSERT_FALSE(accepted.error);
	pair.exchange();
	EXPECT_EQ(pair.clientEvents.messages[healthy].body, "survives");
	EXPECT_EQ(pair.clientEvents.messages[healthy].ended, 1U);
}

TEST(Http2BodyTest, ResetAfterFirstDataSuppressesTrailersAndPreservesAnotherStream)
{
	for (auto resetCode : {Http2Code::hCancel, Http2Code::hNoError})
	{
		Http2Options options;
		options.maxOutputBatchBytes = 16;
		Pair pair(options);
		auto id = pair.open();
		auto response = HttpResponse::chunked();
		response.chunkedBody().write(std::string(96, 'x'));
		response.chunkedBody().end({{"x-final", "must not arrive"}});
		auto submitted = Http2Response::submit(pair.serverOutput, id, HttpMethod::hGet, std::move(response), options);
		ASSERT_FALSE(submitted.error);
		for (size_t count = 0; count < 128 && pair.clientEvents.messages[id].body.empty(); ++count)
		{
			ASSERT_TRUE(pair.pump(pair.serverOutput, pair.client, true));
		}
		ASSERT_FALSE(pair.clientEvents.messages[id].body.empty());
		ASSERT_LT(pair.clientEvents.messages[id].body.size(), 96U);
		ASSERT_FALSE(pair.client.reset(id, resetCode));
		pair.exchange();
		const auto& failed = pair.clientEvents.messages[id];
		ASSERT_EQ(failed.blocks.size(), 1U);
		EXPECT_EQ(failed.ended, 0U);
		EXPECT_EQ(std::ranges::count(pair.written, id), 0);
		EXPECT_EQ(submitted.producer->queuedBytes(), 0U);
		auto healthy = pair.open();
		auto good = HttpResponse::ok("healthy");
		auto accepted = Http2Response::submit(pair.serverOutput, healthy, HttpMethod::hGet, std::move(good), options);
		ASSERT_FALSE(accepted.error);
		pair.exchange();
		EXPECT_EQ(pair.clientEvents.messages[healthy].body, "healthy");
		EXPECT_EQ(pair.clientEvents.messages[healthy].ended, 1U);
	}
}
