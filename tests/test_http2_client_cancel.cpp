/**
 * @file test_http2_client_cancel.cpp
 * @brief 客户端提交期限、共享建连与逐流取消测试
 */

#include "asio/HttpClient.h"
#ifdef HICAL_HAS_HTTP2
	#include "asio/Http2ClientSession.h"
#endif
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/read_until.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/asio/use_future.hpp>
#include <gtest/gtest.h>
#include <array>
#include <chrono>
#include <fstream>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

using namespace hical;

namespace
{
	using Clock = std::chrono::steady_clock;
	using Tcp = boost::asio::ip::tcp;
	using TlsSocket = boost::asio::ssl::stream<Tcp::socket>;
	constexpr auto kWatchdog = std::chrono::seconds {4};
	constexpr auto kRequestLimit = size_t {16 * 1024};

	struct DeadlineHarness
	{
		boost::asio::io_context io;
		Tcp::acceptor acceptor {io, {boost::asio::ip::address_v4::loopback(), 0}};
		boost::asio::steady_timer watchdog {io};
		boost::asio::steady_timer releaseGate {io, Clock::time_point::max()};
		std::vector<std::shared_ptr<Tcp::socket>> plain;
		std::vector<std::shared_ptr<TlsSocket>> tls;
		std::shared_ptr<SslContext> serverContext;
		std::function<Awaitable<void>(std::shared_ptr<TlsSocket>)> h2Handler;
		std::function<void()> onHello;
		std::function<void()> onTimeout;
		size_t accepts {0};
		size_t requests {0};
		size_t handshakes {0};
		bool pauseHandshake {false};
		bool pauseFirstResponse {false};
		bool timedOut {false};

		uint16_t port() const
		{
			return acceptor.local_endpoint().port();
		}

		void release()
		{
			pauseHandshake = false;
			pauseFirstResponse = false;
			releaseGate.cancel();
		}

		void stop()
		{
			boost::system::error_code ignored;
			acceptor.close(ignored);
			for (const auto& socket : plain)
			{
				socket->close(ignored);
			}
			for (const auto& socket : tls)
			{
				socket->next_layer().close(ignored);
			}
			release();
			watchdog.cancel();
		}

		template <typename Socket>
		Awaitable<void> serveH1(std::shared_ptr<Socket> socket)
		{
			std::string input;
			boost::system::error_code error;
			for (;;)
			{
				const auto size = co_await boost::asio::async_read_until(
					*socket,
					boost::asio::dynamic_buffer(input, kRequestLimit),
					"\r\n\r\n",
					boost::asio::redirect_error(boost::asio::use_awaitable, error));
				if (error)
				{
					co_return;
				}
				const auto first = input.find(' ');
				const auto last = input.find(' ', first + 1);
				const auto target = input.substr(first + 1, last - first - 1);
				input.erase(0, size);
				++requests;
				if (requests == 1 && pauseFirstResponse)
				{
					co_await releaseGate.async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, error));
				}
				const auto response =
					"HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(target.size()) + "\r\n\r\n" + target;
				co_await boost::asio::async_write(*socket,
												  boost::asio::buffer(response),
												  boost::asio::redirect_error(boost::asio::use_awaitable, error));
				if (error)
				{
					co_return;
				}
			}
		}

		Awaitable<void> serveTls(std::shared_ptr<TlsSocket> socket)
		{
			boost::system::error_code error;
			if (pauseHandshake)
			{
				// PEEK 确认实际 ClientHello 已到，不消耗后续真实 TLS 握手需要的字节。
				std::array<char, 1> hello;
				co_await socket->next_layer().async_receive(
					boost::asio::buffer(hello),
					Tcp::socket::message_peek,
					boost::asio::redirect_error(boost::asio::use_awaitable, error));
				if (error)
				{
					co_return;
				}
				EXPECT_EQ(static_cast<unsigned char>(hello[0]), 0x16U);
				if (onHello)
				{
					onHello();
				}
				co_await releaseGate.async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, error));
			}
			co_await socket->async_handshake(boost::asio::ssl::stream_base::server,
											 boost::asio::redirect_error(boost::asio::use_awaitable, error));
			if (error)
			{
				co_return;
			}
			++handshakes;
			if (h2Handler)
			{
				co_await h2Handler(std::move(socket));
			}
			else
			{
				co_await serveH1(std::move(socket));
			}
		}

		Awaitable<void> accept()
		{
			for (;;)
			{
				auto socket = std::make_shared<Tcp::socket>(io);
				boost::system::error_code error;
				co_await acceptor.async_accept(*socket, boost::asio::redirect_error(boost::asio::use_awaitable, error));
				if (error)
				{
					co_return;
				}
				++accepts;
				if (serverContext)
				{
					auto secure = std::make_shared<TlsSocket>(std::move(*socket), serverContext->native());
					tls.push_back(secure);
					coSpawn(io, serveTls(std::move(secure)));
				}
				else
				{
					plain.push_back(socket);
					coSpawn(io, serveH1(std::move(socket)));
				}
			}
		}

		static Awaitable<void> execute(DeadlineHarness& harness, Awaitable<void> scenario)
		{
			try
			{
				co_await std::move(scenario);
			}
			catch (const std::exception& error)
			{
				ADD_FAILURE() << error.what();
			}
			harness.stop();
		}

		void run(Awaitable<void> scenario)
		{
			watchdog.expires_at(Clock::now() + kWatchdog);
			watchdog.async_wait(
				[this](boost::system::error_code error)
				{
					if (!error)
					{
						timedOut = true;
						if (onTimeout)
						{
							onTimeout();
						}
						stop();
					}
				});
			coSpawn(io, accept());
			auto future = boost::asio::co_spawn(io, execute(*this, std::move(scenario)), boost::asio::use_future);
			io.run();
			EXPECT_FALSE(timedOut);
			EXPECT_NO_THROW(future.get());
		}
	};

	class Http2ClientCancelTlsTest : public testing::Test
	{
	protected:
		void SetUp() override
		{
			ASSERT_TRUE(std::ifstream("test_server.crt").good()) << "取消行为测试缺少测试证书";
			ASSERT_TRUE(std::ifstream("test_server.key").good()) << "取消行为测试缺少测试私钥";
		}

		static std::shared_ptr<SslContext> serverContext()
		{
			auto result = std::make_shared<SslContext>(boost::asio::ssl::context::tls_server);
			result->loadCertificate("test_server.crt");
			result->loadPrivateKey("test_server.key");
			return result;
		}

		static HttpClientOptions tlsOptions()
		{
			HttpClientOptions options;
			options.tlsContext = std::make_shared<SslContext>(boost::asio::ssl::context::tls_client);
			options.tlsContext->loadCaCertificate("test_server.crt");
			options.requestTimeout = std::chrono::seconds {2};
			return options;
		}
	};
} // namespace

TEST(Http2ClientDeadlineTest, SavedAwaitableStartsAfterItsOriginalDeadlineWithoutConnecting)
{
	DeadlineHarness harness;
	HttpClientOptions options;
	options.requestTimeout = std::chrono::milliseconds {20};
	HttpClient client(harness.io, "127.0.0.1", harness.port(), options);
	const auto submitted = Clock::now();
	auto pending = client.get("/expired");
	auto scenario = [&]() -> Awaitable<void>
	{
		boost::asio::steady_timer start(harness.io, submitted + std::chrono::milliseconds {60});
		co_await start.async_wait(boost::asio::use_awaitable);
		const auto result = co_await std::move(pending);
		EXPECT_EQ(result.error.code, ErrorCode::hTimedOut);
		EXPECT_EQ(result.requestDisposition, HttpRequestDisposition::hNotSent);
		client.close();
	};
	harness.run(scenario());
	EXPECT_EQ(harness.accepts, 0U);
	EXPECT_EQ(harness.requests, 0U);
}

TEST(Http2ClientDeadlineTest, NonPositiveTimeoutsStayDisabledAcrossDelayedSubmissionAndTheExchange)
{
	for (const auto timeout : {std::chrono::milliseconds {0}, std::chrono::milliseconds {-1}})
	{
		DeadlineHarness harness;
		HttpClientOptions options;
		options.requestTimeout = timeout;
		options.connectTimeout = timeout;
		HttpClient client(harness.io, "127.0.0.1", harness.port(), options);
		auto pending = client.get("/unlimited");
		auto scenario = [&]() -> Awaitable<void>
		{
			boost::asio::steady_timer delayed(harness.io, Clock::now() + std::chrono::milliseconds {30});
			co_await delayed.async_wait(boost::asio::use_awaitable);
			const auto result = co_await std::move(pending);
			EXPECT_TRUE(result.ok()) << result.error.message;
			EXPECT_EQ(result.body, "/unlimited");
			client.close();
		};
		harness.run(scenario());
		EXPECT_EQ(harness.accepts, 1U);
	}
}

TEST_F(Http2ClientCancelTlsTest, CancellingOneActualClientHelloWaiterDoesNotCancelTheSharedHandshake)
{
	DeadlineHarness harness;
	harness.serverContext = serverContext();
	harness.pauseHandshake = true;
	HttpClient client(harness.io, "127.0.0.1", harness.port(), tlsOptions());
	boost::asio::cancellation_signal cancellation;
	std::optional<HttpClientResult> first, second;
	bool helloSeen = false;
	bool secondEntered = false;
	harness.onHello = [&]
	{
		helloSeen = true;
	};
	harness.onTimeout = [&]
	{
		client.close();
	};
	auto secondRequest = [&]() -> Awaitable<HttpClientResult>
	{
		secondEntered = true;
		co_return co_await client.get("/survivor");
	};
	auto scenario = [&]() -> Awaitable<void>
	{
		boost::asio::co_spawn(harness.io,
							  client.get("/cancelled"),
							  boost::asio::bind_cancellation_slot(cancellation.slot(),
																  [&](std::exception_ptr error, HttpClientResult result)
																  {
																	  EXPECT_FALSE(error);
																	  first = std::move(result);
																	  harness.release();
																  }));
		boost::asio::co_spawn(harness.io,
							  secondRequest(),
							  [&](std::exception_ptr error, HttpClientResult result)
							  {
								  EXPECT_FALSE(error);
								  second = std::move(result);
							  });
		while ((!helloSeen || !secondEntered) && !harness.timedOut)
		{
			co_await boost::asio::post(boost::asio::use_awaitable);
		}
		cancellation.emit(boost::asio::cancellation_type::terminal);
		while ((!first || !second) && !harness.timedOut)
		{
			co_await boost::asio::post(boost::asio::use_awaitable);
		}
		EXPECT_TRUE(first.has_value());
		EXPECT_TRUE(second.has_value());
		if (!first || !second)
		{
			co_return;
		}
		EXPECT_EQ(first->error.code, ErrorCode::hOperationAborted);
		EXPECT_EQ(first->requestDisposition, HttpRequestDisposition::hNotSent);
		EXPECT_TRUE(second->ok()) << second->error.message;
		EXPECT_EQ(second->body, "/survivor");
		client.close();
	};
	harness.run(scenario());
	EXPECT_TRUE(helloSeen);
	EXPECT_EQ(harness.accepts, 1U);
	EXPECT_EQ(harness.handshakes, 1U);
	EXPECT_EQ(harness.requests, 1U);
}

TEST_F(Http2ClientCancelTlsTest, OneHandshakeWaiterDeadlineDoesNotExpireTheOtherWaitersFlight)
{
	DeadlineHarness harness;
	harness.serverContext = serverContext();
	harness.pauseHandshake = true;
	auto options = tlsOptions();
	options.requestTimeout = std::chrono::seconds {2};
	HttpClient client(harness.io, "127.0.0.1", harness.port(), options);
	const auto submitted = Clock::now();
	std::optional<HttpClientResult> first, second;
	bool helloSeen = false;
	bool secondEntered = false;
	harness.onHello = [&]
	{
		helloSeen = true;
	};
	harness.onTimeout = [&]
	{
		client.close();
	};
	auto secondRequest = [&]() -> Awaitable<HttpClientResult>
	{
		secondEntered = true;
		co_return co_await client.get("/survivor");
	};
	auto scenario = [&]() -> Awaitable<void>
	{
		boost::asio::co_spawn(harness.io,
							  client.get("/expires"),
							  [&](std::exception_ptr error, HttpClientResult result)
							  {
								  EXPECT_FALSE(error);
								  first = std::move(result);
								  harness.release();
							  });
		while (!helloSeen && !harness.timedOut)
		{
			co_await boost::asio::post(boost::asio::use_awaitable);
		}
		// 收到 ClientHello 后再发第二个请求，让两个请求的绝对期限错开。
		boost::asio::steady_timer stagger(harness.io, submitted + std::chrono::milliseconds {1500});
		co_await stagger.async_wait(boost::asio::use_awaitable);
		EXPECT_FALSE(first.has_value());
		boost::asio::co_spawn(harness.io,
							  secondRequest(),
							  [&](std::exception_ptr error, HttpClientResult result)
							  {
								  EXPECT_FALSE(error);
								  second = std::move(result);
							  });
		while (!secondEntered && !harness.timedOut)
		{
			co_await boost::asio::post(boost::asio::use_awaitable);
		}
		EXPECT_FALSE(first.has_value()) << "第二个调用必须在首个调用超时前加入同一次握手";
		while ((!first || !second) && !harness.timedOut)
		{
			co_await boost::asio::post(boost::asio::use_awaitable);
		}
		EXPECT_TRUE(first.has_value());
		EXPECT_TRUE(second.has_value());
		if (!first || !second)
		{
			co_return;
		}
		EXPECT_EQ(first->error.code, ErrorCode::hTimedOut);
		EXPECT_EQ(first->requestDisposition, HttpRequestDisposition::hNotSent);
		EXPECT_TRUE(second->ok()) << second->error.message;
		EXPECT_EQ(second->body, "/survivor");
		client.close();
	};
	harness.run(scenario());
	EXPECT_TRUE(helloSeen);
	EXPECT_EQ(harness.accepts, 1U);
	EXPECT_EQ(harness.handshakes, 1U);
	EXPECT_EQ(harness.requests, 1U);
}

TEST(Http2ClientDeadlineTest, CancellingAnH1GateWaiterLeavesTheOwnerAndItsSuccessorUsable)
{
	DeadlineHarness harness;
	harness.pauseFirstResponse = true;
	HttpClient client(harness.io, "127.0.0.1", harness.port());
	boost::asio::cancellation_signal cancellation;
	std::optional<HttpClientResult> first, queued;
	bool queueEntered = false;
	harness.onTimeout = [&]
	{
		client.close();
	};
	auto queuedRequest = [&]() -> Awaitable<HttpClientResult>
	{
		queueEntered = true;
		co_return co_await client.get("/cancelled");
	};
	auto scenario = [&]() -> Awaitable<void>
	{
		boost::asio::co_spawn(harness.io,
							  client.get("/owner"),
							  [&](std::exception_ptr error, HttpClientResult result)
							  {
								  EXPECT_FALSE(error);
								  first = std::move(result);
							  });
		while (harness.requests != 1 && !harness.timedOut)
		{
			co_await boost::asio::post(boost::asio::use_awaitable);
		}
		boost::asio::co_spawn(harness.io,
							  queuedRequest(),
							  boost::asio::bind_cancellation_slot(cancellation.slot(),
																  [&](std::exception_ptr error, HttpClientResult result)
																  {
																	  EXPECT_FALSE(error);
																	  queued = std::move(result);
																  }));
		while (!queueEntered && !harness.timedOut)
		{
			co_await boost::asio::post(boost::asio::use_awaitable);
		}
		cancellation.emit(boost::asio::cancellation_type::terminal);
		while (!queued && !harness.timedOut)
		{
			co_await boost::asio::post(boost::asio::use_awaitable);
		}
		EXPECT_TRUE(queued.has_value());
		if (!queued)
		{
			co_return;
		}
		EXPECT_EQ(queued->error.code, ErrorCode::hOperationAborted);
		EXPECT_EQ(queued->requestDisposition, HttpRequestDisposition::hNotSent);
		EXPECT_FALSE(first.has_value());
		EXPECT_TRUE(client.isAlive());
		harness.release();
		while (!first && !harness.timedOut)
		{
			co_await boost::asio::post(boost::asio::use_awaitable);
		}
		EXPECT_TRUE(first.has_value());
		if (!first)
		{
			co_return;
		}
		EXPECT_TRUE(first->ok()) << first->error.message;
		const auto successor = co_await client.get("/successor");
		EXPECT_TRUE(successor.ok()) << successor.error.message;
		EXPECT_EQ(successor.body, "/successor");
		client.close();
	};
	harness.run(scenario());
	EXPECT_EQ(harness.accepts, 1U);
	EXPECT_EQ(harness.requests, 2U);
}

TEST(Http2ClientDeadlineTest, H1GateWaiterDeadlineDoesNotCancelTheLongerLivedOwner)
{
	DeadlineHarness harness;
	harness.pauseFirstResponse = true;
	HttpClientOptions options;
	options.requestTimeout = std::chrono::seconds {1};
	HttpClient client(harness.io, "127.0.0.1", harness.port(), options);
	const auto submitted = Clock::now();
	auto pending = client.get("/expires-in-gate");
	std::optional<HttpClientResult> owner, queued;
	harness.onTimeout = [&]
	{
		client.close();
	};
	auto scenario = [&]() -> Awaitable<void>
	{
		boost::asio::steady_timer stagger(harness.io, submitted + std::chrono::milliseconds {500});
		co_await stagger.async_wait(boost::asio::use_awaitable);
		boost::asio::co_spawn(harness.io,
							  client.get("/owner"),
							  [&](std::exception_ptr error, HttpClientResult value)
							  {
								  EXPECT_FALSE(error);
								  owner = std::move(value);
							  });
		while (harness.requests != 1 && !harness.timedOut)
		{
			co_await boost::asio::post(boost::asio::use_awaitable);
		}
		boost::asio::co_spawn(harness.io,
							  std::move(pending),
							  [&](std::exception_ptr error, HttpClientResult value)
							  {
								  EXPECT_FALSE(error);
								  queued = std::move(value);
								  harness.release();
							  });
		while ((!owner || !queued) && !harness.timedOut)
		{
			co_await boost::asio::post(boost::asio::use_awaitable);
		}
		EXPECT_TRUE(owner.has_value());
		EXPECT_TRUE(queued.has_value());
		if (!owner || !queued)
		{
			co_return;
		}
		EXPECT_EQ(queued->error.code, ErrorCode::hTimedOut);
		EXPECT_EQ(queued->requestDisposition, HttpRequestDisposition::hNotSent);
		EXPECT_TRUE(owner->ok()) << owner->error.message;
		const auto next = co_await client.get("/next");
		EXPECT_TRUE(next.ok()) << next.error.message;
		client.close();
	};
	harness.run(scenario());
	EXPECT_EQ(harness.accepts, 1U);
	EXPECT_EQ(harness.requests, 2U);
}

#ifdef HICAL_HAS_HTTP2
namespace
{
	struct DeadlineH2Peer final : detail::Http2Codec::Listener
	{
		std::shared_ptr<detail::Http2Connection> connection;
		std::unordered_map<int32_t, std::string> paths;
		int32_t slowId {0};
		int32_t survivorId {0};
		std::vector<int32_t> resets;

		detail::Http2Code onHeader(int32_t id, detail::Http2Codec::Header header) override
		{
			if (header.name == ":path")
			{
				paths[id] = header.value;
				if (header.value == "/slow")
				{
					slowId = id;
				}
			}
			return detail::Http2Code::hNoError;
		}

		void reply(int32_t id)
		{
			const detail::Http2Codec::Header fields[] {{":status", "200"}, {"content-length", "0"}};
			EXPECT_FALSE(connection->output().submitResponseHeaders(id, fields).error);
		}

		void onEndStream(int32_t id) override
		{
			if (paths[id] == "/survivor")
			{
				survivorId = id;
				if (!resets.empty())
				{
					reply(id);
				}
			}
			else if (paths[id] == "/next" || paths[id] == "/warmup")
			{
				reply(id);
			}
		}

		void onStreamClose(int32_t id, detail::Http2Code code) override
		{
			if (id == slowId && code == detail::Http2Code::hCancel)
			{
				resets.push_back(id);
				if (survivorId != 0)
				{
					reply(survivorId);
				}
			}
		}
	};
} // namespace

TEST_F(Http2ClientCancelTlsTest, ZeroWindowDeadlineResetsOnlyItsStreamAndLeavesTwoOtherRequestsUsable)
{
	DeadlineHarness harness;
	harness.serverContext = serverContext();
	harness.serverContext->setServerHttpProtocolMode(HttpProtocolMode::hNegotiateHttp2);
	harness.serverContext->freezeServerHttpProtocolConfig();
	detail::Http2ServiceBudget service(8);
	auto peer = std::make_shared<DeadlineH2Peer>();
	harness.h2Handler = [&](std::shared_ptr<TlsSocket> socket) -> Awaitable<void>
	{
		Http2Options options;
		options.initialStreamWindow = 0;
		options.maxConcurrentStreams = 2;
		peer->connection =
			detail::Http2Connection::create(*socket,
											detail::Http2Codec::Role::hServer,
											options,
											detail::Http2Budget(detail::Http2Budget::Role::hServer, options, &service));
		EXPECT_FALSE(peer->connection->setListener(peer.get()));
		co_await peer->connection->run();
	};
	const auto configuration = tlsOptions();
	std::shared_ptr<detail::Http2ClientSession> session;
	harness.onTimeout = [&]
	{
		if (session)
		{
			session->close();
		}
	};
	std::optional<HttpClientResult> slow, survivor;
	size_t slowCompletions = 0;
	bool runnerDone = false;
	auto scenario = [&]() -> Awaitable<void>
	{
		auto socket = std::make_shared<TlsSocket>(harness.io, configuration.tlsContext->native());
		socket->set_verify_mode(boost::asio::ssl::verify_peer);
		EXPECT_FALSE(SslContext::configureClientHttpProtocols(socket->native_handle(), HttpProtocolMode::hHttp2Only));
		co_await socket->next_layer().async_connect(harness.acceptor.local_endpoint(), boost::asio::use_awaitable);
		co_await socket->async_handshake(boost::asio::ssl::stream_base::client, boost::asio::use_awaitable);
		session = detail::Http2ClientSession::create(socket);
		boost::asio::co_spawn(harness.io,
							  session->run(),
							  [&](std::exception_ptr error, NetworkError)
							  {
								  EXPECT_FALSE(error);
								  runnerDone = true;
							  });
		// 先完成 SETTINGS 和一次真实交换，短期限只测试已建立连接上的零窗口流。
		detail::Http2ClientSession::Request warmup;
		warmup.authority = "localhost";
		warmup.target = "/warmup";
		warmup.deadline = Clock::now() + std::chrono::seconds {2};
		const auto warmed = co_await session->request(std::move(warmup));
		EXPECT_TRUE(warmed.ok()) << warmed.error.message;
		if (!warmed.ok())
		{
			session->close();
			co_return;
		}
		const auto submitted = Clock::now();
		detail::Http2ClientSession::Request first;
		first.method = HttpMethod::hPost;
		first.authority = "localhost";
		first.target = "/slow";
		first.body.assign(128, 'x');
		first.deadline = submitted + std::chrono::milliseconds {250};
		boost::asio::co_spawn(harness.io,
							  session->request(std::move(first)),
							  [&](std::exception_ptr error, HttpClientResult result)
							  {
								  EXPECT_FALSE(error);
								  ++slowCompletions;
								  slow = std::move(result);
							  });
		detail::Http2ClientSession::Request second;
		second.authority = "localhost";
		second.target = "/survivor";
		second.deadline = submitted + std::chrono::seconds {2};
		boost::asio::co_spawn(harness.io,
							  session->request(std::move(second)),
							  [&](std::exception_ptr error, HttpClientResult result)
							  {
								  EXPECT_FALSE(error);
								  survivor = std::move(result);
							  });
		while ((!slow || !survivor) && !harness.timedOut)
		{
			co_await boost::asio::post(boost::asio::use_awaitable);
		}
		EXPECT_TRUE(slow.has_value());
		EXPECT_TRUE(survivor.has_value());
		if (!slow || !survivor)
		{
			co_return;
		}
		EXPECT_EQ(slow->error.code, ErrorCode::hTimedOut);
		EXPECT_GT(peer->slowId, 0) << "短期限必须在请求头发到对端后才到期";
		EXPECT_EQ(slow->requestDisposition, HttpRequestDisposition::hPossiblyProcessed);
		EXPECT_TRUE(survivor->ok()) << survivor->error.message;
		EXPECT_EQ(peer->resets, (std::vector<int32_t> {peer->slowId}));
		detail::Http2ClientSession::Request third;
		third.authority = "localhost";
		third.target = "/next";
		third.deadline = Clock::now() + std::chrono::seconds {2};
		const auto next = co_await session->request(std::move(third));
		EXPECT_TRUE(next.ok()) << next.error.message;
		EXPECT_EQ(slowCompletions, 1U);
		session->close();
		while (!runnerDone && !harness.timedOut)
		{
			co_await boost::asio::post(boost::asio::use_awaitable);
		}
	};
	harness.run(scenario());
	EXPECT_EQ(harness.accepts, 1U);
	EXPECT_EQ(harness.handshakes, 1U);
	EXPECT_EQ(slowCompletions, 1U);
}

TEST(Http2ClientDeadlineTest, LateEndStreamCannotReplaceTheFirstDeadlineResultOrCompleteTwice)
{
	boost::asio::io_context io;
	SslContext tls(boost::asio::ssl::context::tls_client);
	auto socket = std::make_shared<TlsSocket>(io, tls.native());
	auto session = detail::Http2ClientSession::create(socket);
	detail::Http2Codec peer(detail::Http2Codec::Role::hServer);
	auto fromPeer = [&]
	{
		for (size_t n = 0; n != 16; ++n)
		{
			auto wire = peer.memSend();
			ASSERT_FALSE(wire.error);
			if (wire.bytes.empty())
			{
				return;
			}
			ASSERT_FALSE(session->connection().codec().memRecv(wire.bytes).error);
		}
		FAIL() << "对端输出未在有限批次内排空";
	};
	fromPeer();
	const auto deadline = Clock::now() + std::chrono::seconds {1};
	detail::Http2ClientSession::Request request;
	request.authority = "localhost";
	request.target = "/late";
	request.deadline = deadline;
	std::optional<HttpClientResult> result;
	size_t completions = 0;
	size_t completionsBeforeLateEnd = 0;
	boost::asio::steady_timer watchdog(io, Clock::now() + kWatchdog);
	bool timedOut = false;
	watchdog.async_wait(
		[&](boost::system::error_code error)
		{
			if (!error)
			{
				timedOut = true;
				session->close();
			}
		});
	boost::asio::co_spawn(
		io,
		session->request(std::move(request)),
		[&](std::exception_ptr error, HttpClientResult value)
		{
			EXPECT_FALSE(error);
			++completions;
			result = std::move(value);
			boost::asio::post(
				io,
				[&]
				{
					completionsBeforeLateEnd = completions;
					// 原请求已经完成，再送迟到的 END，不能覆盖结果或重复完成。
					const std::array<char, 9> end {0, 0, 0, 0, 1, 0, 0, 0, 1};
					EXPECT_FALSE(session->connection().codec().memRecv(std::string_view(end.data(), end.size())).error);
					boost::asio::post(io,
									  [&]
									  {
										  session->close();
										  watchdog.cancel();
									  });
				});
		});
	io.poll();
	ASSERT_FALSE(result.has_value()) << "先完成请求提交，再验证等待响应时的超时";
	ASSERT_EQ(session->capacity().admitted, 1U);
	for (size_t n = 0; n != 16; ++n)
	{
		auto wire = session->connection().output().nextBatch();
		ASSERT_FALSE(wire.error);
		if (wire.bytes.empty())
		{
			break;
		}
		ASSERT_FALSE(peer.memRecv(wire.bytes).error);
		ASSERT_FALSE(session->connection().output().acknowledge(wire.bytes.size()).error);
	}
	const detail::Http2Codec::Header head[] {{":status", "200"}, {"content-length", "0"}};
	ASSERT_FALSE(peer.submitResponse(1, head, false));
	fromPeer();
	io.restart();
	io.run();
	EXPECT_FALSE(timedOut);
	EXPECT_EQ(completionsBeforeLateEnd, 1U);
	EXPECT_EQ(completions, 1U);
	ASSERT_TRUE(result.has_value());
	EXPECT_EQ(result->error.code, ErrorCode::hTimedOut);
	EXPECT_TRUE(result->body.empty());
	EXPECT_EQ(result->requestDisposition, HttpRequestDisposition::hPossiblyProcessed);
}

TEST(Http2ClientDeadlineTest, CancellingAZeroCapacityWaiterReturnsTheOnlyQueueSlotWithoutAllocatingAnId)
{
	boost::asio::io_context io;
	SslContext tls(boost::asio::ssl::context::tls_client);
	auto socket = std::make_shared<TlsSocket>(io, tls.native());
	Http2Options options;
	options.maxQueuedRequestsPerOrigin = 1;
	auto session = detail::Http2ClientSession::create(socket, options);
	detail::Http2Codec peer(detail::Http2Codec::Role::hServer);
	auto settings = [&](uint32_t maximum)
	{
		const detail::Http2Codec::Setting setting {detail::Http2Setting::hMaxConcurrentStreams, maximum};
		ASSERT_FALSE(peer.submitSettings(std::span(&setting, 1)));
		for (size_t n = 0; n != 16; ++n)
		{
			auto wire = peer.memSend();
			ASSERT_FALSE(wire.error);
			if (wire.bytes.empty())
			{
				return;
			}
			ASSERT_FALSE(session->connection().codec().memRecv(wire.bytes).error);
		}
	};
	settings(0);
	detail::Http2ClientSession::Request request;
	request.authority = "localhost";
	request.target = "/queued";
	request.deadline = Clock::now() + std::chrono::seconds {2};
	boost::asio::cancellation_signal cancellation;
	auto first =
		boost::asio::co_spawn(io,
							  session->request(request),
							  boost::asio::bind_cancellation_slot(cancellation.slot(), boost::asio::use_future));
	io.poll();
	EXPECT_EQ(first.wait_for(std::chrono::seconds::zero()), std::future_status::timeout);
	EXPECT_EQ(session->capacity().admitted, 0U);
	auto overflow = boost::asio::co_spawn(io, session->request(request), boost::asio::use_future);
	io.restart();
	io.poll();
	ASSERT_EQ(overflow.wait_for(std::chrono::seconds::zero()), std::future_status::ready);
	const auto rejected = overflow.get();
	EXPECT_EQ(rejected.error.code, ErrorCode::hResourceLimitExceeded);
	EXPECT_EQ(rejected.requestDisposition, HttpRequestDisposition::hNotSent);
	cancellation.emit(boost::asio::cancellation_type::terminal);
	io.restart();
	io.poll();
	ASSERT_EQ(first.wait_for(std::chrono::seconds::zero()), std::future_status::ready);
	const auto cancelled = first.get();
	EXPECT_EQ(cancelled.error.code, ErrorCode::hOperationAborted);
	EXPECT_EQ(cancelled.requestDisposition, HttpRequestDisposition::hNotSent);
	EXPECT_FALSE(cancelled.error.http2.has_value());
	auto next = boost::asio::co_spawn(io, session->request(request), boost::asio::use_future);
	io.restart();
	io.poll();
	EXPECT_EQ(next.wait_for(std::chrono::seconds::zero()), std::future_status::timeout);
	settings(1);
	io.restart();
	io.poll();
	EXPECT_EQ(session->capacity().admitted, 1U);
	for (size_t n = 0; n != 16; ++n)
	{
		auto wire = session->connection().output().nextBatch();
		ASSERT_FALSE(wire.error);
		if (wire.bytes.empty())
		{
			break;
		}
		ASSERT_FALSE(peer.memRecv(wire.bytes).error);
		ASSERT_FALSE(session->connection().output().acknowledge(wire.bytes.size()).error);
	}
	// 被取消的队列请求和溢出拒绝都未占 ID，恢复后的第一个实际请求仍是 1。
	const detail::Http2Codec::Header response[] {{":status", "200"}};
	ASSERT_FALSE(peer.submitResponse(1, response));
	settings(1);
	io.restart();
	io.poll();
	ASSERT_EQ(next.wait_for(std::chrono::seconds::zero()), std::future_status::ready);
	EXPECT_TRUE(next.get().ok());
	session->close();
	io.restart();
	io.poll();
}
#endif
