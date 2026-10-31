/**
 * @file test_http2_codec.cpp
 * @brief HTTP/2 帧交换、流事件和错误隔离测试
 */

#include "core/http2/Http2Codec.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <stdexcept>
#include <vector>

using namespace hical;
using namespace hical::detail;

namespace
{
	using Codec = Http2Codec;
	constexpr std::array<Codec::Header, 4> kRequest {
		{{":method", "GET"}, {":scheme", "https"}, {":authority", "localhost"}, {":path", "/"}}};
	constexpr std::array<Codec::Header, 1> kResponse {{{":status", "200"}}};

	struct Recorder : Codec::Listener
	{
		std::vector<Codec::HeaderBlock> blocks;
		std::vector<NetworkError> errors;
		std::vector<Codec::Frame> sent;
		std::vector<int32_t> ended;
		std::vector<int32_t> closed;
		std::vector<std::pair<std::string, std::string>> headers;
		size_t settings {0};
		size_t acknowledgements {0};
		int32_t rejectedStream {0};

		Http2Code onHeader(int32_t streamId, Codec::Header header) override
		{
			if (streamId == rejectedStream)
			{
				return Http2Code::hEnhanceYourCalm;
			}
			headers.emplace_back(header.name, header.value);
			return Http2Code::hNoError;
		}

		Http2Code onEndHeaders(const Codec::HeaderBlock& block) override
		{
			blocks.push_back(block);
			return Http2Code::hNoError;
		}

		void onEndStream(int32_t streamId) override
		{
			ended.push_back(streamId);
		}

		void onStreamClose(int32_t streamId, Http2Code) override
		{
			closed.push_back(streamId);
		}

		void onFrameSent(const Codec::Frame& frame) override
		{
			sent.push_back(frame);
		}

		void onSettings(bool ack) override
		{
			ack ? ++acknowledgements : ++settings;
		}

		void onError(const NetworkError& error) override
		{
			errors.push_back(error);
		}
	};

	std::string drain(Codec& codec, size_t limit = Http2Options::kDefaultOutputBatchBytes)
	{
		std::string output;
		for (size_t count = 0; count < 4096; ++count)
		{
			auto batch = codec.memSend(limit);
			EXPECT_TRUE(batch.error.ok()) << batch.error.message;
			EXPECT_LE(batch.bytes.size(), limit);
			if (batch.error || batch.bytes.empty())
			{
				return output;
			}
			output += batch.bytes;
		}
		ADD_FAILURE() << "协议输出未在有界轮次内排空";
		return output;
	}

	void feed(Codec& codec, std::string_view bytes, size_t fragment = 0)
	{
		while (!bytes.empty())
		{
			const auto chunk = bytes.substr(0, fragment == 0 ? bytes.size() : fragment);
			const auto result = codec.memRecv(chunk);
			ASSERT_TRUE(result.error.ok()) << result.error.message;
			ASSERT_GT(result.consumed, 0U);
			ASSERT_LE(result.consumed, chunk.size());
			bytes.remove_prefix(result.consumed);
		}
	}

	void exchange(Codec& client, Codec& server, size_t fragment = 0)
	{
		for (size_t count = 0; count < 64; ++count)
		{
			auto fromClient = drain(client);
			feed(server, fromClient, fragment);
			auto fromServer = drain(server);
			feed(client, fromServer, fragment);
			if (fromClient.empty() && fromServer.empty())
			{
				return;
			}
		}
		ADD_FAILURE() << "协议交换未在有界轮次内收敛";
	}

	std::string frame(Codec::FrameType type, uint8_t flags, int32_t streamId, std::string_view payload)
	{
		constexpr size_t kFrameHeaderSize = 9;
		std::string bytes(kFrameHeaderSize, '\0');
		bytes[0] = static_cast<char>(payload.size() >> 16);
		bytes[1] = static_cast<char>(payload.size() >> 8);
		bytes[2] = static_cast<char>(payload.size());
		bytes[3] = static_cast<char>(type);
		bytes[4] = static_cast<char>(flags);
		for (size_t index = 0; index < sizeof(streamId); ++index)
		{
			bytes[5 + index] = static_cast<char>(static_cast<uint32_t>(streamId) >> (24 - index * 8));
		}
		bytes += payload;
		return bytes;
	}

	bool hasError(const Recorder& recorder, ErrorCode category, Http2Code code, int32_t streamId)
	{
		return std::ranges::any_of(recorder.errors,
								   [&](const auto& error)
								   {
									   return error.code == category && error.http2
											  && error.http2->code == static_cast<uint32_t>(code)
											  && error.http2->streamId == streamId;
								   });
	}
} // namespace

TEST(Http2CodecTest, PrefaceAndSettingsSurviveByteFragmentsAndMergedFrames)
{
	Recorder clientEvents;
	Recorder serverEvents;
	Http2Options options;
	options.initialStreamWindow = 32768;
	Codec client(Codec::Role::hClient, {}, &clientEvents);
	Codec server(Codec::Role::hServer, options, &serverEvents);
	exchange(client, server, 1);
	EXPECT_EQ(clientEvents.settings, 1U);
	EXPECT_EQ(clientEvents.acknowledgements, 1U);
	EXPECT_EQ(serverEvents.settings, 1U);
	EXPECT_EQ(serverEvents.acknowledgements, 1U);
	EXPECT_EQ(client.peerSetting(Http2Setting::hInitialWindowSize), 32768U);
	EXPECT_EQ(server.peerSetting(Http2Setting::hEnablePush), 0U);
	EXPECT_EQ(server.peerMaxConcurrentStreams(), 0U);
	EXPECT_EQ(client.peerMaxConcurrentStreams(), options.maxConcurrentStreams);

	auto first = client.submitRequest(kRequest);
	auto second = client.submitRequest(kRequest);
	ASSERT_TRUE(first.error.ok());
	ASSERT_TRUE(second.error.ok());
	auto merged = drain(client);
	feed(server, merged);
	ASSERT_EQ(serverEvents.blocks.size(), 2U);
	EXPECT_EQ(serverEvents.ended, (std::vector<int32_t> {first.streamId, second.streamId}));
	EXPECT_TRUE(serverEvents.errors.empty());
	EXPECT_TRUE(std::ranges::any_of(clientEvents.sent,
									[&](const auto& sent)
									{
										return sent.type == Codec::FrameType::hHeaders
											   && sent.streamId == first.streamId
											   && (sent.flags & Codec::kEndStream) != 0;
									}));
}

TEST(Http2CodecTest, PriorityOnStreamZeroQueuesProtocolErrorBeforeItsPayloadArrives)
{
	for (const size_t fragment : {size_t {1}, size_t {9}})
	{
		Recorder events;
		Codec client(Codec::Role::hClient);
		Codec server(Codec::Role::hServer, {}, &events);
		exchange(client, server);
		// 只发九字节帧头，错误应立即返回，不等待已废弃的五字节依赖载荷。
		auto header = frame(Codec::FrameType::hPriority, 0, 0, std::string(5, '\0'));
		header.resize(9);
		feed(server, header, fragment);
		const auto output = drain(server);
		EXPECT_FALSE(output.empty());
		EXPECT_TRUE(hasError(events, ErrorCode::hHttp2ConnectionError, Http2Code::hProtocolError, 0));
		EXPECT_FALSE(server.wantRead());
	}
}

TEST(Http2CodecTest, DeprecatedPriorityDependencyDoesNotOpenOrRejectAStream)
{
	Recorder events;
	Codec client(Codec::Role::hClient);
	Codec server(Codec::Role::hServer, {}, &events);
	exchange(client, server);
	// RFC 9113 保留帧结构，已移除自依赖树的语义校验；PRIORITY 也不能占用流 ID。
	feed(server, frame(Codec::FrameType::hPriority, 0, 5, std::string("\0\0\0\5\x0f", 5)), 1);
	const auto request = client.submitRequest(kRequest);
	ASSERT_FALSE(request.error);
	EXPECT_EQ(request.streamId, 1);
	exchange(client, server);
	ASSERT_EQ(events.blocks.size(), 1U);
	EXPECT_EQ(events.blocks.front().streamId, request.streamId);
	EXPECT_TRUE(events.errors.empty());
}

TEST(Http2CodecTest, OwnedOutputRemainsValidAcrossTinyBatchesAndFurtherCalls)
{
	Codec client(Codec::Role::hClient);
	auto preface = client.memSend(3);
	ASSERT_TRUE(preface.error.ok());
	ASSERT_EQ(preface.bytes, "PRI");
	EXPECT_EQ(client.bufferedOutputBytes(), 21U);
	EXPECT_TRUE(client.memSend(0).bytes.empty());
	std::string bytes = preface.bytes + drain(client, 3);
	EXPECT_EQ(preface.bytes, "PRI");
	EXPECT_TRUE(bytes.starts_with("PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n"));
	Recorder events;
	Codec server(Codec::Role::hServer, {}, &events);
	feed(server, bytes, 2);
	EXPECT_EQ(events.settings, 1U);
}

TEST(Http2CodecTest, OutputIsRejectedBeforeCopyWhenTheConnectionBudgetCannotHoldIt)
{
	Codec client(Codec::Role::hClient);
	auto output = client.memSend(3, 2);
	EXPECT_TRUE(output.bytes.empty());
	EXPECT_EQ(output.error.code, ErrorCode::hResourceLimitExceeded);
	EXPECT_EQ(client.bufferedOutputBytes(), 0U);
	EXPECT_FALSE(client.wantWrite());
}

TEST(Http2CodecTest, ResponseInformationalHeadersAndTrailersKeepTheirCategories)
{
	Recorder events;
	Codec client(Codec::Role::hClient, {}, &events);
	Codec server(Codec::Role::hServer);
	exchange(client, server);
	auto request = client.submitRequest(kRequest);
	ASSERT_TRUE(request.error.ok());
	exchange(client, server);
	constexpr std::array<Codec::Header, 1> kInformational {{{":status", "103"}}};
	constexpr std::array<Codec::Header, 1> kTrailer {{{"x-checksum", "final"}}};
	ASSERT_TRUE(server.submitResponse(request.streamId, kInformational, false).ok());
	exchange(client, server);
	ASSERT_TRUE(server.submitResponse(request.streamId, kResponse, false).ok());
	exchange(client, server);
	ASSERT_TRUE(server.submitTrailers(request.streamId, kTrailer).ok());
	exchange(client, server);
	ASSERT_EQ(events.blocks.size(), 3U);
	EXPECT_EQ(events.blocks[0].kind, Codec::HeaderKind::hResponse);
	EXPECT_EQ(events.blocks[0].statusCode, 103);
	EXPECT_EQ(events.blocks[1].kind, Codec::HeaderKind::hResponse);
	EXPECT_EQ(events.blocks[1].statusCode, 200);
	EXPECT_EQ(events.blocks[2].kind, Codec::HeaderKind::hTrailers);
	EXPECT_TRUE(events.blocks[2].endStream);
	EXPECT_EQ(events.ended, std::vector<int32_t> {request.streamId});
}

TEST(Http2CodecTest, HeaderRejectionResetsOnlyThatStream)
{
	Recorder events;
	Codec client(Codec::Role::hClient);
	Codec server(Codec::Role::hServer, {}, &events);
	exchange(client, server);
	auto rejected = client.submitRequest(kRequest);
	auto accepted = client.submitRequest(kRequest);
	ASSERT_TRUE(rejected.error.ok());
	ASSERT_TRUE(accepted.error.ok());
	events.rejectedStream = rejected.streamId;
	exchange(client, server);
	ASSERT_EQ(events.blocks.size(), 1U);
	EXPECT_EQ(events.blocks.front().streamId, accepted.streamId);
	EXPECT_TRUE(hasError(events, ErrorCode::hHttp2StreamError, Http2Code::hEnhanceYourCalm, rejected.streamId));
	EXPECT_TRUE(server.submitResponse(accepted.streamId, kResponse).ok());
	exchange(client, server);
	EXPECT_TRUE(server.wantRead());
}

TEST(Http2CodecTest, ForbiddenHttpHeaderProducesStreamErrorAndKeepsConnectionUsable)
{
	Recorder events;
	Codec client(Codec::Role::hClient);
	Codec server(Codec::Role::hServer, {}, &events);
	exchange(client, server);
	auto headers = std::vector<Codec::Header>(kRequest.begin(), kRequest.end());
	headers.push_back({"connection", "keep-alive"});
	auto invalid = client.submitRequest(headers);
	ASSERT_TRUE(invalid.error.ok());
	exchange(client, server);
	EXPECT_TRUE(hasError(events, ErrorCode::hHttp2StreamError, Http2Code::hProtocolError, invalid.streamId));
	auto valid = client.submitRequest(kRequest);
	ASSERT_TRUE(valid.error.ok());
	exchange(client, server);
	EXPECT_EQ(events.blocks.back().streamId, valid.streamId);
}

TEST(Http2CodecTest, MalformedHpackAndFrameSizeAreConnectionErrors)
{
	for (bool hpack : {false, true})
	{
		Recorder events;
		Codec client(Codec::Role::hClient);
		Codec server(Codec::Role::hServer, {}, &events);
		exchange(client, server);
		auto bytes = hpack ? frame(Codec::FrameType::hHeaders, 0x05, 1, std::string(1, '\x80'))
						   : frame(Codec::FrameType::hPing, 0, 0, "x");
		auto result = server.memRecv(bytes);
		EXPECT_TRUE(result.error.ok() || result.error.code == ErrorCode::hHttp2ConnectionError);
		drain(server);
		EXPECT_TRUE(hasError(events,
							 ErrorCode::hHttp2ConnectionError,
							 hpack ? Http2Code::hCompressionError : Http2Code::hFrameSizeError,
							 0));
	}
}

TEST(Http2CodecTest, ClientRejectsPushEvenWhenThePeerSendsItAnyway)
{
	Recorder events;
	Codec client(Codec::Role::hClient, {}, &events);
	Codec server(Codec::Role::hServer);
	exchange(client, server);
	ASSERT_TRUE(client.submitRequest(kRequest).error.ok());
	exchange(client, server);
	const std::string promise {"\0\0\0\2\x82\x87\x84", 7};
	auto result = client.memRecv(frame(Codec::FrameType::hPushPromise, 0x04, 1, promise));
	EXPECT_TRUE(result.error.ok());
	drain(client);
	EXPECT_TRUE(hasError(events, ErrorCode::hHttp2ConnectionError, Http2Code::hProtocolError, 0));
}

TEST(Http2CodecTest, IncomingHeaderLimitsRejectBeforeAccumulatingExtraFields)
{
	Recorder events;
	Http2Options limits;
	limits.maxHeaderFields = kRequest.size();
	Codec client(Codec::Role::hClient);
	Codec server(Codec::Role::hServer, limits, &events);
	exchange(client, server);
	auto headers = std::vector<Codec::Header>(kRequest.begin(), kRequest.end());
	headers.push_back({"x-extra", "rejected"});
	auto request = client.submitRequest(headers);
	ASSERT_TRUE(request.error.ok());
	exchange(client, server);
	EXPECT_EQ(events.headers.size(), kRequest.size());
	EXPECT_TRUE(events.blocks.empty());
	EXPECT_TRUE(hasError(events, ErrorCode::hHttp2StreamError, Http2Code::hEnhanceYourCalm, request.streamId));
}

TEST(Http2CodecTest, CallbackExceptionsAndPumpReentryStayInsideCppBoundary)
{
	struct Throwing : Codec::Listener
	{
		Http2Code onBeginHeaders(int32_t) override
		{
			throw std::runtime_error("listener failure");
		}
	} throwing;

	Codec client(Codec::Role::hClient);
	Codec server(Codec::Role::hServer, {}, &throwing);
	exchange(client, server);
	ASSERT_TRUE(client.submitRequest(kRequest).error.ok());
	Codec::ReceiveResult result;
	EXPECT_NO_THROW(result = server.memRecv(drain(client)));
	EXPECT_EQ(result.error.code, ErrorCode::hHttp2ConnectionError);

	struct Reentrant : Codec::Listener
	{
		Codec* codec {nullptr};
		ErrorCode receiveError {ErrorCode::hNoError};
		ErrorCode sendError {ErrorCode::hNoError};

		void onSettings(bool) override
		{
			receiveError = codec->memRecv({}).error.code;
			sendError = codec->memSend().error.code;
		}
	} reentrant;

	Codec anotherClient(Codec::Role::hClient);
	Codec anotherServer(Codec::Role::hServer, {}, &reentrant);
	reentrant.codec = &anotherServer;
	exchange(anotherClient, anotherServer);
	EXPECT_EQ(reentrant.receiveError, ErrorCode::hOperationInProgress);
	EXPECT_EQ(reentrant.sendError, ErrorCode::hOperationInProgress);
}

TEST(Http2CodecTest, PeerZeroStreamCapacityQueuesUntilUpdated)
{
	Recorder events;
	Codec client(Codec::Role::hClient);
	Codec server(Codec::Role::hServer, {}, &events);
	exchange(client, server);
	constexpr std::array<Codec::Setting, 1> kPaused {{{Http2Setting::hMaxConcurrentStreams, 0}}};
	ASSERT_TRUE(server.submitSettings(kPaused).ok());
	exchange(client, server);
	EXPECT_EQ(client.peerMaxConcurrentStreams(), 0U);
	auto request = client.submitRequest(kRequest);
	ASSERT_TRUE(request.error.ok());
	exchange(client, server);
	EXPECT_TRUE(events.blocks.empty());
	constexpr std::array<Codec::Setting, 1> kResumed {{{Http2Setting::hMaxConcurrentStreams, 1}}};
	ASSERT_TRUE(server.submitSettings(kResumed).ok());
	exchange(client, server);
	ASSERT_EQ(events.blocks.size(), 1U);
	EXPECT_EQ(events.blocks.front().streamId, request.streamId);
}

TEST(Http2CodecTest, SubmissionCopiesHeadersAndEnforcesTheLocalHeaderBudget)
{
	Recorder events;
	Codec client(Codec::Role::hClient);
	Codec server(Codec::Role::hServer, {}, &events);
	exchange(client, server);
	{
		std::string value = "original";
		auto headers = std::vector<Codec::Header>(kRequest.begin(), kRequest.end());
		headers.push_back({"x-owned", value});
		ASSERT_TRUE(client.submitRequest(headers).error.ok());
		value.assign("changed!");
	}
	exchange(client, server);
	EXPECT_NE(std::ranges::find(events.headers, std::pair<std::string, std::string> {"x-owned", "original"}),
			  events.headers.end());
	std::string oversized(Http2Options::kDefaultMaxHeaderBytes, 'x');
	std::array<Codec::Header, 1> tooLarge {{{"x-large", oversized}}};
	EXPECT_EQ(client.submitRequest(tooLarge).error.code, ErrorCode::hResourceLimitExceeded);
}

TEST(Http2CodecTest, InvalidRolesStreamIdsAndPushSettingsFailBeforeSubmission)
{
	Codec client(Codec::Role::hClient);
	Codec server(Codec::Role::hServer);
	EXPECT_EQ(server.submitRequest(kRequest).error.code, ErrorCode::hInvalidConfiguration);
	EXPECT_EQ(client.submitResponse(1, kResponse).code, ErrorCode::hInvalidConfiguration);
	EXPECT_EQ(server.submitResponse(0, kResponse).code, ErrorCode::hInvalidConfiguration);
	EXPECT_EQ(client.submitTrailers(-1, kResponse).code, ErrorCode::hInvalidConfiguration);
	constexpr std::array<Codec::Setting, 1> kEnablePush {{{Http2Setting::hEnablePush, 1}}};
	EXPECT_EQ(client.submitSettings(kEnablePush).code, ErrorCode::hInvalidConfiguration);
	EXPECT_EQ(server.submitSettings(kEnablePush).code, ErrorCode::hInvalidConfiguration);
	EXPECT_THROW(Codec(static_cast<Codec::Role>(255)), std::invalid_argument);
}

TEST(Http2CodecTest, HeaderListBudgetChargesThirtyTwoBytesForEveryOriginalField)
{
	Http2Options options;
	options.maxHeaderBytes = 0;
	for (const auto& field : kRequest)
	{
		options.maxHeaderBytes += Http2Options::kHeaderFieldOverheadBytes + field.name.size() + field.value.size();
	}
	Codec limited(Codec::Role::hClient, options);
	ASSERT_TRUE(limited.submitRequest(kRequest).error.ok());
	auto headers = std::vector<Codec::Header>(kRequest.begin(), kRequest.end());
	headers.push_back({"x", ""});
	EXPECT_EQ(limited.submitRequest(headers).error.code, ErrorCode::hResourceLimitExceeded);

	Recorder events;
	Codec client(Codec::Role::hClient);
	Codec server(Codec::Role::hServer, options, &events);
	exchange(client, server);
	ASSERT_TRUE(client.submitRequest(kRequest).error.ok());
	exchange(client, server);
	ASSERT_EQ(events.blocks.size(), 1U);
	auto overflow = client.submitRequest(headers);
	ASSERT_TRUE(overflow.error.ok());
	exchange(client, server);
	EXPECT_EQ(events.blocks.size(), 1U);
	EXPECT_TRUE(hasError(events, ErrorCode::hHttp2StreamError, Http2Code::hEnhanceYourCalm, overflow.streamId));
}

TEST(Http2CodecTest, InvalidRegularHeadersRejectOnlyTheirStreamInsteadOfBeingIgnored)
{
	const std::array<Codec::Header, 2> invalidFields {{{"bad name", "x"}, {"x-bad", "a\x01"}}};
	for (const auto& invalid : invalidFields)
	{
		SCOPED_TRACE(invalid.name);
		Http2Options options;
		options.maxHeaderFields = kRequest.size();
		Recorder events;
		Codec client(Codec::Role::hClient);
		Codec server(Codec::Role::hServer, options, &events);
		exchange(client, server);
		auto headers = std::vector<Codec::Header>(kRequest.begin(), kRequest.end());
		headers.push_back(invalid);
		const auto rejected = client.submitRequest(headers);
		const auto accepted = client.submitRequest(kRequest);
		ASSERT_TRUE(rejected.error.ok());
		ASSERT_TRUE(accepted.error.ok());
		exchange(client, server);
		ASSERT_EQ(events.blocks.size(), 1U);
		EXPECT_EQ(events.blocks.front().streamId, accepted.streamId);
		EXPECT_TRUE(hasError(events, ErrorCode::hHttp2StreamError, Http2Code::hProtocolError, rejected.streamId));
		EXPECT_TRUE(server.wantRead());
	}
}
