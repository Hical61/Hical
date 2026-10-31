/**
 * @file test_http2_limits.cpp
 * @brief HTTP/2 控制帧限制、头块超时和多流测试
 */

#include "core/http2/Http2Connection.h"
#include "core/http2/Http2Receive.h"
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/asio/write.hpp>
#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <future>
#include <optional>
#include <stdexcept>
#include <vector>

using namespace hical;
using namespace hical::detail;
using namespace boost::asio::experimental::awaitable_operators;

namespace
{
	using Codec = Http2Codec;
	using Clock = std::chrono::steady_clock;
	constexpr size_t kFrameHeaderBytes = 9;
	constexpr size_t kMaximumTestWireBytes = 64 * 1024;
	constexpr uint8_t kEndHeaders = 0x04;
	constexpr std::array<Codec::Header, 4> kRequest {
		{{":method", "GET"}, {":scheme", "https"}, {":authority", "localhost"}, {":path", "/"}}};
	constexpr std::array<Codec::Header, 1> kResponse {{{":status", "200"}}};

	struct Recorder : Codec::Listener
	{
		Http2Receive* receive {nullptr};
		std::vector<NetworkError> errors;
		size_t begun {0};
		size_t completed {0};
		size_t ended {0};
		size_t settings {0};
		size_t acknowledgements {0};
		size_t wireBegun {0};
		size_t wireCompleted {0};
		size_t pingAcks {0};
		bool rejectHeaders {false};

		Http2Code onBeginHeaders(int32_t id) override
		{
			++begun;
			if (rejectHeaders)
			{
				return Http2Code::hRefusedStream;
			}
			return receive ? receive->onBeginHeaders(id) : Http2Code::hNoError;
		}

		Http2Code onHeader(int32_t id, Codec::Header field) override
		{
			return receive ? receive->onHeader(id, field) : Http2Code::hNoError;
		}

		Http2Code onEndHeaders(const Codec::HeaderBlock& block) override
		{
			++completed;
			return receive ? receive->onEndHeaders(block) : Http2Code::hNoError;
		}

		Http2Code onData(int32_t id, std::string_view bytes) override
		{
			return receive ? receive->onData(id, bytes) : Http2Code::hNoError;
		}

		void onEndStream(int32_t id) override
		{
			++ended;
			if (receive)
			{
				receive->onEndStream(id);
			}
		}

		void onStreamClose(int32_t id, Http2Code code) override
		{
			if (receive)
			{
				receive->onStreamClose(id, code);
			}
		}

		void onSettings(bool ack) override
		{
			ack ? ++acknowledgements : ++settings;
		}

		void onHeaderBlock(int32_t, bool complete) override
		{
			complete ? ++wireCompleted : ++wireBegun;
		}

		void onFrameReceived(const Codec::Frame& frame) override
		{
			if (frame.type == Codec::FrameType::hPing && (frame.flags & Codec::kAck))
			{
				++pingAcks;
			}
		}

		void onError(const NetworkError& error) override
		{
			errors.push_back(error);
			if (receive)
			{
				receive->onError(error);
			}
		}
	};

	std::string frameHeader(Codec::FrameType type, uint8_t flags, int32_t streamId, size_t length)
	{
		std::string bytes(kFrameHeaderBytes, '\0');
		bytes[0] = static_cast<char>(length >> 16);
		bytes[1] = static_cast<char>(length >> 8);
		bytes[2] = static_cast<char>(length);
		bytes[3] = static_cast<char>(type);
		bytes[4] = static_cast<char>(flags);
		for (size_t index = 0; index < sizeof(streamId); ++index)
		{
			bytes[5 + index] = static_cast<char>(static_cast<uint32_t>(streamId) >> (24 - index * 8));
		}
		return bytes;
	}

	std::string frame(Codec::FrameType type, uint8_t flags, int32_t streamId, std::string_view payload = {})
	{
		return frameHeader(type, flags, streamId, payload.size()) + std::string(payload);
	}

	std::string drain(Codec& codec)
	{
		std::string bytes;
		for (size_t attempt = 0; attempt < 1024; ++attempt)
		{
			auto batch = codec.memSend();
			EXPECT_FALSE(batch.error) << batch.error.message;
			if (batch.error || batch.bytes.empty())
			{
				return bytes;
			}
			if (batch.bytes.size() > kMaximumTestWireBytes - bytes.size())
			{
				ADD_FAILURE() << "协议输出超过测试夹具的固定容量";
				return bytes;
			}
			bytes += batch.bytes;
		}
		ADD_FAILURE() << "协议输出未在有界次数内排空";
		return bytes;
	}

	void feed(Codec& codec, std::string_view bytes)
	{
		if (!bytes.empty())
		{
			const auto result = codec.memRecv(bytes);
			ASSERT_FALSE(result.error) << result.error.message;
			ASSERT_EQ(result.consumed, bytes.size());
		}
	}

	void exchange(Codec& client, Codec& server)
	{
		for (size_t attempt = 0; attempt < 32; ++attempt)
		{
			const auto fromClient = drain(client);
			feed(server, fromClient);
			const auto fromServer = drain(server);
			feed(client, fromServer);
			if (fromClient.empty() && fromServer.empty())
			{
				return;
			}
		}
		ADD_FAILURE() << "双端协议交换未在有界次数内收敛";
	}

	bool connectionError(const Recorder& events, Http2Code code)
	{
		return std::ranges::any_of(events.errors,
								   [code](const auto& error)
								   {
									   return error.code == ErrorCode::hHttp2ConnectionError && error.http2
											  && error.http2->streamId == 0
											  && error.http2->code == static_cast<uint32_t>(code);
								   });
	}

	void expectEmptyBudget(const Http2Budget& budget, const Http2ServiceBudget& service)
	{
		const auto usage = budget.usage();
		EXPECT_EQ(usage.protocolStreams, 0U);
		EXPECT_EQ(usage.applicationTasks, 0U);
		EXPECT_EQ(usage.receiveBytes, 0U);
		EXPECT_EQ(usage.sendBytes, 0U);
		EXPECT_EQ(service.used(), 0U);
	}

	struct TlsHarness
	{
		static boost::asio::ssl::context makeServerContext()
		{
			boost::asio::ssl::context context(boost::asio::ssl::context::tls_server);
			context.use_certificate_chain_file("test_server.crt");
			context.use_private_key_file("test_server.key", boost::asio::ssl::context::pem);
			return context;
		}

		boost::asio::io_context io;
		boost::asio::ssl::context serverContext {makeServerContext()};
		boost::asio::ssl::context clientContext {boost::asio::ssl::context::tls_client};
		boost::asio::ip::tcp::acceptor acceptor {io, {boost::asio::ip::address_v4::loopback(), 0}};
		Http2Connection::TlsSocket server {io, serverContext};
		Http2Connection::TlsSocket client {io, clientContext};
		boost::asio::steady_timer watchdog {io};
		bool timedOut {false};

		TlsHarness()
		{
			client.set_verify_mode(boost::asio::ssl::verify_none);
		}

		Awaitable<void> connect()
		{
			co_await (acceptor.async_accept(server.next_layer(), boost::asio::use_awaitable)
					  && client.next_layer().async_connect(acceptor.local_endpoint(), boost::asio::use_awaitable));
			co_await (server.async_handshake(boost::asio::ssl::stream_base::server, boost::asio::use_awaitable)
					  && client.async_handshake(boost::asio::ssl::stream_base::client, boost::asio::use_awaitable));
		}

		static Awaitable<void> execute(TlsHarness& harness, Awaitable<void> scenario)
		{
			try
			{
				co_await std::move(scenario);
			}
			catch (...)
			{
				boost::system::error_code ignored;
				harness.server.next_layer().close(ignored);
				harness.client.next_layer().close(ignored);
				harness.watchdog.cancel();
				throw;
			}
			harness.watchdog.cancel();
		}

		void run(Awaitable<void> scenario)
		{
			watchdog.expires_at(Clock::now() + std::chrono::seconds {3});
			watchdog.async_wait(
				[this](const boost::system::error_code& error)
				{
					if (!error)
					{
						timedOut = true;
						boost::system::error_code ignored;
						server.next_layer().close(ignored);
						client.next_layer().close(ignored);
						acceptor.close(ignored);
					}
				});
			auto result = boost::asio::co_spawn(io, execute(*this, std::move(scenario)), boost::asio::use_future);
			io.run();
			EXPECT_FALSE(timedOut) << "头块期限未在独立绝对 watchdog 前结束连接";
			EXPECT_NO_THROW(result.get());
		}
	};

	Awaitable<bool> writePeer(Http2Connection::TlsSocket& socket, const std::string& bytes)
	{
		boost::system::error_code error;
		co_await boost::asio::async_write(socket,
										  boost::asio::buffer(bytes),
										  boost::asio::redirect_error(boost::asio::use_awaitable, error));
		co_return !error;
	}

	Awaitable<void> negotiatePeer(TlsHarness& harness, Codec& peer, const Recorder& events)
	{
		if (!co_await writePeer(harness.client, drain(peer)))
		{
			ADD_FAILURE() << "无法发送客户端 preface/SETTINGS";
			co_return;
		}
		std::array<char, 1024> buffer;
		for (size_t attempt = 0; attempt < 8 && (events.settings == 0 || events.acknowledgements == 0); ++attempt)
		{
			boost::system::error_code error;
			const auto count =
				co_await harness.client.async_read_some(boost::asio::buffer(buffer),
														boost::asio::redirect_error(boost::asio::use_awaitable, error));
			if (error || count == 0)
			{
				ADD_FAILURE() << "对端 SETTINGS 交换提前结束";
				co_return;
			}
			feed(peer, {buffer.data(), count});
			if (!co_await writePeer(harness.client, drain(peer)))
			{
				ADD_FAILURE() << "无法确认服务端 SETTINGS";
				co_return;
			}
		}
		EXPECT_EQ(events.settings, 1U);
		EXPECT_EQ(events.acknowledgements, 1U);
	}

	class Http2LimitsTlsTest : public testing::Test
	{
	protected:
		void SetUp() override
		{
			if (!std::ifstream("test_server.crt").good() || !std::ifstream("test_server.key").good())
			{
				if (std::getenv("HICAL_REQUIRE_SSL_CERTS"))
				{
					FAIL() << "要求 SSL 证书，测试工作目录缺少 test_server.crt/key";
				}
				GTEST_SKIP() << "请先在测试工作目录生成 TLS 证书";
			}
		}

		void checkHeaderDeadline(bool drip, uint8_t headerFlags = 0, bool rejectHeaders = false)
		{
			TlsHarness harness;
			Http2Options options;
			options.headerTimeout = std::chrono::milliseconds {200};
			options.negotiationTimeout = std::chrono::seconds {2};
			Http2ServiceBudget service(options.maxServerApplicationTasks);
			Http2Budget budget(Http2Budget::Role::hServer, options, &service);
			Recorder events;
			events.rejectHeaders = rejectHeaders;
			std::unique_ptr<Http2Receive> receive;
			Http2Connection::Callbacks callbacks;
			callbacks.onFailure = [&](const NetworkError& error)
			{
				receive->fail(error);
			};
			auto connection = Http2Connection::create(harness.server, Codec::Role::hServer, options, budget, callbacks);
			receive = std::make_unique<Http2Receive>(harness.io.get_executor(),
													 connection->codec(),
													 budget,
													 Http2Receive::Config {Codec::Role::hServer, options});
			events.receive = receive.get();
			EXPECT_FALSE(connection->setListener(&events));
			Recorder peerEvents;
			Codec peer(Codec::Role::hClient, {}, &peerEvents);
			boost::asio::steady_timer cadence {harness.io};
			std::optional<NetworkError> result;
			size_t payloadBytesSent = 0;
			bool joined = false;
			auto scenario = [&]() -> Awaitable<void>
			{
				co_await harness.connect();
				boost::asio::co_spawn(harness.io,
									  connection->run(),
									  [&](std::exception_ptr exception, NetworkError error)
									  {
										  EXPECT_FALSE(exception);
										  result = std::move(error);
										  joined = true;
										  cadence.cancel();
									  });
				co_await negotiatePeer(harness, peer, peerEvents);
				EXPECT_EQ(events.settings, 1U);
				// 完成后也是合法请求：四个伪头后追加 x: <1024 个 a>，此时只发值长度。
				std::string prefix {"\x82\x87\x84\x01\x09", 5};
				prefix += "localhost";
				prefix.append("\x00\x01x\x7f\x81\x07", 6);
				if (headerFlags != 0)
				{
					// 只发完整帧头；PAD 长度字节或五字节 priority 尚未到达。
					EXPECT_TRUE(co_await writePeer(
						harness.client,
						frameHeader(Codec::FrameType::hHeaders, headerFlags | Codec::kEndStream, 1, 64)));
				}
				else
				{
					EXPECT_TRUE(co_await writePeer(harness.client,
												   frame(Codec::FrameType::hHeaders, Codec::kEndStream, 1, prefix)));
				}
				if (drip)
				{
					EXPECT_TRUE(co_await writePeer(harness.client,
												   frameHeader(Codec::FrameType::hContinuation, kEndHeaders, 1, 1024)));
					if (co_await writePeer(harness.client, std::string(1, 'a')))
					{
						++payloadBytesSent;
					}
				}
				auto nextByte = Clock::now();
				while (!joined)
				{
					// 定时器控制发送节奏，run 结束后会唤醒等待，不靠固定延时判断结果。
					nextByte += std::chrono::milliseconds {25};
					cadence.expires_at(nextByte);
					boost::system::error_code ignored;
					co_await cadence.async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, ignored));
					if (!joined && drip && payloadBytesSent < 1024)
					{
						if (co_await writePeer(harness.client, std::string(1, 'a')))
						{
							++payloadBytesSent;
						}
					}
				}
			};
			harness.run(scenario());
			ASSERT_TRUE(joined);
			ASSERT_TRUE(result);
			EXPECT_EQ(result->code, ErrorCode::hTimedOut);
			EXPECT_FALSE(connection->running());
			EXPECT_EQ(events.begun, headerFlags == 0 ? 1U : 0U);
			EXPECT_EQ(events.completed, 0U);
			EXPECT_EQ(events.wireBegun, 1U);
			EXPECT_EQ(events.wireCompleted, 0U);
			if (drip)
			{
				EXPECT_GT(payloadBytesSent, 0U);
			}
			EXPECT_EQ(receive->size(), 0U);
			// wire 租约覆盖在途 TLS 写入，等 run 结束并销毁连接后配额才归零。
			events.receive = nullptr;
			receive.reset();
			connection.reset();
			harness.io.restart();
			harness.io.run();
			expectEmptyBudget(budget, service);
		}

		void checkCompleteHeaderCancellation()
		{
			TlsHarness harness;
			Http2Options options;
			options.headerTimeout = std::chrono::milliseconds {40};
			Http2ServiceBudget service(options.maxServerApplicationTasks);
			Http2Budget budget(Http2Budget::Role::hServer, options, &service);
			Recorder events;
			std::unique_ptr<Http2Receive> receive;
			Http2Connection::Callbacks callbacks;
			callbacks.onFailure = [&](const NetworkError& error)
			{
				receive->fail(error);
			};
			auto connection = Http2Connection::create(harness.server, Codec::Role::hServer, options, budget, callbacks);
			receive = std::make_unique<Http2Receive>(harness.io.get_executor(),
													 connection->codec(),
													 budget,
													 Http2Receive::Config {Codec::Role::hServer, options});
			events.receive = receive.get();
			EXPECT_FALSE(connection->setListener(&events));
			Recorder peerEvents;
			Codec peer(Codec::Role::hClient, {}, &peerEvents);
			boost::asio::steady_timer probe {harness.io};
			bool joined = false;
			auto scenario = [&]() -> Awaitable<void>
			{
				co_await harness.connect();
				boost::asio::co_spawn(harness.io,
									  connection->run(),
									  [&](std::exception_ptr exception, NetworkError)
									  {
										  EXPECT_FALSE(exception);
										  joined = true;
										  probe.cancel();
									  });
				co_await negotiatePeer(harness, peer, peerEvents);
				EXPECT_FALSE(peer.submitRequest(kRequest).error);
				EXPECT_TRUE(co_await writePeer(harness.client, drain(peer)));
				// 等待超过头块期限，检查头块完整接收后，已取消的定时器不会再触发。
				probe.expires_at(Clock::now() + options.headerTimeout * 3);
				boost::system::error_code ignored;
				co_await probe.async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, ignored));
				EXPECT_TRUE(connection->running());
				EXPECT_EQ(events.completed, 1U);
				if (connection->running())
				{
					EXPECT_FALSE(peer.submitRequest(kRequest).error);
					EXPECT_TRUE(co_await writePeer(harness.client, drain(peer)));
					EXPECT_TRUE(
						co_await writePeer(harness.client, frame(Codec::FrameType::hPing, 0, 0, std::string(8, 'p'))));
					std::array<char, 1024> buffer;
					for (size_t attempt = 0; attempt < 8 && peerEvents.pingAcks == 0; ++attempt)
					{
						const auto count = co_await harness.client.async_read_some(
							boost::asio::buffer(buffer),
							boost::asio::redirect_error(boost::asio::use_awaitable, ignored));
						if (ignored || count == 0)
						{
							break;
						}
						feed(peer, {buffer.data(), count});
					}
					EXPECT_EQ(peerEvents.pingAcks, 1U);
					EXPECT_EQ(events.completed, 2U);
				}
				connection->close();
				while (!joined)
				{
					probe.expires_at(Clock::time_point::max());
					co_await probe.async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, ignored));
				}
			};
			harness.run(scenario());
			EXPECT_TRUE(joined);
			EXPECT_EQ(events.wireBegun, 2U);
			EXPECT_EQ(events.wireCompleted, 2U);
			events.receive = nullptr;
			receive.reset();
			connection.reset();
			harness.io.restart();
			harness.io.run();
			expectEmptyBudget(budget, service);
		}
	};
} // namespace

TEST(Http2LimitsTest, OversizedContinuationIsRejectedFromItsHeaderForActiveAndIgnoredBlocks)
{
	for (bool rejected : {false, true})
	{
		for (const size_t fragment : {size_t {1}, kFrameHeaderBytes})
		{
			SCOPED_TRACE(rejected ? "已拒绝头块" : "接纳中的头块");
			SCOPED_TRACE(fragment);
			Recorder events;
			events.rejectHeaders = rejected;
			Codec client(Codec::Role::hClient);
			Codec server(Codec::Role::hServer, {}, &events);
			exchange(client, server);
			feed(server, frame(Codec::FrameType::hHeaders, 0, 1));
			ASSERT_EQ(events.begun, 1U);
			// 只发九字节帧头，帧长错误必须在读取攻击者声明的 payload 前生效。
			const auto malformed = frameHeader(Codec::FrameType::hContinuation, kEndHeaders, 1, 16385);
			for (size_t offset = 0; offset < malformed.size(); offset += fragment)
			{
				feed(server, std::string_view(malformed).substr(offset, fragment));
			}
			drain(server);
			EXPECT_TRUE(connectionError(events, Http2Code::hFrameSizeError));
			EXPECT_EQ(events.completed, 0U);
		}
	}
}

TEST(Http2LimitsTest, NinthContinuationTerminatesBothActiveAndIgnoredHeaderBlocks)
{
	for (bool rejected : {false, true})
	{
		SCOPED_TRACE(rejected ? "已拒绝头块" : "接纳中的头块");
		Recorder events;
		events.rejectHeaders = rejected;
		Codec client(Codec::Role::hClient);
		Codec server(Codec::Role::hServer, {}, &events);
		exchange(client, server);
		feed(server, frame(Codec::FrameType::hHeaders, 0, 1));
		const auto continuation = frame(Codec::FrameType::hContinuation, 0, 1);
		for (size_t index = 0; index < 8; ++index)
		{
			feed(server, continuation);
		}
		const auto result = server.memRecv(continuation);
		EXPECT_EQ(result.error.code, ErrorCode::hHttp2ConnectionError);
		EXPECT_EQ(events.completed, 0U);
		EXPECT_EQ(server.memRecv(continuation).error.code, ErrorCode::hHttp2ConnectionError);
	}
}

TEST(Http2LimitsTest, NegotiatedLargerFramesAllowContinuationsAboveTheDefaultFrameSize)
{
	Http2Options options;
	options.maxFrameBytes = 32768;
	options.maxHeaderBytes = 32768;
	Recorder events;
	Codec client(Codec::Role::hClient);
	Codec server(Codec::Role::hServer, options, &events);
	exchange(client, server);
	std::string prefix {"\x82\x87\x84\x01\x09", 5};
	prefix += "localhost";
	// 无索引 literal x 的原始值声明为 16385；完整块仍在已协商的头预算内。
	prefix.append("\x00\x01x\x7f\x82\x7f", 6);
	feed(server, frame(Codec::FrameType::hHeaders, Codec::kEndStream, 1, prefix));
	feed(server, frame(Codec::FrameType::hContinuation, kEndHeaders, 1, std::string(16385, 'a')));
	EXPECT_EQ(events.completed, 1U);
	EXPECT_EQ(events.wireBegun, 1U);
	EXPECT_EQ(events.wireCompleted, 1U);
	EXPECT_TRUE(events.errors.empty());
}

TEST(Http2LimitsTest, RawHeaderBlockCallbackExceptionsStayInsideTheCodecPump)
{
	struct Throwing : Codec::Listener
	{
		void onHeaderBlock(int32_t, bool) override
		{
			throw std::runtime_error("测试原始头块回调异常");
		}
	} events;

	Codec client(Codec::Role::hClient);
	Codec server(Codec::Role::hServer, {}, &events);
	exchange(client, server);
	Codec::ReceiveResult result;
	EXPECT_NO_THROW(result = server.memRecv(frame(Codec::FrameType::hHeaders, 0, 1)));
	EXPECT_EQ(result.error.code, ErrorCode::hHttp2ConnectionError);
	ASSERT_TRUE(result.error.http2);
	EXPECT_EQ(result.error.http2->code, static_cast<uint32_t>(Http2Code::hInternalError));
}

TEST(Http2LimitsTest, SettingsAcceptsThirtyTwoEntriesButRejectsThirtyThreeBeforePayload)
{
	Recorder events;
	Codec client(Codec::Role::hClient);
	Codec server(Codec::Role::hServer, {}, &events);
	exchange(client, server);
	// 未识别的设置标识符必须忽略；这里的 32 项输入在协议上合法。
	std::string entries;
	for (size_t index = 0; index < 32; ++index)
	{
		entries.append("\x01", 1);
		entries += static_cast<char>(index);
		entries.append(4, '\0');
	}
	feed(server, frame(Codec::FrameType::hSettings, 0, 0, entries));
	EXPECT_EQ(events.settings, 2U);
	drain(server);
	const auto result = server.memRecv(frameHeader(Codec::FrameType::hSettings, 0, 0, 33 * 6));
	EXPECT_FALSE(result.error);
	drain(server);
	EXPECT_TRUE(connectionError(events, Http2Code::hEnhanceYourCalm));
}

TEST(Http2LimitsTest, UnflushedPingAndSettingsAcksShareOneFiniteQueue)
{
	Codec client(Codec::Role::hClient);
	Codec server(Codec::Role::hServer);
	exchange(client, server);
	const auto ping = frame(Codec::FrameType::hPing, 0, 0, std::string(8, 'p'));
	const auto settings = frame(Codec::FrameType::hSettings, 0, 0);
	// 不调用 memSend，确保是库内未排出的 ACK 存量，而非累计合法请求数。
	for (size_t index = 0; index < 1000; ++index)
	{
		feed(server, index % 2 == 0 ? ping : settings);
	}
	EXPECT_EQ(server.memRecv(ping).error.code, ErrorCode::hHttp2ConnectionError);
}

TEST(Http2LimitsTest, LegalConcurrentStreamsFinishAndReturnAllConnectionAndServiceQuotas)
{
	boost::asio::io_context io;
	Http2Options options;
	options.maxConcurrentStreams = 2;
	options.maxApplicationTasks = 2;
	options.maxServerApplicationTasks = 2;
	Http2ServiceBudget service(options.maxServerApplicationTasks);
	Http2Budget budget(Http2Budget::Role::hServer, options, &service);
	Recorder clientEvents;
	Recorder serverEvents;
	Codec client(Codec::Role::hClient, {}, &clientEvents);
	Codec server(Codec::Role::hServer, options, &serverEvents);
	Http2Receive receive(io.get_executor(), server, budget, {Codec::Role::hServer, options});
	serverEvents.receive = &receive;
	exchange(client, server);
	const auto first = client.submitRequest(kRequest);
	const auto second = client.submitRequest(kRequest);
	ASSERT_FALSE(first.error);
	ASSERT_FALSE(second.error);
	exchange(client, server);
	EXPECT_EQ(serverEvents.completed, 2U);
	ASSERT_EQ(receive.size(), 2U);
	EXPECT_EQ(budget.usage().protocolStreams, 2U);
	EXPECT_EQ(budget.usage().applicationTasks, 2U);
	EXPECT_EQ(service.used(), 2U);
	EXPECT_FALSE(server.submitResponse(first.streamId, kResponse));
	EXPECT_FALSE(server.submitResponse(second.streamId, kResponse));
	exchange(client, server);
	EXPECT_EQ(clientEvents.completed, 2U);
	EXPECT_EQ(clientEvents.ended, 2U);
	EXPECT_EQ(receive.size(), 0U);
	expectEmptyBudget(budget, service);
}

TEST_F(Http2LimitsTlsTest, IncompleteHeadersExpireAfterSettingsNegotiation)
{
	checkHeaderDeadline(false);
}

TEST_F(Http2LimitsTlsTest, SlowHpackBytesDoNotExtendTheHeaderBlockDeadline)
{
	checkHeaderDeadline(true);
}

TEST_F(Http2LimitsTlsTest, PaddedAndPriorityHeaderDeadlinesStartBeforeTheirExtraFieldsArrive)
{
	for (const uint8_t flags : {uint8_t {0x08}, uint8_t {0x20}})
	{
		SCOPED_TRACE(static_cast<int>(flags));
		checkHeaderDeadline(false, flags);
	}
}

TEST_F(Http2LimitsTlsTest, IgnoredHeaderBlocksStillHaveAnAbsoluteDeadline)
{
	checkHeaderDeadline(true, 0, true);
}

TEST_F(Http2LimitsTlsTest, CompleteHeadersCancelTheDeadlineAndAllowLaterStreamsAndPings)
{
	checkCompleteHeaderCancellation();
}
