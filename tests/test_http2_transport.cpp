/**
 * @file test_http2_transport.cpp
 * @brief HTTP/2 TLS 收发、故障处理和子任务清理测试
 */

#include "core/http2/Http2Connection.h"
#include "core/http2/Http2Receive.h"
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/asio/write.hpp>
#include <gtest/gtest.h>
#include <array>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <future>
#include <optional>
#include <vector>

using namespace hical;
using namespace hical::detail;
using namespace boost::asio::experimental::awaitable_operators;

namespace
{
	using Codec = Http2Codec;
	using Clock = std::chrono::steady_clock;
	constexpr auto kTestTimeout = std::chrono::seconds {10};
	constexpr std::array<Codec::Header, 4> kRequest {
		{{":method", "GET"}, {":scheme", "https"}, {":authority", "localhost"}, {":path", "/"}}};
	constexpr std::array<Codec::Header, 1> kResponse {{{":status", "200"}}};

	std::string drainCodec(Codec& codec)
	{
		std::string bytes;
		for (size_t attempts = 0; attempts < 1024; ++attempts)
		{
			auto batch = codec.memSend();
			EXPECT_FALSE(batch.error);
			if (batch.bytes.empty() || batch.error)
			{
				return bytes;
			}
			bytes += batch.bytes;
		}
		ADD_FAILURE() << "内存协议输出未收敛";
		return bytes;
	}

	struct RawPeer : Codec::Listener
	{
		Codec codec {Codec::Role::hClient, {}, this};
		bool headers {false};
		bool ended {false};
		bool goaway {false};
		Http2Code goawayCode {Http2Code::hNoError};
		std::string body;

		explicit RawPeer(const Http2Options& options = {}) : codec(Codec::Role::hClient, options, this)
		{
		}

		Http2Code onEndHeaders(const Codec::HeaderBlock& block) override
		{
			headers = block.kind == Codec::HeaderKind::hResponse && block.statusCode >= 200;
			return Http2Code::hNoError;
		}

		Http2Code onData(int32_t id, std::string_view bytes) override
		{
			body.append(bytes);
			EXPECT_FALSE(codec.consume(id, bytes.size()));
			return Http2Code::hNoError;
		}

		void onEndStream(int32_t) override
		{
			ended = true;
		}

		void onGoaway(int32_t, Http2Code code, std::string_view) override
		{
			goaway = true;
			goawayCode = code;
		}
	};

	Awaitable<bool> readPeer(Http2Connection::TlsSocket& socket, RawPeer& peer)
	{
		std::array<char, 16384> buffer;
		boost::system::error_code error;
		const auto count =
			co_await socket.async_read_some(boost::asio::buffer(buffer),
											boost::asio::redirect_error(boost::asio::use_awaitable, error));
		if (count != 0)
		{
			const auto result = peer.codec.memRecv({buffer.data(), count});
			EXPECT_FALSE(result.error);
			EXPECT_EQ(result.consumed, count);
		}
		co_return !error && count != 0;
	}

	Awaitable<bool> writePeer(Http2Connection::TlsSocket& socket, const std::string& bytes)
	{
		boost::system::error_code error;
		co_await boost::asio::async_write(socket,
										  boost::asio::buffer(bytes),
										  boost::asio::redirect_error(boost::asio::use_awaitable, error));
		co_return !error;
	}

	struct Harness
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

		Harness()
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

		static Awaitable<void> execute(Harness& harness, Awaitable<void> scenario)
		{
			try
			{
				co_await std::move(scenario);
			}
			catch (...)
			{
				harness.watchdog.cancel();
				throw;
			}
			harness.watchdog.cancel();
		}

		void run(Awaitable<void> scenario)
		{
			watchdog.expires_at(Clock::now() + kTestTimeout);
			watchdog.async_wait(
				[this](const boost::system::error_code& error)
				{
					if (!error)
					{
						timedOut = true;
						boost::system::error_code ignored;
						server.next_layer().close(ignored);
						client.next_layer().close(ignored);
					}
				});
			auto result = boost::asio::co_spawn(io, execute(*this, std::move(scenario)), boost::asio::use_future);
			io.run();
			EXPECT_FALSE(timedOut) << "TLS 收发测试未在绝对截止点前收尾";
			EXPECT_NO_THROW(result.get());
		}
	};

	class Http2TransportTest : public testing::Test
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
	};

	struct Endpoint : Codec::Listener
	{
		boost::asio::any_io_executor executor;
		std::shared_ptr<Http2Connection> connection;
		std::unique_ptr<Http2Receive> receive;
		std::vector<std::shared_ptr<Http2Stream>> streams;
		std::vector<int32_t> written;
		size_t failures {0};
		size_t activity {0};
		std::function<void(std::shared_ptr<Http2Stream>)> headersReady;
		std::function<void(int32_t)> ended;
		std::function<void()> wrote;

		Endpoint(Http2Connection::TlsSocket& socket, Codec::Role role, const Http2Options& options, Http2Budget budget)
			: executor(socket.get_executor())
		{
			Http2Connection::Callbacks callbacks;
			callbacks.onActivity = [this]
			{
				++activity;
			};
			callbacks.onWritten = [this](int32_t id)
			{
				written.push_back(id);
				if (wrote)
				{
					wrote();
				}
			};
			callbacks.onFailure = [this](const NetworkError& error)
			{
				++failures;
				receive->fail(error);
			};
			connection = Http2Connection::create(socket, role, options, budget, std::move(callbacks));
			Http2Receive::Callbacks inputCallbacks;
			inputCallbacks.onHeadersReady = [this](std::shared_ptr<Http2Stream> stream)
			{
				streams.push_back(stream);
				if (headersReady)
				{
					headersReady(std::move(stream));
				}
			};
			receive = std::make_unique<Http2Receive>(socket.get_executor(),
													 connection->codec(),
													 budget,
													 Http2Receive::Config {role, options},
													 std::move(inputCallbacks));
			EXPECT_FALSE(connection->setListener(this));
		}

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
			if (ended)
			{
				ended(id);
			}
		}

		void onStreamClose(int32_t id, Http2Code code) override
		{
			receive->onStreamClose(id, code);
		}

		void onError(const NetworkError& error) override
		{
			receive->onError(error);
		}

		std::shared_ptr<Http2Output::Producer> request(const Http2Options& options, Http2Budget budget)
		{
			auto opened = connection->output().submitRequest(kRequest);
			EXPECT_FALSE(opened.error);
			Http2Stream::Config config;
			config.streamId = opened.streamId;
			config.incomingKind = Codec::HeaderKind::hResponse;
			config.options = options;
			auto stream = Http2Stream::create(executor, config, budget);
			EXPECT_TRUE(stream);
			EXPECT_FALSE(receive->addClientStream(std::move(stream), {}));
			return opened.producer;
		}
	};
} // namespace

TEST_F(Http2TransportTest, RealTlsMultiplexesBodiesAndJoinsBothPumps)
{
	Harness harness;
	const Http2Options options;
	Http2ServiceBudget service(options.maxServerApplicationTasks);
	Http2Budget serverBudget(Http2Budget::Role::hServer, options, &service);
	Http2Budget clientBudget(Http2Budget::Role::hClient, options);
	Endpoint server(harness.server, Codec::Role::hServer, options, serverBudget);
	Endpoint client(harness.client, Codec::Role::hClient, options, clientBudget);
	server.headersReady = [&](std::shared_ptr<Http2Stream> stream)
	{
		auto response = server.connection->output().submitResponse(stream->id(), kResponse);
		EXPECT_FALSE(response.error);
		EXPECT_FALSE(response.producer->append("stream-" + std::to_string(stream->id())));
		EXPECT_FALSE(response.producer->finish());
	};
	const auto finishIfComplete = [&]
	{
		if (server.written.size() == 2 && client.streams.size() == 2 && client.streams[0]->bodyComplete()
			&& client.streams[1]->bodyComplete())
		{
			client.connection->finish();
		}
	};
	client.ended = [&](int32_t)
	{
		finishIfComplete();
	};
	server.wrote = finishIfComplete;
	auto scenario = [&]() -> Awaitable<void>
	{
		co_await harness.connect();
		EXPECT_FALSE(client.request(options, clientBudget)->finish());
		EXPECT_FALSE(client.request(options, clientBudget)->finish());
		co_await (server.connection->run() && client.connection->run());
	};
	harness.run(scenario());
	ASSERT_EQ(client.streams.size(), 2U);
	EXPECT_EQ(client.streams[0]->body(), "stream-1");
	EXPECT_EQ(client.streams[1]->body(), "stream-3");
	EXPECT_EQ(server.written.size(), 2U);
	EXPECT_FALSE(server.connection->running());
	EXPECT_FALSE(client.connection->running());
	EXPECT_GT(server.activity, 0U);
	EXPECT_GT(client.activity, 0U);
}

TEST_F(Http2TransportTest, NegotiationTimeoutClosesAReadAndAnIdleWriter)
{
	Harness harness;
	Http2Options options;
	options.negotiationTimeout = std::chrono::milliseconds {25};
	Http2ServiceBudget service(options.maxServerApplicationTasks);
	Http2Budget budget(Http2Budget::Role::hServer, options, &service);
	Endpoint server(harness.server, Codec::Role::hServer, options, budget);
	auto scenario = [&]() -> Awaitable<void>
	{
		co_await harness.connect();
		const auto result = co_await server.connection->run();
		EXPECT_EQ(result.code, ErrorCode::hTimedOut);
		EXPECT_FALSE(server.connection->running());
	};
	harness.run(scenario());
	EXPECT_EQ(server.failures, 1U);
}

TEST_F(Http2TransportTest, RepeatedCloseAndLateRunDoNotStartBackgroundIo)
{
	Harness harness;
	const Http2Options options;
	Http2Budget budget(Http2Budget::Role::hClient, options);
	Endpoint client(harness.client, Codec::Role::hClient, options, budget);
	client.connection->close({ErrorCode::hConnectionReset, "测试关闭"});
	client.connection->close({ErrorCode::hTimedOut, "迟到关闭"});
	auto scenario = [&]() -> Awaitable<void>
	{
		EXPECT_EQ((co_await client.connection->run()).code, ErrorCode::hConnectionReset);
		EXPECT_EQ((co_await client.connection->run()).code, ErrorCode::hOperationInProgress);
		EXPECT_EQ(client.connection->setListener(nullptr).code, ErrorCode::hOperationInProgress);
	};
	harness.run(scenario());
	EXPECT_EQ(client.failures, 1U);
	EXPECT_EQ(client.activity, 0U);
}

TEST_F(Http2TransportTest, CallerCancellationStillJoinsTheWriterBeforeReturning)
{
	Harness harness;
	const Http2Options options;
	Http2ServiceBudget service(options.maxServerApplicationTasks);
	Http2Budget budget(Http2Budget::Role::hServer, options, &service);
	Endpoint server(harness.server, Codec::Role::hServer, options, budget);
	boost::asio::cancellation_signal cancellation;
	std::optional<NetworkError> result;
	bool completed {false};
	auto scenario = [&]() -> Awaitable<void>
	{
		co_await harness.connect();
		boost::asio::co_spawn(harness.io,
							  server.connection->run(),
							  boost::asio::bind_cancellation_slot(cancellation.slot(),
																  [&](std::exception_ptr exception, NetworkError error)
																  {
																	  EXPECT_FALSE(exception);
																	  result = std::move(error);
																	  completed = true;
																  }));
		while (!server.connection->running())
		{
			co_await boost::asio::post(boost::asio::use_awaitable);
		}
		cancellation.emit(boost::asio::cancellation_type::terminal);
		while (!completed)
		{
			co_await boost::asio::post(boost::asio::use_awaitable);
		}
		EXPECT_FALSE(server.connection->running());
	};
	harness.run(scenario());
	ASSERT_TRUE(result);
	EXPECT_EQ(result->code, ErrorCode::hOperationAborted);
	EXPECT_EQ(server.failures, 1U);
}

TEST_F(Http2TransportTest, ProtocolFailureSendsGoawayAndReturnsTheOriginalConnectionError)
{
	Harness harness;
	const Http2Options options;
	Http2ServiceBudget service(options.maxServerApplicationTasks);
	Http2Budget budget(Http2Budget::Role::hServer, options, &service);
	Endpoint server(harness.server, Codec::Role::hServer, options, budget);
	RawPeer peer;
	NetworkError result;
	auto peerRun = [&]() -> Awaitable<void>
	{
		auto bytes = drainCodec(peer.codec);
		const std::array<char, 10> invalidPing {{0, 0, 1, 6, 0, 0, 0, 0, 0, 'x'}};
		bytes.append(invalidPing.data(), invalidPing.size());
		EXPECT_TRUE(co_await writePeer(harness.client, bytes));
		while (!peer.goaway && (co_await readPeer(harness.client, peer)))
		{
		}
	};
	auto serverRun = [&]() -> Awaitable<void>
	{
		result = co_await server.connection->run();
	};
	auto scenario = [&]() -> Awaitable<void>
	{
		co_await harness.connect();
		co_await (serverRun() && peerRun());
	};
	harness.run(scenario());
	EXPECT_TRUE(peer.goaway);
	EXPECT_EQ(peer.goawayCode, Http2Code::hFrameSizeError);
	EXPECT_EQ(result.code, ErrorCode::hHttp2ConnectionError);
	ASSERT_TRUE(result.http2);
	EXPECT_EQ(result.http2->streamId, 0);
	EXPECT_EQ(result.http2->code, static_cast<uint32_t>(Http2Code::hFrameSizeError));
	EXPECT_EQ(server.failures, 1U);
}

TEST_F(Http2TransportTest, FinishWaitsForAZeroWindowToReopenAndForPhysicalEnd)
{
	Harness harness;
	const Http2Options options;
	Http2ServiceBudget service(options.maxServerApplicationTasks);
	Http2Budget budget(Http2Budget::Role::hServer, options, &service);
	Endpoint server(harness.server, Codec::Role::hServer, options, budget);
	Http2Options peerOptions;
	peerOptions.initialStreamWindow = 0;
	RawPeer peer(peerOptions);
	const std::string payload(4096, 'w');
	server.headersReady = [&](std::shared_ptr<Http2Stream> stream)
	{
		auto response = server.connection->output().submitResponse(stream->id(), kResponse);
		EXPECT_FALSE(response.error);
		EXPECT_FALSE(response.producer->append(payload));
		EXPECT_FALSE(response.producer->finish());
		server.connection->finish();
	};
	auto peerRun = [&]() -> Awaitable<void>
	{
		EXPECT_FALSE(peer.codec.submitRequest(kRequest).error);
		EXPECT_TRUE(co_await writePeer(harness.client, drainCodec(peer.codec)));
		while (!peer.headers && (co_await readPeer(harness.client, peer)))
		{
		}
		EXPECT_TRUE(peer.headers);
		EXPECT_FALSE(peer.ended);
		constexpr std::array<Codec::Setting, 1> kOpenWindow {{{Http2Setting::hInitialWindowSize, 8192}}};
		EXPECT_FALSE(peer.codec.submitSettings(kOpenWindow));
		if (!(co_await writePeer(harness.client, drainCodec(peer.codec))))
		{
			co_return;
		}
		while (!peer.ended && (co_await readPeer(harness.client, peer)))
		{
		}
	};
	auto scenario = [&]() -> Awaitable<void>
	{
		co_await harness.connect();
		co_await (server.connection->run() && peerRun());
	};
	harness.run(scenario());
	EXPECT_TRUE(peer.ended);
	EXPECT_EQ(peer.body, payload);
	EXPECT_EQ(server.written.size(), 1U);
	EXPECT_FALSE(server.connection->error());
}

TEST_F(Http2TransportTest, SlowTlsReaderTriggersOneWriteTimeoutAndJoinsTheReadPump)
{
	Harness harness;
	Http2Options options;
	options.writeTimeout = std::chrono::milliseconds {25};
	Http2ServiceBudget service(options.maxServerApplicationTasks);
	Http2Budget budget(Http2Budget::Role::hServer, options, &service);
	Endpoint server(harness.server, Codec::Role::hServer, options, budget);
	Http2Options peerOptions;
	peerOptions.initialStreamWindow = Http2Options::kMaxWindowSize;
	peerOptions.initialConnectionWindow = Http2Options::kMaxWindowSize;
	RawPeer peer(peerOptions);
	NetworkError result;
	const std::string payload(1024 * 1024, 's');
	server.headersReady = [&](std::shared_ptr<Http2Stream> stream)
	{
		auto response = server.connection->output().submitResponse(stream->id(), kResponse);
		EXPECT_FALSE(response.error);
		EXPECT_FALSE(response.producer->append(payload));
		EXPECT_FALSE(response.producer->finish());
	};
	auto peerRun = [&]() -> Awaitable<void>
	{
		EXPECT_FALSE(peer.codec.submitRequest(kRequest).error);
		EXPECT_TRUE(co_await writePeer(harness.client, drainCodec(peer.codec)));
		// 只发送请求，不读取 TLS 响应；等待故障通知，不靠固定 sleep 判断写阻塞。
		while (server.failures == 0)
		{
			co_await boost::asio::post(boost::asio::use_awaitable);
		}
	};
	auto serverRun = [&]() -> Awaitable<void>
	{
		result = co_await server.connection->run();
	};
	auto scenario = [&]() -> Awaitable<void>
	{
		// TCP 握手前设置接收缓冲，避免先协商出的较大 TCP 窗口掩盖慢读背压。
		harness.client.next_layer().open(boost::asio::ip::tcp::v4());
		harness.client.next_layer().set_option(boost::asio::socket_base::receive_buffer_size(1024));
		co_await harness.connect();
		harness.server.next_layer().set_option(boost::asio::socket_base::send_buffer_size(1024));
		co_await (serverRun() && peerRun());
	};
	harness.run(scenario());
	EXPECT_EQ(result.code, ErrorCode::hTimedOut);
	EXPECT_EQ(server.failures, 1U);
	EXPECT_FALSE(server.connection->running());
	EXPECT_TRUE(server.written.empty());
}
