/**
 * @file test_http2_output.cpp
 * @brief HTTP/2 DATA provider、有界输出和写入确认测试
 */

#include "core/http2/Http2Output.h"
#include <boost/asio/use_future.hpp>
#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <future>
#include <map>
#include <string>
#include <vector>

using namespace hical;
using namespace hical::detail;

namespace
{
	using Codec = Http2Codec;
	constexpr std::array<Codec::Header, 4> kRequest {
		{{":method", "POST"}, {":scheme", "https"}, {":authority", "localhost"}, {":path", "/"}}};
	constexpr std::array<Codec::Header, 1> kResponse {{{":status", "200"}}};

	struct Recorder : Codec::Listener
	{
		Codec* codec {nullptr};
		bool consume {true};
		std::map<int32_t, std::string> body;
		std::vector<int32_t> ended;
		std::vector<Codec::HeaderBlock> blocks;
		std::vector<Codec::Frame> frames;
		std::vector<std::pair<std::string, std::string>> headers;

		Http2Code onData(int32_t id, std::string_view bytes) override
		{
			body[id].append(bytes);
			if (consume)
			{
				EXPECT_TRUE(codec->consume(id, bytes.size()).ok());
			}
			return Http2Code::hNoError;
		}

		Http2Code onHeader(int32_t, Codec::Header field) override
		{
			headers.emplace_back(field.name, field.value);
			return Http2Code::hNoError;
		}

		Http2Code onEndHeaders(const Codec::HeaderBlock& block) override
		{
			blocks.push_back(block);
			return Http2Code::hNoError;
		}

		void onEndStream(int32_t id) override
		{
			ended.push_back(id);
		}

		void onFrameReceived(const Codec::Frame& frame) override
		{
			frames.push_back(frame);
		}
	};

	struct Pair
	{
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

		explicit Pair(const Http2Options& options = {})
			: client(Codec::Role::hClient, options, &clientEvents)
			, server(Codec::Role::hServer, options, &serverEvents)
			, clientBudget(Http2Budget::Role::hClient, options)
			, serverBudget(Http2Budget::Role::hServer, options, &service)
			, clientOutput(client, clientBudget, options)
			, serverOutput(server, serverBudget, options)
		{
			clientEvents.codec = &client;
			serverEvents.codec = &server;
		}

		bool pump(Http2Output& output, Codec& peer, size_t part = static_cast<size_t>(-1))
		{
			auto batch = output.nextBatch();
			EXPECT_TRUE(batch.error.ok()) << batch.error.message;
			if (batch.error || batch.bytes.empty())
			{
				return false;
			}
			const auto bytes = batch.bytes.substr(0, std::min(part, batch.bytes.size()));
			const auto received = peer.memRecv(bytes);
			EXPECT_TRUE(received.error.ok()) << received.error.message;
			EXPECT_EQ(received.consumed, bytes.size());
			auto acknowledged = output.acknowledge(bytes.size());
			EXPECT_TRUE(acknowledged.error.ok());
			written.insert(written.end(), acknowledged.endedStreams.begin(), acknowledged.endedStreams.end());
			return true;
		}

		void exchange()
		{
			for (size_t count = 0; count < 16384; ++count)
			{
				bool progress = pump(clientOutput, server);
				progress = pump(serverOutput, client) || progress;
				if (!progress)
				{
					return;
				}
			}
			ADD_FAILURE() << "输出未在有限轮次内收敛";
		}
	};
} // namespace

TEST(Http2OutputTest, InflightReadPermitSurvivesProducerCancellationUntilReadOwnerReleases)
{
	Pair pair;
	pair.exchange();
	auto request = pair.clientOutput.submitRequest(kRequest);
	ASSERT_FALSE(request.error);
	ASSERT_FALSE(request.producer->finish());
	pair.exchange();
	auto response = pair.serverOutput.submitResponse(request.streamId, kResponse);
	ASSERT_FALSE(response.error);
	const auto baseline = pair.serverBudget.usage().sendBytes;
	boost::asio::io_context io;
	auto result = boost::asio::co_spawn(
		io,
		response.producer->reserveRead(1024, std::chrono::steady_clock::now() + std::chrono::seconds {1}),
		boost::asio::use_future);
	io.run();
	auto reserved = result.get();
	ASSERT_FALSE(reserved.error);
	ASSERT_EQ(reserved.permit.size(), 1024U);
	EXPECT_EQ(pair.serverBudget.usage().sendBytes, baseline + 1024);
	response.producer->cancel();
	response.producer->cancel();
	EXPECT_EQ(pair.serverOutput.queuedBytes(), 0U);
	EXPECT_EQ(pair.serverBudget.usage().sendBytes, baseline + 1024);
	reserved.permit = {};
	EXPECT_EQ(pair.serverBudget.usage().sendBytes, baseline);
}

TEST(Http2OutputTest, ReadPermitTransfersToOwnedQueueWithoutDoubleChargingAndRollsBackForeignUse)
{
	Http2Options options;
	options.maxHeaderBytes = 512;
	options.maxConnectionSendBytes = 4096;
	options.maxOutputBatchBytes = 256;
	options.maxFilePrefetchBytes = 4096;
	Pair pair(options);
	pair.exchange();
	auto request = pair.clientOutput.submitRequest(kRequest);
	ASSERT_FALSE(request.error);
	ASSERT_FALSE(request.producer->finish());
	pair.exchange();
	auto response = pair.serverOutput.submitResponse(request.streamId, kResponse);
	ASSERT_FALSE(response.error);
	const auto baseline = pair.serverBudget.usage().sendBytes;
	const auto amount = options.maxConnectionSendBytes - baseline;
	boost::asio::io_context io;
	auto result = boost::asio::co_spawn(
		io,
		response.producer->reserveRead(amount, std::chrono::steady_clock::now() + std::chrono::seconds {1}),
		boost::asio::use_future);
	io.run();
	auto reserved = result.get();
	ASSERT_FALSE(reserved.error);
	std::string payload(reserved.permit.size(), 'r');
	ASSERT_FALSE(response.producer->appendReserved(std::move(payload), std::move(reserved.permit)));
	EXPECT_TRUE(payload.empty());
	EXPECT_EQ(pair.serverBudget.usage().sendBytes, options.maxConnectionSendBytes);
	ASSERT_FALSE(response.producer->finish());
	pair.exchange();
	EXPECT_EQ(pair.clientEvents.body[request.streamId], std::string(amount, 'r'));
	EXPECT_EQ(pair.serverBudget.usage().sendBytes, baseline);

	io.restart();
	auto request2 = pair.clientOutput.submitRequest(kRequest);
	ASSERT_FALSE(request2.error);
	ASSERT_FALSE(request2.producer->finish());
	pair.exchange();
	auto response2 = pair.serverOutput.submitResponse(request2.streamId, kResponse);
	ASSERT_FALSE(response2.error);
	auto foreign = boost::asio::co_spawn(
		io,
		response2.producer->reserveRead(128, std::chrono::steady_clock::now() + std::chrono::seconds {1}),
		boost::asio::use_future);
	io.run();
	auto permit = foreign.get();
	ASSERT_FALSE(permit.error);
	std::string bytes(128, 'f');
	EXPECT_EQ(request2.producer->appendReserved(std::move(bytes), std::move(permit.permit)).code,
			  ErrorCode::hInvalidConfiguration);
	EXPECT_EQ(bytes, std::string(128, 'f'));
	EXPECT_EQ(pair.serverBudget.usage().sendBytes, baseline);
}

TEST(Http2OutputTest, ReadWaitAtZeroWindowDoesNotWakeWriterAndSettingsResumeIndependentWait)
{
	Http2Options options;
	options.initialStreamWindow = 0;
	Pair pair(options);
	pair.exchange();
	auto request = pair.clientOutput.submitRequest(kRequest);
	ASSERT_FALSE(request.error);
	ASSERT_FALSE(request.producer->finish());
	pair.exchange();
	auto response = pair.serverOutput.submitResponse(request.streamId, kResponse);
	ASSERT_FALSE(response.error);
	pair.exchange();
	size_t writerWakeups = 0;
	pair.serverOutput.setWakeup(
		[&]
		{
			++writerWakeups;
		});
	boost::asio::io_context io;
	auto result = boost::asio::co_spawn(
		io,
		response.producer->reserveRead(4096, std::chrono::steady_clock::now() + std::chrono::seconds {1}),
		boost::asio::use_future);
	io.poll();
	EXPECT_EQ(result.wait_for(std::chrono::seconds {0}), std::future_status::timeout);
	EXPECT_EQ(writerWakeups, 0U);
	const std::array<Codec::Setting, 1> settings {{{Http2Setting::hInitialWindowSize, 1024}}};
	ASSERT_FALSE(pair.client.submitSettings(settings));
	pair.exchange();
	io.restart();
	io.run();
	auto reserved = result.get();
	ASSERT_FALSE(reserved.error);
	EXPECT_EQ(reserved.permit.size(), 1024U);
	EXPECT_EQ(writerWakeups, 0U);
	pair.serverOutput.setWakeup({});
}

TEST(Http2OutputTest, ReadWaitCancellationAndApplicationTaskLeaseRetireExactlyOnce)
{
	Http2Options options;
	options.initialStreamWindow = 0;
	options.maxApplicationTasks = 1;
	Pair pair(options);
	pair.exchange();
	auto request = pair.clientOutput.submitRequest(kRequest);
	ASSERT_FALSE(request.error);
	ASSERT_FALSE(request.producer->finish());
	pair.exchange();
	auto response = pair.serverOutput.submitResponse(request.streamId, kResponse);
	ASSERT_FALSE(response.error);
	auto task = response.producer->acquireApplicationTask();
	ASSERT_TRUE(task);
	EXPECT_EQ(pair.service.used(), 1U);
	EXPECT_FALSE(response.producer->acquireApplicationTask());
	size_t cancellations = 0;
	response.producer->setCancelHandler(
		[&]
		{
			++cancellations;
		});
	boost::asio::io_context io;
	auto result = boost::asio::co_spawn(
		io,
		response.producer->reserveRead(1024, std::chrono::steady_clock::now() + std::chrono::seconds {1}),
		boost::asio::use_future);
	io.poll();
	response.producer->cancel();
	response.producer->cancel();
	io.restart();
	io.run();
	EXPECT_EQ(result.get().error.code, ErrorCode::hOperationAborted);
	EXPECT_EQ(cancellations, 1U);
	EXPECT_EQ(pair.service.used(), 1U);
	task.reset();
	task.reset();
	EXPECT_EQ(pair.service.used(), 0U);
	response.producer->setCancelHandler({});
}

TEST(Http2OutputTest, DeferredProducerResumesWithOwnedBytesAndTrailers)
{
	Pair pair;
	pair.exchange();
	auto request = pair.clientOutput.submitRequest(kRequest);
	ASSERT_TRUE(request.error.ok());
	pair.exchange();
	EXPECT_TRUE(pair.serverEvents.body.empty());
	EXPECT_TRUE(pair.serverEvents.ended.empty());
	{
		std::string body = "owned bytes";
		ASSERT_TRUE(request.producer->append(body).ok());
		body.assign("overwritten");
	}
	const std::array<Codec::Header, 1> trailers {{{"x-checksum", "final"}}};
	ASSERT_TRUE(request.producer->finish(trailers).ok());
	pair.exchange();
	EXPECT_EQ(pair.serverEvents.body[request.streamId], "owned bytes");
	ASSERT_EQ(pair.serverEvents.ended.size(), 1U);
	EXPECT_EQ(pair.serverEvents.ended.front(), request.streamId);
	ASSERT_EQ(pair.serverEvents.blocks.size(), 2U);
	EXPECT_EQ(pair.serverEvents.blocks.back().kind, Codec::HeaderKind::hTrailers);
	EXPECT_EQ(pair.serverEvents.headers.back(), std::make_pair(std::string("x-checksum"), std::string("final")));
}

TEST(Http2OutputTest, PhysicalEndRequiresEveryByteOfSmallPartialBatches)
{
	Http2Options options;
	options.maxOutputBatchBytes = 1;
	Pair pair(options);
	pair.exchange();
	auto request = pair.clientOutput.submitRequest(kRequest);
	ASSERT_TRUE(request.error.ok());
	ASSERT_TRUE(request.producer->append("body").ok());
	ASSERT_TRUE(request.producer->finish().ok());
	for (size_t count = 0; count < 1024 && pair.serverEvents.ended.empty(); ++count)
	{
		auto batch = pair.clientOutput.nextBatch();
		ASSERT_TRUE(batch.error.ok());
		ASSERT_EQ(batch.bytes.size(), 1U);
		const auto repeated = pair.clientOutput.nextBatch();
		ASSERT_EQ(repeated.bytes.data(), batch.bytes.data());
		ASSERT_EQ(repeated.bytes, batch.bytes);
		auto result = pair.server.memRecv(batch.bytes);
		ASSERT_TRUE(result.error.ok());
		auto acknowledged = pair.clientOutput.acknowledge(1);
		ASSERT_TRUE(acknowledged.error.ok());
		EXPECT_EQ(acknowledged.endedStreams.empty(), pair.serverEvents.ended.empty());
		if (!acknowledged.endedStreams.empty())
		{
			EXPECT_EQ(acknowledged.endedStreams.front(), request.streamId);
		}
	}
	ASSERT_EQ(pair.serverEvents.ended.size(), 1U);
}

TEST(Http2OutputTest, FullPayloadBudgetStillSendsDataAndControlFrames)
{
	Http2Options options;
	options.maxHeaderBytes = 512;
	options.maxConnectionSendBytes = 4096;
	options.maxOutputBatchBytes = 256;
	options.maxFilePrefetchBytes = 256;
	Pair pair(options);
	pair.exchange();
	auto request = pair.clientOutput.submitRequest(kRequest);
	ASSERT_TRUE(request.error.ok());
	const auto available = options.maxConnectionSendBytes - pair.clientBudget.usage().sendBytes;
	std::string body(available, 'x');
	ASSERT_TRUE(request.producer->appendOwned(std::move(body)).ok());
	EXPECT_EQ(pair.clientBudget.usage().sendBytes, options.maxConnectionSendBytes);
	EXPECT_EQ(request.producer->append("overflow").code, ErrorCode::hResourceLimitExceeded);
	ASSERT_TRUE(request.producer->finish().ok());
	const std::array<Codec::Setting, 1> setting {{{Http2Setting::hMaxConcurrentStreams, 32}}};
	ASSERT_TRUE(pair.client.submitSettings(setting).ok());
	pair.exchange();
	EXPECT_EQ(pair.serverEvents.body[request.streamId], std::string(available, 'x'));
	EXPECT_EQ(pair.clientOutput.queuedBytes(), 0U);
	EXPECT_LE(pair.clientBudget.usage().sendBytes, options.maxConnectionSendBytes);
	EXPECT_EQ(pair.server.peerMaxConcurrentStreams(), 32U);
}

TEST(Http2OutputTest, ZeroStreamWindowPausesThenResumesWithoutBlockingAnotherStream)
{
	Pair pair;
	pair.exchange();
	const std::array<Codec::Setting, 1> paused {{{Http2Setting::hInitialWindowSize, 0}}};
	ASSERT_TRUE(pair.server.submitSettings(paused).ok());
	pair.exchange();
	auto request = pair.clientOutput.submitRequest(kRequest);
	ASSERT_TRUE(request.error.ok());
	ASSERT_TRUE(request.producer->append("paused body").ok());
	ASSERT_TRUE(request.producer->finish().ok());
	pair.exchange();
	EXPECT_TRUE(pair.serverEvents.body.empty());
	auto second = pair.client.submitRequest(kRequest);
	ASSERT_TRUE(second.error.ok());
	pair.exchange();
	EXPECT_NE(std::find(pair.serverEvents.ended.begin(), pair.serverEvents.ended.end(), second.streamId),
			  pair.serverEvents.ended.end());
	const std::array<Codec::Setting, 1> resumed {{{Http2Setting::hInitialWindowSize, 65535}}};
	ASSERT_TRUE(pair.server.submitSettings(resumed).ok());
	pair.exchange();
	EXPECT_EQ(pair.serverEvents.body[request.streamId], "paused body");
}

TEST(Http2OutputTest, ResetRejectsLateProductionAndPreservesAnotherResponse)
{
	Pair pair;
	pair.exchange();
	auto first = pair.client.submitRequest(kRequest);
	auto second = pair.client.submitRequest(kRequest);
	ASSERT_TRUE(first.error.ok());
	ASSERT_TRUE(second.error.ok());
	pair.exchange();
	auto rejected = pair.serverOutput.submitResponse(first.streamId, kResponse);
	auto accepted = pair.serverOutput.submitResponse(second.streamId, kResponse);
	ASSERT_TRUE(rejected.error.ok());
	ASSERT_TRUE(accepted.error.ok());
	ASSERT_TRUE(rejected.producer->append("discarded").ok());
	ASSERT_TRUE(pair.client.reset(first.streamId).ok());
	pair.exchange();
	EXPECT_FALSE(rejected.producer->append("late").ok());
	EXPECT_FALSE(rejected.producer->finish().ok());
	ASSERT_TRUE(accepted.producer->append("survives").ok());
	ASSERT_TRUE(accepted.producer->finish().ok());
	pair.exchange();
	EXPECT_EQ(pair.clientEvents.body[second.streamId], "survives");
}

TEST(Http2OutputTest, CloseKeepsInflightViewStableAndMakesProducersFail)
{
	Pair pair;
	auto batch = pair.clientOutput.nextBatch();
	ASSERT_TRUE(batch.error.ok());
	ASSERT_FALSE(batch.bytes.empty());
	const std::string copy(batch.bytes);
	const auto* original = batch.bytes.data();
	pair.clientOutput.close();
	EXPECT_EQ(batch.bytes.data(), original);
	EXPECT_EQ(batch.bytes, copy);
	EXPECT_FALSE(pair.clientOutput.nextBatch().error.ok());
	EXPECT_TRUE(pair.clientOutput.acknowledge(copy.size()).endedStreams.empty());
}

TEST(Http2OutputTest, PartialAcknowledgementKeepsSuffixAndRejectsOversizedAck)
{
	Pair pair;
	pair.exchange();
	auto request = pair.clientOutput.submitRequest(kRequest);
	ASSERT_TRUE(request.error.ok());
	ASSERT_TRUE(request.producer->append("physical write boundary").ok());
	ASSERT_TRUE(request.producer->finish().ok());
	bool ended = false;
	for (size_t count = 0; count < 64 && !ended; ++count)
	{
		auto batch = pair.clientOutput.nextBatch();
		ASSERT_TRUE(batch.error.ok());
		ASSERT_GT(batch.bytes.size(), 1U);
		const auto* suffix = batch.bytes.data() + batch.bytes.size() - 1;
		EXPECT_EQ(pair.clientOutput.acknowledge(batch.bytes.size() + 1).error.code, ErrorCode::hInvalidConfiguration);
		EXPECT_EQ(pair.clientOutput.nextBatch().bytes, batch.bytes);
		ASSERT_TRUE(pair.server.memRecv(batch.bytes.substr(0, batch.bytes.size() - 1)).error.ok());
		auto prefixAck = pair.clientOutput.acknowledge(batch.bytes.size() - 1);
		ASSERT_TRUE(prefixAck.error.ok());
		EXPECT_TRUE(prefixAck.endedStreams.empty());
		auto remaining = pair.clientOutput.nextBatch();
		ASSERT_EQ(remaining.bytes.size(), 1U);
		EXPECT_EQ(remaining.bytes.data(), suffix);
		ASSERT_TRUE(pair.server.memRecv(remaining.bytes).error.ok());
		auto finalAck = pair.clientOutput.acknowledge(1);
		ASSERT_TRUE(finalAck.error.ok());
		ended = !finalAck.endedStreams.empty();
		EXPECT_EQ(ended, !pair.serverEvents.ended.empty());
	}
	EXPECT_TRUE(ended);
}

TEST(Http2OutputTest, LargeTrailerWaitsForFinalContinuationPhysicalWrite)
{
	Http2Options options;
	options.maxHeaderBytes = 64 * 1024;
	Pair pair(options);
	pair.exchange();
	auto request = pair.clientOutput.submitRequest(kRequest);
	ASSERT_TRUE(request.error.ok());
	std::string value;
	uint32_t random = 0x3198ab45;
	for (size_t count = 0; count < 40000; ++count)
	{
		random ^= random << 13;
		random ^= random >> 17;
		random ^= random << 5;
		value.push_back(static_cast<char>('!' + random % 94));
	}
	const std::array<Codec::Header, 1> trailers {{{"x-large", value}}};
	ASSERT_TRUE(request.producer->finish(trailers).ok());
	bool ended = false;
	std::string wire;
	for (size_t count = 0; count < 64 && !ended; ++count)
	{
		auto batch = pair.clientOutput.nextBatch();
		ASSERT_TRUE(batch.error.ok()) << batch.error.message;
		ASSERT_FALSE(batch.bytes.empty());
		wire.append(batch.bytes);
		const auto prefix = batch.bytes.substr(0, batch.bytes.size() - 1);
		ASSERT_TRUE(pair.server.memRecv(prefix).error.ok());
		EXPECT_TRUE(pair.clientOutput.acknowledge(prefix.size()).endedStreams.empty());
		auto last = pair.clientOutput.nextBatch();
		ASSERT_EQ(last.bytes.size(), 1U);
		ASSERT_TRUE(pair.server.memRecv(last.bytes).error.ok());
		auto ack = pair.clientOutput.acknowledge(1);
		ASSERT_TRUE(ack.error.ok());
		ended = !ack.endedStreams.empty();
		EXPECT_EQ(ended, !pair.serverEvents.ended.empty());
	}
	EXPECT_TRUE(ended);
	bool continuation = false;
	for (size_t offset = 0; offset + 9 <= wire.size();)
	{
		const auto* frame = reinterpret_cast<const unsigned char*>(wire.data() + offset);
		const auto length = (static_cast<size_t>(frame[0]) << 16) | (static_cast<size_t>(frame[1]) << 8) | frame[2];
		ASSERT_LE(length, wire.size() - offset - 9);
		continuation = continuation || frame[3] == static_cast<uint8_t>(Codec::FrameType::hContinuation);
		offset += 9 + length;
	}
	EXPECT_TRUE(continuation);
}

TEST(Http2OutputTest, BothQueuedStreamsProgressBeforeEitherFinishes)
{
	Http2Options options;
	options.maxOutputBatchBytes = 4096;
	Pair pair(options);
	pair.exchange();
	auto first = pair.clientOutput.submitRequest(kRequest);
	auto second = pair.clientOutput.submitRequest(kRequest);
	ASSERT_TRUE(first.error.ok());
	ASSERT_TRUE(second.error.ok());
	ASSERT_TRUE(first.producer->append(std::string(100000, 'a')).ok());
	ASSERT_TRUE(second.producer->append(std::string(100000, 'b')).ok());
	ASSERT_TRUE(first.producer->finish().ok());
	ASSERT_TRUE(second.producer->finish().ok());
	for (size_t count = 0; count < 1024 && pair.serverEvents.ended.empty(); ++count)
	{
		ASSERT_TRUE(pair.pump(pair.clientOutput, pair.server));
		pair.pump(pair.serverOutput, pair.client);
		EXPECT_LE(pair.clientOutput.bufferedWireBytes(), options.maxOutputBatchBytes);
		EXPECT_LE(pair.clientBudget.usage().sendBytes, options.maxConnectionSendBytes);
	}
	ASSERT_FALSE(pair.serverEvents.ended.empty());
	EXPECT_FALSE(pair.serverEvents.body[first.streamId].empty());
	EXPECT_FALSE(pair.serverEvents.body[second.streamId].empty());
	pair.exchange();
	EXPECT_EQ(pair.serverEvents.body[first.streamId], std::string(100000, 'a'));
	EXPECT_EQ(pair.serverEvents.body[second.streamId], std::string(100000, 'b'));
}

TEST(Http2OutputTest, InvalidTrailersCanBeCorrectedAndBudgetFailureDoesNotMoveInput)
{
	Pair pair;
	pair.exchange();
	auto request = pair.clientOutput.submitRequest(kRequest);
	ASSERT_TRUE(request.error.ok());
	const auto used = pair.clientBudget.usage().sendBytes;
	std::string oversized(Http2Options::kDefaultConnectionBufferBytes, 'x');
	const auto original = oversized;
	EXPECT_EQ(request.producer->appendOwned(std::move(oversized)).code, ErrorCode::hResourceLimitExceeded);
	EXPECT_EQ(oversized, original);
	EXPECT_EQ(pair.clientBudget.usage().sendBytes, used);
	const std::array<Codec::Header, 1> invalid {{{"connection", "close"}}};
	EXPECT_FALSE(request.producer->finish(invalid).ok());
	EXPECT_EQ(pair.clientBudget.usage().sendBytes, used);
	ASSERT_TRUE(request.producer->append("valid after rejection").ok());
	ASSERT_TRUE(request.producer->finish().ok());
	EXPECT_FALSE(request.producer->append("late").ok());
	EXPECT_FALSE(request.producer->finish().ok());
	pair.exchange();
	EXPECT_EQ(pair.serverEvents.body[request.streamId], "valid after rejection");
}

TEST(Http2OutputTest, OutputDestructionCancelsSurvivingProducerAndReturnsEveryLease)
{
	Http2Options options;
	Codec client(Codec::Role::hClient, options);
	Http2Budget budget(Http2Budget::Role::hClient, options);
	std::shared_ptr<Http2Output::Producer> survivor;
	{
		Http2Output output(client, budget, options);
		auto request = output.submitRequest(kRequest);
		ASSERT_TRUE(request.error.ok());
		survivor = request.producer;
		ASSERT_TRUE(survivor->append("still queued").ok());
		EXPECT_GT(budget.usage().sendBytes, options.maxOutputBatchBytes);
	}
	EXPECT_EQ(budget.usage().sendBytes, 0U);
	EXPECT_EQ(survivor->queuedBytes(), 0U);
	EXPECT_EQ(survivor->append("late").code, ErrorCode::hOperationAborted);
	survivor.reset();
	EXPECT_EQ(budget.usage().sendBytes, 0U);
}

TEST(Http2OutputTest, CodecOwnsSourceUntilProtocolStreamCloses)
{
	struct Source final : Codec::DataSource
	{
		explicit Source(size_t& notifications) : closed(notifications)
		{
		}

		Codec::DataRead read(std::span<char>) override
		{
			return {0, true, false, {}};
		}

		void onClose(Http2Code) override
		{
			++closed;
		}

		size_t& closed;
	};

	size_t closed = 0;
	Pair pair;
	pair.exchange();
	auto source = std::make_shared<Source>(closed);
	std::weak_ptr<Source> weak = source;
	auto request = pair.client.submitRequest(kRequest, source);
	ASSERT_TRUE(request.error.ok());
	source.reset();
	pair.exchange();
	EXPECT_FALSE(weak.expired());
	EXPECT_EQ(closed, 0U);
	ASSERT_TRUE(pair.server.submitResponse(request.streamId, kResponse).ok());
	pair.exchange();
	EXPECT_TRUE(weak.expired());
	EXPECT_EQ(closed, 1U);
}

TEST(Http2OutputTest, ProducerReadinessAndCancellationWakeWriter)
{
	Pair pair;
	pair.exchange();
	size_t wakeups = 0;
	pair.clientOutput.setWakeup(
		[&wakeups]
		{
			++wakeups;
		});
	auto request = pair.clientOutput.submitRequest(kRequest);
	ASSERT_TRUE(request.error.ok());
	EXPECT_GT(wakeups, 0U);
	pair.exchange();
	auto previous = wakeups;
	ASSERT_TRUE(request.producer->append("resumed").ok());
	EXPECT_GT(wakeups, previous);
	previous = wakeups;
	ASSERT_TRUE(request.producer->finish().ok());
	EXPECT_GT(wakeups, previous);
	previous = wakeups;
	request.producer->cancel();
	EXPECT_GT(wakeups, previous);
	pair.clientOutput.setWakeup({});
}

TEST(Http2OutputTest, PendingOutputIncludesDeferredDataAndEndsAtLocalPhysicalEnd)
{
	Pair pair;
	EXPECT_TRUE(pair.clientOutput.hasPendingOutput());
	pair.exchange();
	EXPECT_FALSE(pair.clientOutput.hasPendingOutput());
	auto request = pair.clientOutput.submitRequest(kRequest);
	ASSERT_TRUE(request.error.ok());
	pair.exchange();
	EXPECT_FALSE(pair.client.wantWrite());
	EXPECT_TRUE(pair.clientOutput.hasPendingOutput());
	ASSERT_TRUE(request.producer->append("local half close").ok());
	ASSERT_TRUE(request.producer->finish().ok());
	pair.exchange();
	EXPECT_FALSE(pair.clientOutput.hasPendingOutput());
	EXPECT_TRUE(pair.clientEvents.ended.empty());
	EXPECT_EQ(pair.serverEvents.body[request.streamId], "local half close");
}

TEST(Http2OutputTest, PhysicalEndStaysPendingUntilAcknowledgementAndCodecRetirement)
{
	Pair pair;
	pair.exchange();
	auto request = pair.client.submitRequest(kRequest);
	ASSERT_TRUE(request.error.ok());
	pair.exchange();
	auto response = pair.serverOutput.submitResponse(request.streamId, kResponse);
	ASSERT_TRUE(response.error.ok());
	ASSERT_TRUE(response.producer->append("response").ok());
	ASSERT_TRUE(response.producer->finish().ok());
	for (size_t count = 0; count < 64; ++count)
	{
		auto batch = pair.serverOutput.nextBatch();
		ASSERT_TRUE(batch.error.ok());
		ASSERT_FALSE(batch.bytes.empty());
		ASSERT_TRUE(pair.serverOutput.hasPendingOutput());
		ASSERT_TRUE(pair.client.memRecv(batch.bytes).error.ok());
		auto ack = pair.serverOutput.acknowledge(batch.bytes.size());
		ASSERT_TRUE(ack.error.ok());
		if (!ack.endedStreams.empty())
		{
			auto retired = pair.serverOutput.nextBatch();
			ASSERT_TRUE(retired.error.ok());
			EXPECT_TRUE(retired.bytes.empty());
			EXPECT_FALSE(pair.serverOutput.hasPendingOutput());
			return;
		}
	}
	FAIL() << "响应未完成物理发送";
}

TEST(Http2OutputTest, HeaderOnlyResponsesEndInInitialHeadersEvenAtZeroDataWindow)
{
	Pair pair;
	pair.exchange();
	const std::array<Codec::Setting, 1> paused {{{Http2Setting::hInitialWindowSize, 0}}};
	ASSERT_TRUE(pair.client.submitSettings(paused).ok());
	pair.exchange();
	for (const auto status : {"200", "204", "304"})
	{
		const auto request = pair.client.submitRequest(kRequest);
		ASSERT_TRUE(request.error.ok());
		pair.exchange();
		const auto blockCount = pair.clientEvents.blocks.size();
		const auto frameCount = pair.clientEvents.frames.size();
		const std::array<Codec::Header, 1> responseHeaders {{{":status", status}}};
		auto response = pair.serverOutput.submitResponseHeaders(request.streamId, responseHeaders);
		ASSERT_TRUE(response.error.ok());
		EXPECT_EQ(response.producer, nullptr);
		EXPECT_TRUE(pair.serverOutput.hasPendingOutput());
		pair.exchange();
		ASSERT_EQ(pair.clientEvents.blocks.size(), blockCount + 1);
		EXPECT_TRUE(pair.clientEvents.blocks.back().endStream);
		EXPECT_TRUE(pair.clientEvents.body[request.streamId].empty());
		EXPECT_EQ(std::count(pair.written.begin(), pair.written.end(), request.streamId), 1);
		EXPECT_FALSE(pair.serverOutput.hasPendingOutput());
		for (size_t index = frameCount; index < pair.clientEvents.frames.size(); ++index)
		{
			EXPECT_NE(pair.clientEvents.frames[index].type, Codec::FrameType::hData);
		}
	}
}

TEST(Http2OutputTest, HeaderOnlyEndWaitsForLastContinuationAndAcknowledgement)
{
	Http2Options options;
	options.maxHeaderBytes = 64 * 1024;
	Pair pair(options);
	pair.exchange();
	const auto request = pair.client.submitRequest(kRequest);
	ASSERT_TRUE(request.error.ok());
	pair.exchange();
	std::string value;
	uint32_t random = 0x3198ab45;
	for (size_t count = 0; count < 40000; ++count)
	{
		random ^= random << 13;
		random ^= random >> 17;
		random ^= random << 5;
		value.push_back(static_cast<char>('!' + random % 94));
	}
	const std::array<Codec::Header, 2> fields {{{":status", "200"}, {"x-large", value}}};
	ASSERT_TRUE(pair.serverOutput.submitResponseHeaders(request.streamId, fields).error.ok());
	bool ended = false;
	std::string wire;
	for (size_t count = 0; count < 64 && !ended; ++count)
	{
		auto batch = pair.serverOutput.nextBatch();
		ASSERT_TRUE(batch.error.ok());
		ASSERT_FALSE(batch.bytes.empty());
		wire.append(batch.bytes);
		// 模拟协议库已关闭流，但 TLS 写入尚未确认完成。
		pair.serverOutput.onStreamClose(request.streamId, Http2Code::hNoError);
		EXPECT_TRUE(pair.serverOutput.hasPendingOutput());
		const auto prefix = batch.bytes.substr(0, batch.bytes.size() - 1);
		ASSERT_TRUE(pair.client.memRecv(prefix).error.ok());
		EXPECT_TRUE(pair.serverOutput.acknowledge(prefix.size()).endedStreams.empty());
		EXPECT_TRUE(pair.serverOutput.hasPendingOutput());
		auto last = pair.serverOutput.nextBatch();
		ASSERT_EQ(last.bytes.size(), 1U);
		ASSERT_TRUE(pair.client.memRecv(last.bytes).error.ok());
		auto acknowledged = pair.serverOutput.acknowledge(1);
		ASSERT_TRUE(acknowledged.error.ok());
		ended = !acknowledged.endedStreams.empty();
		EXPECT_EQ(ended, !pair.clientEvents.ended.empty());
	}
	EXPECT_TRUE(ended);
	const auto retired = pair.serverOutput.nextBatch();
	ASSERT_TRUE(retired.error.ok());
	EXPECT_TRUE(retired.bytes.empty());
	EXPECT_FALSE(pair.serverOutput.hasPendingOutput());
	bool continuation = false;
	for (size_t offset = 0; offset + 9 <= wire.size();)
	{
		const auto* frame = reinterpret_cast<const unsigned char*>(wire.data() + offset);
		const auto length = (static_cast<size_t>(frame[0]) << 16) | (static_cast<size_t>(frame[1]) << 8) | frame[2];
		ASSERT_LE(length, wire.size() - offset - 9);
		continuation = continuation || frame[3] == static_cast<uint8_t>(Codec::FrameType::hContinuation);
		offset += 9 + length;
	}
	EXPECT_TRUE(continuation);
}

TEST(Http2OutputTest, CancelledHeaderOnlyResponseNeverReportsPhysicalSuccess)
{
	for (const auto code : {Http2Code::hCancel, Http2Code::hNoError})
	{
		Pair pair;
		pair.exchange();
		const auto request = pair.client.submitRequest(kRequest);
		ASSERT_TRUE(request.error.ok());
		pair.exchange();
		ASSERT_TRUE(pair.serverOutput.submitResponseHeaders(request.streamId, kResponse).error.ok());
		auto batch = pair.serverOutput.nextBatch();
		ASSERT_TRUE(batch.error.ok());
		ASSERT_FALSE(batch.bytes.empty());
		if (code == Http2Code::hNoError)
		{
			pair.serverOutput.cancelStream(request.streamId);
		}
		else
		{
			pair.serverOutput.onStreamClose(request.streamId, code);
		}
		EXPECT_TRUE(pair.serverOutput.acknowledge(batch.bytes.size()).endedStreams.empty());
		const auto retired = pair.serverOutput.nextBatch();
		ASSERT_TRUE(retired.error.ok());
		EXPECT_TRUE(retired.bytes.empty());
		EXPECT_FALSE(pair.serverOutput.hasPendingOutput());
	}
}

TEST(Http2OutputTest, ProducerChunkCountIsBoundedAndCanProgressAfterRejection)
{
	Pair pair;
	pair.exchange();
	auto request = pair.clientOutput.submitRequest(kRequest);
	ASSERT_TRUE(request.error.ok());
	for (size_t count = 0; count < 64; ++count)
	{
		ASSERT_TRUE(request.producer->append("x").ok());
	}
	EXPECT_EQ(request.producer->append("overflow").code, ErrorCode::hResourceLimitExceeded);
	ASSERT_TRUE(request.producer->finish().ok());
	pair.exchange();
	EXPECT_EQ(pair.serverEvents.body[request.streamId], std::string(64, 'x'));
	EXPECT_EQ(pair.clientOutput.queuedBytes(), 0U);
}

TEST(Http2OutputTest, CollectedChunksHaveOneBoundedOwningQueueEntry)
{
	Pair pair;
	pair.exchange();
	auto request = pair.clientOutput.submitRequest(kRequest);
	ASSERT_TRUE(request.error.ok());
	std::string expected;
	{
		std::vector<std::string> chunks;
		for (size_t count = 0; count < 150; ++count)
		{
			chunks.emplace_back("a\0b", 3);
			chunks.emplace_back();
			expected.append("a\0b", 3);
		}
		ASSERT_TRUE(request.producer->appendChunks(chunks).ok());
		EXPECT_EQ(request.producer->queuedBytes(), expected.size());
		for (auto& chunk : chunks)
		{
			chunk.assign("overwritten");
		}
	}
	ASSERT_TRUE(request.producer->finish().ok());
	pair.exchange();
	EXPECT_EQ(pair.serverEvents.body[request.streamId], expected);
	EXPECT_EQ(pair.clientOutput.queuedBytes(), 0U);
}

TEST(Http2OutputTest, CollectedChunkBudgetFailureDoesNotRetainInputOrConsumeCapacity)
{
	Http2Options options;
	options.maxHeaderBytes = 512;
	options.maxConnectionSendBytes = 4096;
	options.maxOutputBatchBytes = 256;
	options.maxFilePrefetchBytes = 256;
	Pair pair(options);
	pair.exchange();
	auto request = pair.clientOutput.submitRequest(kRequest);
	ASSERT_TRUE(request.error.ok());
	const auto reserved = pair.clientBudget.usage().sendBytes;
	const auto available = options.maxConnectionSendBytes - reserved;
	const std::array<std::string, 2> rejected {{std::string(available, 'x'), "overflow"}};
	EXPECT_EQ(request.producer->appendChunks(rejected).code, ErrorCode::hResourceLimitExceeded);
	EXPECT_EQ(pair.clientBudget.usage().sendBytes, reserved);
	EXPECT_EQ(request.producer->queuedBytes(), 0U);
	EXPECT_EQ(rejected.front(), std::string(available, 'x'));
	const std::array<std::string, 2> accepted {{"still ", "works"}};
	ASSERT_TRUE(request.producer->appendChunks(accepted).ok());
	ASSERT_TRUE(request.producer->finish().ok());
	pair.exchange();
	EXPECT_EQ(pair.serverEvents.body[request.streamId], "still works");
	EXPECT_EQ(pair.clientOutput.queuedBytes(), 0U);
}

TEST(Http2OutputTest, ResumeFailureRestoresOwnedInputAndEveryQueueCharge)
{
	Pair pair;
	pair.exchange();
	auto request = pair.clientOutput.submitRequest(kRequest);
	ASSERT_TRUE(request.error.ok());
	pair.exchange();
	const auto used = pair.clientBudget.usage().sendBytes;
	pair.client.abortOutput();
	std::string body = "restore after failed resume";
	const auto original = body;
	EXPECT_EQ(request.producer->appendOwned(std::move(body)).code, ErrorCode::hOperationAborted);
	EXPECT_EQ(body, original);
	EXPECT_EQ(request.producer->queuedBytes(), 0U);
	EXPECT_EQ(pair.clientBudget.usage().sendBytes, used);
	EXPECT_EQ(request.producer->finish().code, ErrorCode::hOperationAborted);
}

TEST(Http2OutputTest, SourceCapacityIsReportedAsResourceLimit)
{
	Http2Options options;
	options.maxConcurrentStreams = 1;
	Pair pair(options);
	auto accepted = pair.clientOutput.submitRequest(kRequest);
	ASSERT_TRUE(accepted.error.ok());
	auto rejected = pair.clientOutput.submitRequest(kRequest);
	EXPECT_EQ(rejected.error.code, ErrorCode::hResourceLimitExceeded);
	EXPECT_FALSE(rejected.producer);
}

TEST(Http2OutputTest, QueuedRequestCannotDeferTheOnlyPermittedOpenStream)
{
	Pair pair;
	pair.exchange();
	const std::array<Codec::Setting, 1> limit {{{Http2Setting::hMaxConcurrentStreams, 1}}};
	ASSERT_TRUE(pair.server.submitSettings(limit).ok());
	pair.exchange();
	auto first = pair.clientOutput.submitRequest(kRequest);
	auto second = pair.clientOutput.submitRequest(kRequest);
	ASSERT_TRUE(first.error.ok());
	ASSERT_TRUE(second.error.ok());
	ASSERT_TRUE(first.producer->append(std::string(32000, 'a')).ok());
	ASSERT_TRUE(second.producer->append("second").ok());
	ASSERT_TRUE(first.producer->finish().ok());
	ASSERT_TRUE(second.producer->finish().ok());
	pair.exchange();
	EXPECT_EQ(pair.client.activeLocalStreamCount(), 1U);
	EXPECT_EQ(pair.serverEvents.body[first.streamId], std::string(32000, 'a'));
	EXPECT_TRUE(pair.serverEvents.body[second.streamId].empty());
	ASSERT_TRUE(pair.server.submitResponse(first.streamId, kResponse).ok());
	pair.exchange();
	EXPECT_EQ(pair.serverEvents.body[second.streamId], "second");
}

TEST(Http2OutputTest, RawRequestAlsoOccupiesPeerConcurrentStreamSlot)
{
	Pair pair;
	pair.exchange();
	const std::array<Codec::Setting, 1> limit {{{Http2Setting::hMaxConcurrentStreams, 2}}};
	ASSERT_TRUE(pair.server.submitSettings(limit).ok());
	pair.exchange();
	auto raw = pair.client.submitRequest(kRequest);
	ASSERT_TRUE(raw.error.ok());
	pair.exchange();
	auto first = pair.clientOutput.submitRequest(kRequest);
	auto second = pair.clientOutput.submitRequest(kRequest);
	ASSERT_TRUE(first.error.ok());
	ASSERT_TRUE(second.error.ok());
	ASSERT_TRUE(first.producer->append(std::string(32000, 'a')).ok());
	ASSERT_TRUE(second.producer->append("queued").ok());
	ASSERT_TRUE(first.producer->finish().ok());
	ASSERT_TRUE(second.producer->finish().ok());
	pair.exchange();
	EXPECT_EQ(pair.client.activeLocalStreamCount(), 2U);
	EXPECT_EQ(pair.serverEvents.body[first.streamId], std::string(32000, 'a'));
	ASSERT_TRUE(pair.server.submitResponse(raw.streamId, kResponse).ok());
	pair.exchange();
	EXPECT_EQ(pair.serverEvents.body[second.streamId], "queued");
}

TEST(Http2OutputTest, ZeroRequestEndDoesNotWaitForWindow)
{
	Http2Options options;
	options.initialStreamWindow = 0;
	Pair pair(options);
	pair.exchange();
	auto request = pair.clientOutput.submitRequest(kRequest);
	ASSERT_TRUE(request.error.ok());
	ASSERT_TRUE(request.producer->finish().ok());
	pair.exchange();
	ASSERT_EQ(pair.serverEvents.ended.size(), 1U);
	EXPECT_EQ(pair.serverEvents.ended.front(), request.streamId);
	EXPECT_FALSE(pair.clientOutput.hasPendingOutput());
}

TEST(Http2OutputTest, ZeroResponseEndDoesNotWaitForWindow)
{
	Http2Options options;
	options.initialStreamWindow = 0;
	Pair pair(options);
	pair.exchange();
	auto request = pair.client.submitRequest(kRequest);
	ASSERT_TRUE(request.error.ok());
	pair.exchange();
	auto response = pair.serverOutput.submitResponse(request.streamId, kResponse);
	ASSERT_TRUE(response.error.ok());
	ASSERT_TRUE(response.producer->finish().ok());
	pair.exchange();
	ASSERT_EQ(pair.clientEvents.ended.size(), 1U);
	EXPECT_EQ(pair.clientEvents.ended.front(), request.streamId);
	EXPECT_FALSE(pair.serverOutput.hasPendingOutput());
}

TEST(Http2OutputTest, LateFinishAfterExactWindowExhaustionStillEndsMessage)
{
	Http2Options options;
	options.initialStreamWindow = 4;
	Pair pair(options);
	pair.serverEvents.consume = false;
	pair.exchange();
	auto request = pair.clientOutput.submitRequest(kRequest);
	ASSERT_TRUE(request.error.ok());
	ASSERT_TRUE(request.producer->append("body").ok());
	pair.exchange();
	ASSERT_EQ(pair.serverEvents.body[request.streamId], "body");
	ASSERT_EQ(pair.client.peerWindow(request.streamId), 0);
	ASSERT_TRUE(request.producer->finish().ok());
	pair.exchange();
	EXPECT_EQ(pair.serverEvents.ended, std::vector<int32_t> {request.streamId});
	EXPECT_FALSE(pair.clientOutput.hasPendingOutput());
}

TEST(Http2OutputTest, OneByteBatchesStillCarryLegalLongHeaders)
{
	Http2Options options;
	options.maxOutputBatchBytes = 1;
	Pair pair(options);
	pair.exchange();
	std::string value;
	uint32_t random = 0x73895ab1;
	for (size_t count = 0; count < 512; ++count)
	{
		random ^= random << 13;
		random ^= random >> 17;
		random ^= random << 5;
		value.push_back(static_cast<char>('!' + random % 94));
	}
	const std::array<Codec::Header, 5> headers {
		{{":method", "POST"}, {":scheme", "https"}, {":authority", "localhost"}, {":path", "/"}, {"x-random", value}}};
	auto request = pair.clientOutput.submitRequest(headers);
	ASSERT_TRUE(request.error.ok());
	ASSERT_TRUE(request.producer->finish().ok());
	pair.exchange();
	EXPECT_EQ(pair.serverEvents.ended, std::vector<int32_t> {request.streamId});
	EXPECT_NE(std::find(pair.serverEvents.headers.begin(),
						pair.serverEvents.headers.end(),
						std::make_pair(std::string("x-random"), value)),
			  pair.serverEvents.headers.end());
}

TEST(Http2OutputTest, OutputRejectsConfigurationThatCannotReservePayloadProgress)
{
	Http2Options options;
	options.maxHeaderBytes = 512;
	options.maxConnectionSendBytes = 4096;
	options.maxOutputBatchBytes = 4096;
	options.maxFilePrefetchBytes = 4096;
	Codec client(Codec::Role::hClient, options);
	Http2Budget budget(Http2Budget::Role::hClient, options);
	EXPECT_THROW(Http2Output output(client, budget, options), std::invalid_argument);
	EXPECT_EQ(budget.usage().sendBytes, 0U);
}
