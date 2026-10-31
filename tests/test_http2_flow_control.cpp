/**
 * @file test_http2_flow_control.cpp
 * @brief HTTP/2 接收窗口、body 完成和多流预算测试
 */

#include "core/http2/Http2Receive.h"
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_future.hpp>
#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <future>
#include <vector>

using namespace hical;
using namespace hical::detail;

namespace
{
	using Codec = Http2Codec;
	constexpr size_t kFrameHeaderBytes = 9;
	constexpr std::array<Codec::Header, 4> kRequest {
		{{":method", "POST"}, {":scheme", "https"}, {":authority", "localhost"}, {":path", "/upload"}}};

	struct Forwarder : Codec::Listener
	{
		Http2Receive* receive {nullptr};
		std::vector<NetworkError> errors;

		Http2Code onBeginHeaders(int32_t id) override
		{
			return receive->onBeginHeaders(id);
		}

		Http2Code onHeader(int32_t id, Codec::Header field) override
		{
			return receive->onHeader(id, field);
		}

		Http2Code onEndHeaders(const Codec::HeaderBlock& block) override
		{
			return receive->onEndHeaders(block);
		}

		Http2Code onData(int32_t id, std::string_view bytes) override
		{
			return receive->onData(id, bytes);
		}

		void onEndStream(int32_t id) override
		{
			receive->onEndStream(id);
		}

		void onStreamClose(int32_t id, Http2Code code) override
		{
			receive->onStreamClose(id, code);
		}

		void onError(const NetworkError& error) override
		{
			errors.push_back(error);
			receive->onError(error);
		}
	};

	struct Endpoint
	{
		boost::asio::io_context io;
		Http2Options options;
		Http2ServiceBudget service;
		Http2Budget budget;
		Forwarder listener;
		Codec codec;
		std::vector<std::shared_ptr<Http2Stream>> ready;
		std::vector<uint16_t> informational;
		Http2Receive receive;

		Endpoint(Codec::Role role, Http2Options settings = {})
			: options(settings)
			, service(options.maxServerApplicationTasks)
			, budget(role == Codec::Role::hServer ? Http2Budget::Role::hServer : Http2Budget::Role::hClient,
					 options,
					 &service)
			, codec(role, options, &listener)
			, receive(io.get_executor(),
					  codec,
					  budget,
					  {role, options},
					  {[this](std::shared_ptr<Http2Stream> stream)
					   {
						   ready.push_back(std::move(stream));
					   },
					   [this](std::shared_ptr<Http2Stream>, const Http2Headers& headers)
					   {
						   informational.push_back(headers.status());
					   }})
		{
			listener.receive = &receive;
		}

		std::shared_ptr<Http2Stream> addClient(int32_t id, bool head = false)
		{
			Http2Stream::Config config;
			config.streamId = id;
			config.incomingKind = Codec::HeaderKind::hResponse;
			config.peerLimit = codec.peerMaxConcurrentStreams();
			config.options = options;
			auto stream = Http2Stream::create(io.get_executor(), config, budget);
			EXPECT_TRUE(stream);
			EXPECT_FALSE(receive.addClientStream(stream, {head}));
			return stream;
		}
	};

	std::string drain(Codec& codec)
	{
		std::string bytes;
		for (size_t round = 0; round < 128; ++round)
		{
			auto batch = codec.memSend();
			EXPECT_FALSE(batch.error) << batch.error.message;
			if (batch.error || batch.bytes.empty())
			{
				return bytes;
			}
			bytes += batch.bytes;
		}
		ADD_FAILURE() << "协议输出未在有限轮次内排空";
		return bytes;
	}

	void feed(Codec& codec, std::string_view bytes, size_t fragment = 0)
	{
		while (!bytes.empty())
		{
			auto part = bytes.substr(0, fragment == 0 ? bytes.size() : fragment);
			auto result = codec.memRecv(part);
			ASSERT_FALSE(result.error) << result.error.message;
			ASSERT_GT(result.consumed, 0U);
			bytes.remove_prefix(result.consumed);
		}
	}

	void exchange(Endpoint& client, Endpoint& server)
	{
		for (size_t round = 0; round < 16; ++round)
		{
			auto request = drain(client.codec);
			feed(server.codec, request);
			auto response = drain(server.codec);
			feed(client.codec, response);
			if (request.empty() && response.empty())
			{
				return;
			}
		}
		ADD_FAILURE() << "握手输出未收敛";
	}

	std::string dataFrame(int32_t id, std::string_view payload, bool end = false)
	{
		std::string frame(kFrameHeaderBytes, '\0');
		frame[0] = static_cast<char>(payload.size() >> 16);
		frame[1] = static_cast<char>(payload.size() >> 8);
		frame[2] = static_cast<char>(payload.size());
		frame[4] = end ? Codec::kEndStream : 0;
		for (size_t index = 0; index < sizeof(id); ++index)
		{
			frame[5 + index] = static_cast<char>(static_cast<uint32_t>(id) >> (24 - index * 8));
		}
		frame += payload;
		return frame;
	}

	uint32_t readWord(std::string_view bytes)
	{
		uint32_t value = 0;
		for (unsigned char byte : bytes)
		{
			value = (value << 8) | byte;
		}
		return value;
	}

	size_t windowCredit(std::string_view frames, int32_t id)
	{
		size_t credit = 0;
		while (frames.size() >= kFrameHeaderBytes)
		{
			auto length = readWord(frames.substr(0, 3));
			EXPECT_GE(frames.size(), kFrameHeaderBytes + length);
			if (frames.size() < kFrameHeaderBytes + length)
			{
				break;
			}
			if (static_cast<uint8_t>(frames[3]) == static_cast<uint8_t>(Codec::FrameType::hWindowUpdate)
				&& readWord(frames.substr(5, 4)) == static_cast<uint32_t>(id) && length == 4)
			{
				credit += readWord(frames.substr(kFrameHeaderBytes, 4)) & Http2Options::kMaxWindowSize;
			}
			frames.remove_prefix(kFrameHeaderBytes + length);
		}
		return credit;
	}

	int32_t openRequest(Endpoint& client, Endpoint& server, std::string_view length = {})
	{
		std::vector<Codec::Header> headers(kRequest.begin(), kRequest.end());
		if (!length.empty())
		{
			headers.push_back({"content-length", length});
		}
		auto submitted = client.codec.submitRequest(headers, false);
		EXPECT_FALSE(submitted.error);
		feed(server.codec, drain(client.codec));
		return submitted.streamId;
	}

	Http2Options tinyOptions()
	{
		Http2Options options;
		options.maxBodyBytes = 8;
		options.maxConnectionReceiveBytes = 12;
		options.maxResponseBytes = 8;
		options.maxClientConnectionReceiveBytes = 12;
		return options;
	}

	std::shared_ptr<Http2Stream> responseStream(Endpoint& client,
												Endpoint& server,
												std::span<const Codec::Header> headers,
												bool head = false)
	{
		auto requestHeaders = kRequest;
		requestHeaders[0].value = head ? "HEAD" : "POST";
		auto request = client.codec.submitRequest(requestHeaders);
		EXPECT_FALSE(request.error);
		auto stream = client.addClient(request.streamId, head);
		feed(server.codec, drain(client.codec));
		EXPECT_FALSE(server.codec.submitResponse(request.streamId, headers, false));
		feed(client.codec, drain(server.codec));
		return stream;
	}
} // namespace

TEST(Http2FlowControlTest, UploadLargerThanStreamAndConnectionWindowsCompletesWithoutPublishing)
{
	Http2Options options;
	options.initialStreamWindow = 1024;
	Endpoint client(Codec::Role::hClient);
	Endpoint server(Codec::Role::hServer, options);
	exchange(client, server);
	auto id = openRequest(client, server, "98304");
	auto stream = server.receive.find(id);
	ASSERT_TRUE(stream);
	auto wait = boost::asio::co_spawn(server.io, stream->waitBody(), boost::asio::use_future);
	std::string body(96 * 1024, 'u');
	size_t available = options.initialStreamWindow;
	size_t connectionAvailable = Http2Options::kDefaultWindowSize;
	size_t streamCredit = 0;
	size_t offset = 0;
	while (offset < body.size())
	{
		auto length = std::min({size_t {512}, body.size() - offset, available, connectionAvailable});
		ASSERT_GT(length, 0U) << "完整 body 等待不能阻止按已存储数据补窗";
		available -= length;
		connectionAvailable -= length;
		feed(server.codec,
			 dataFrame(id, std::string_view(body).substr(offset, length), offset + length == body.size()),
			 17);
		offset += length;
		auto updates = drain(server.codec);
		auto increment = windowCredit(updates, id);
		streamCredit += increment;
		available += increment;
		connectionAvailable += windowCredit(updates, 0);
		feed(client.codec, updates);
	}
	server.io.run();
	EXPECT_FALSE(wait.get());
	EXPECT_GT(streamCredit, options.initialStreamWindow);
	EXPECT_EQ(stream->body(), body);
	EXPECT_TRUE(stream->request().body().empty());
	EXPECT_FALSE(stream->publishBody());
	EXPECT_EQ(stream->request().body(), body);
}

TEST(Http2FlowControlTest, UnknownLengthAndMergedDataCompleteOnlyAtEndStream)
{
	Endpoint client(Codec::Role::hClient);
	Endpoint server(Codec::Role::hServer);
	exchange(client, server);
	auto id = openRequest(client, server);
	auto stream = server.receive.find(id);
	ASSERT_TRUE(stream);
	feed(server.codec, dataFrame(id, "ab") + dataFrame(id, "cd"), 1);
	EXPECT_FALSE(stream->bodyComplete());
	EXPECT_TRUE(stream->body().empty());
	feed(server.codec, dataFrame(id, "ef", true));
	EXPECT_TRUE(stream->bodyComplete());
	EXPECT_EQ(stream->body(), "abcdef");
	EXPECT_TRUE(stream->request().body().empty());
}

TEST(Http2FlowControlTest, ShortContentLengthFailsOnlyItsBodyWaiter)
{
	Endpoint client(Codec::Role::hClient);
	Endpoint server(Codec::Role::hServer);
	exchange(client, server);
	auto firstId = openRequest(client, server, "4");
	auto first = server.receive.find(firstId);
	auto secondId = openRequest(client, server);
	auto second = server.receive.find(secondId);
	ASSERT_TRUE(first);
	ASSERT_TRUE(second);
	auto failed = boost::asio::co_spawn(server.io, first->waitBody(), boost::asio::use_future);
	auto completed = boost::asio::co_spawn(server.io, second->waitBody(), boost::asio::use_future);
	feed(server.codec, dataFrame(firstId, "ab", true) + dataFrame(secondId, "ok", true));
	// 协议库可先排队 RST，再于序列化复位帧时通知流关闭。
	drain(server.codec);
	server.io.run();
	EXPECT_TRUE(failed.get());
	EXPECT_FALSE(completed.get());
	EXPECT_FALSE(server.receive.find(firstId));
	EXPECT_EQ(second->body(), "ok");
}

TEST(Http2FlowControlTest, ExcessContentLengthIsRejectedBeforeStoringNewPayload)
{
	Endpoint client(Codec::Role::hClient);
	Endpoint server(Codec::Role::hServer);
	exchange(client, server);
	auto id = openRequest(client, server, "2");
	auto stream = server.receive.find(id);
	ASSERT_TRUE(stream);
	EXPECT_EQ(server.receive.onData(id, "abc"), Http2Code::hProtocolError);
	EXPECT_EQ(stream->receivedBytes(), 0U);
	EXPECT_FALSE(server.receive.find(id));
	EXPECT_TRUE(stream->bodyComplete());
}

TEST(Http2FlowControlTest, AdapterEndStreamChecksLengthBeforeCompletingBody)
{
	Endpoint client(Codec::Role::hClient);
	Endpoint server(Codec::Role::hServer);
	exchange(client, server);
	auto id = openRequest(client, server, "4");
	auto stream = server.receive.find(id);
	ASSERT_TRUE(stream);
	feed(server.codec, dataFrame(id, "ab"));
	server.receive.onEndStream(id);
	EXPECT_EQ(stream->error().code, ErrorCode::hHttp2StreamError);
	ASSERT_TRUE(stream->error().http2);
	EXPECT_EQ(stream->error().http2->code, static_cast<uint32_t>(Http2Code::hProtocolError));
	EXPECT_TRUE(stream->bodyComplete());
	EXPECT_TRUE(stream->body().empty());
	EXPECT_TRUE(stream->publishBody());
	EXPECT_FALSE(server.receive.find(id));
}

TEST(Http2FlowControlTest, BodyLimitDropsMapOwnerAndKeepsRunningApplicationBytes)
{
	Endpoint client(Codec::Role::hClient);
	Endpoint server(Codec::Role::hServer, tinyOptions());
	exchange(client, server);
	auto id = openRequest(client, server);
	auto stream = server.receive.find(id);
	ASSERT_TRUE(stream);
	auto application = stream->startApplication();
	ASSERT_TRUE(application);
	feed(server.codec, dataFrame(id, "12345678"));
	feed(server.codec, dataFrame(id, "x"));
	EXPECT_FALSE(server.receive.find(id));
	EXPECT_EQ(stream->error().code, ErrorCode::hResourceLimitExceeded);
	EXPECT_EQ(server.budget.usage().receiveBytes, 8U);
	EXPECT_EQ(server.budget.usage().protocolStreams, 0U);
	EXPECT_EQ(server.service.used(), 1U);
	server.ready.clear();
	stream.reset();
	EXPECT_EQ(server.budget.usage().receiveBytes, 8U);
	application.reset();
	EXPECT_EQ(server.budget.usage().receiveBytes, 0U);
	EXPECT_EQ(server.service.used(), 0U);
}

TEST(Http2FlowControlTest, AggregateLimitRejectsCurrentStreamAndLetsOtherPartialBodyFinish)
{
	Endpoint client(Codec::Role::hClient);
	Endpoint server(Codec::Role::hServer, tinyOptions());
	exchange(client, server);
	auto firstId = openRequest(client, server);
	auto secondId = openRequest(client, server);
	auto first = server.receive.find(firstId);
	ASSERT_TRUE(first);
	std::weak_ptr<Http2Stream> rejected = server.receive.find(secondId);
	server.ready.clear();
	feed(server.codec, dataFrame(firstId, "123456") + dataFrame(secondId, "123456"));
	EXPECT_EQ(server.budget.usage().receiveBytes, 12U);
	feed(server.codec, dataFrame(secondId, "x") + dataFrame(firstId, "78", true));
	EXPECT_TRUE(rejected.expired());
	EXPECT_EQ(server.budget.usage().receiveBytes, 8U);
	EXPECT_EQ(first->body(), "12345678");
	EXPECT_FALSE(first->error());
}

TEST(Http2FlowControlTest, RefusedAndLateDataReturnConnectionCreditExactlyOnce)
{
	Endpoint client(Codec::Role::hClient);
	Endpoint server(Codec::Role::hServer, tinyOptions());
	exchange(client, server);
	auto bad = openRequest(client, server);
	auto good = openRequest(client, server);
	auto goodStream = server.receive.find(good);
	ASSERT_TRUE(goodStream);
	std::string large(Http2Options::kMinFrameBytes, 'x');
	std::string packet = dataFrame(bad, large) + dataFrame(bad, large) + dataFrame(good, "ok", true)
						 + dataFrame(bad, large) + dataFrame(bad, large);
	feed(server.codec, packet);
	auto output = drain(server.codec);
	auto credit = windowCredit(output, 0);
	EXPECT_GE(credit, Http2Options::kDefaultWindowSize / 2);
	EXPECT_LE(credit, large.size() * 4 + 2);
	EXPECT_EQ(goodStream->body(), "ok");
	EXPECT_FALSE(goodStream->error());
	EXPECT_FALSE(server.receive.find(bad));
	EXPECT_TRUE(std::ranges::none_of(server.listener.errors,
									 [](const auto& error)
									 {
										 return error.code == ErrorCode::hHttp2ConnectionError;
									 }));
}

TEST(Http2FlowControlTest, LocalCancellationRejectsLatePayloadWithoutResurrectingStream)
{
	Endpoint client(Codec::Role::hClient);
	Endpoint server(Codec::Role::hServer);
	exchange(client, server);
	auto id = openRequest(client, server);
	auto stream = server.receive.find(id);
	ASSERT_TRUE(stream);
	auto wait = boost::asio::co_spawn(server.io, stream->waitBody(), boost::asio::use_future);
	server.receive.cancel(id);
	EXPECT_EQ(server.receive.onData(id, "late"), Http2Code::hStreamClosed);
	EXPECT_EQ(server.receive.onBeginHeaders(id), Http2Code::hStreamClosed);
	server.receive.onEndStream(id);
	server.io.run();
	EXPECT_TRUE(wait.get());
	EXPECT_EQ(stream->receivedBytes(), 0U);
	EXPECT_EQ(server.receive.size(), 0U);
}

TEST(Http2FlowControlTest, TrailersStayIndependentAndCompleteTheCorrectBodyGate)
{
	Endpoint client(Codec::Role::hClient);
	Endpoint server(Codec::Role::hServer);
	exchange(client, server);
	auto id = openRequest(client, server);
	auto stream = server.receive.find(id);
	ASSERT_TRUE(stream);
	feed(server.codec, dataFrame(id, "body"));
	constexpr std::array<Codec::Header, 1> trailers {{{"x-checksum", "four"}}};
	ASSERT_FALSE(client.codec.submitTrailers(id, trailers));
	feed(server.codec, drain(client.codec));
	EXPECT_TRUE(stream->bodyComplete());
	EXPECT_EQ(stream->trailers().find("x-checksum"), "four");
	EXPECT_TRUE(stream->initialHeaders().find("x-checksum").empty());
	EXPECT_EQ(stream->body(), "body");
	EXPECT_EQ(server.ready.size(), 1U);
}

TEST(Http2FlowControlTest, ForbiddenTrailerFieldFailsOnlyThatStream)
{
	Endpoint client(Codec::Role::hClient);
	Endpoint server(Codec::Role::hServer);
	exchange(client, server);
	auto id = openRequest(client, server);
	auto stream = server.receive.find(id);
	ASSERT_TRUE(stream);
	ASSERT_EQ(server.receive.onBeginHeaders(id), Http2Code::hNoError);
	EXPECT_EQ(server.receive.onHeader(id, {"content-length", "0"}), Http2Code::hProtocolError);
	EXPECT_FALSE(server.receive.find(id));
	EXPECT_TRUE(stream->bodyComplete());
	EXPECT_TRUE(stream->trailers().fields().empty());
}

TEST(Http2FlowControlTest, InformationalBlocksDoNotSealOrPolluteFinalHeaders)
{
	Endpoint client(Codec::Role::hClient);
	Endpoint server(Codec::Role::hServer);
	exchange(client, server);
	auto request = client.codec.submitRequest(kRequest);
	ASSERT_FALSE(request.error);
	auto stream = client.addClient(request.streamId);
	feed(server.codec, drain(client.codec));
	constexpr std::array<Codec::Header, 2> early {{{":status", "103"}, {"link", "</early>"}}};
	constexpr std::array<Codec::Header, 1> interim {{{":status", "100"}}};
	constexpr std::array<Codec::Header, 2> final {{{":status", "200"}, {"content-length", "2"}}};
	ASSERT_FALSE(server.codec.submitResponse(request.streamId, early, false));
	feed(client.codec, drain(server.codec));
	ASSERT_FALSE(server.codec.submitResponse(request.streamId, interim, false));
	feed(client.codec, drain(server.codec));
	EXPECT_FALSE(stream->initialHeaders().sealed());
	EXPECT_FALSE(stream->bodyComplete());
	EXPECT_TRUE(client.ready.empty());
	ASSERT_FALSE(server.codec.submitResponse(request.streamId, final, false));
	feed(client.codec, drain(server.codec));
	EXPECT_EQ(client.informational, (std::vector<uint16_t> {103, 100}));
	EXPECT_EQ(client.ready.size(), 1U);
	EXPECT_EQ(stream->initialHeaders().status(), 200);
	EXPECT_TRUE(stream->initialHeaders().find("link").empty());
	feed(client.codec, dataFrame(request.streamId, "ok", true));
	EXPECT_EQ(stream->body(), "ok");
	EXPECT_TRUE(stream->response().body().empty());
	EXPECT_FALSE(stream->publishBody());
	EXPECT_EQ(stream->response().body(), "ok");
}

TEST(Http2FlowControlTest, HeadAndNotModifiedAllowRepresentationLengthWithNoData)
{
	for (bool head : {false, true})
	{
		Endpoint client(Codec::Role::hClient);
		Endpoint server(Codec::Role::hServer);
		exchange(client, server);
		std::array<Codec::Header, 2> headers {{{":status", head ? "200" : "304"}, {"content-length", "999"}}};
		auto stream = responseStream(client, server, headers, head);
		ASSERT_TRUE(stream);
		feed(client.codec, dataFrame(stream->id(), "", true));
		EXPECT_FALSE(stream->error());
		EXPECT_TRUE(stream->bodyComplete());
		EXPECT_TRUE(stream->body().empty());
	}
}

TEST(Http2FlowControlTest, HeadNoContentAndNotModifiedRejectNonEmptyPayload)
{
	for (auto status : {"200", "204", "304"})
	{
		Endpoint client(Codec::Role::hClient);
		Endpoint server(Codec::Role::hServer);
		exchange(client, server);
		std::array<Codec::Header, 1> headers {{{":status", status}}};
		auto stream = responseStream(client, server, headers, std::string_view(status) == "200");
		ASSERT_TRUE(stream);
		EXPECT_EQ(client.receive.onData(stream->id(), "x"), Http2Code::hProtocolError);
		EXPECT_EQ(stream->receivedBytes(), 0U);
		EXPECT_FALSE(client.receive.find(stream->id()));
	}
}

TEST(Http2FlowControlTest, DeclaredBodyOverBudgetIsRefusedBeforeApplicationStarts)
{
	Endpoint client(Codec::Role::hClient);
	Endpoint server(Codec::Role::hServer, tinyOptions());
	exchange(client, server);
	auto id = openRequest(client, server, "9");
	EXPECT_FALSE(server.receive.find(id));
	EXPECT_TRUE(server.ready.empty());
	EXPECT_EQ(server.budget.usage().receiveBytes, 0U);
	EXPECT_EQ(server.budget.usage().protocolStreams, 0U);
	EXPECT_EQ(server.service.used(), 0U);
}

TEST(Http2FlowControlTest, ConnectionFailureWakesAllStreamsAndRetainsApplicationGuard)
{
	Endpoint client(Codec::Role::hClient);
	Endpoint server(Codec::Role::hServer);
	exchange(client, server);
	auto firstId = openRequest(client, server);
	auto secondId = openRequest(client, server);
	auto first = server.receive.find(firstId);
	auto second = server.receive.find(secondId);
	ASSERT_TRUE(first);
	ASSERT_TRUE(second);
	auto application = first->startApplication();
	auto firstWait = boost::asio::co_spawn(server.io, first->waitBody(), boost::asio::use_future);
	auto secondWait = boost::asio::co_spawn(server.io, second->waitBody(), boost::asio::use_future);
	server.receive.fail({ErrorCode::hConnectionReset, "连接断开"});
	server.receive.fail({ErrorCode::hTimedOut, "迟到失败"});
	server.io.run();
	EXPECT_EQ(firstWait.get().code, ErrorCode::hConnectionReset);
	EXPECT_EQ(secondWait.get().code, ErrorCode::hConnectionReset);
	EXPECT_EQ(server.receive.size(), 0U);
	EXPECT_EQ(server.budget.usage().protocolStreams, 0U);
	EXPECT_EQ(server.service.used(), 1U);
	application.reset();
	EXPECT_EQ(server.service.used(), 0U);
}

TEST(Http2FlowControlTest, HeadersNotificationMayCancelItsOwnStreamSynchronously)
{
	boost::asio::io_context io;
	Http2Options options;
	Http2ServiceBudget service(2);
	Http2Budget budget(Http2Budget::Role::hServer, options, &service);
	Codec codec(Codec::Role::hServer);
	Http2Receive* owner = nullptr;
	Http2Receive receive(io.get_executor(),
						 codec,
						 budget,
						 {Codec::Role::hServer, options},
						 {[&](std::shared_ptr<Http2Stream> stream)
						  {
							  owner->cancel(stream->id());
						  },
						  {}});
	owner = &receive;
	ASSERT_EQ(receive.onBeginHeaders(1), Http2Code::hNoError);
	for (auto field : kRequest)
	{
		ASSERT_EQ(receive.onHeader(1, field), Http2Code::hNoError);
	}
	EXPECT_EQ(receive.onEndHeaders({1, Codec::HeaderKind::hRequest, 0, false}), Http2Code::hCancel);
	EXPECT_EQ(receive.size(), 0U);
	EXPECT_EQ(budget.usage().applicationTasks, 0U);
}

TEST(Http2FlowControlTest, ClosedClientStreamDoesNotAcceptUnregisteredOrRepeatedResponse)
{
	Endpoint client(Codec::Role::hClient);
	EXPECT_EQ(client.receive.onBeginHeaders(1), Http2Code::hStreamClosed);
	EXPECT_EQ(client.receive.onData(1, "x"), Http2Code::hStreamClosed);
	EXPECT_TRUE(client.receive.addClientStream({}, {}));
	EXPECT_EQ(client.receive.size(), 0U);
}
