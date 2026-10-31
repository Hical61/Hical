/**
 * @file test_http2_drain.cpp
 * @brief HTTP/2 GOAWAY、连接排空与停机接口测试
 */

#include "core/HttpServer.h"
#include "core/Log.h"
#include "core/http2/Http2Codec.h"
#include "core/http2/Http2ServerSession.h"
#include <boost/asio/cancel_at.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/use_future.hpp>
#include <gtest/gtest.h>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <future>
#include <map>
#include <limits>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>

using namespace hical;
using namespace hical::detail;
using namespace boost::asio::experimental::awaitable_operators;

namespace
{
	using Codec = Http2Codec;
	using Clock = std::chrono::steady_clock;
	using TimePoint = Clock::time_point;
	constexpr auto kTestTimeout = std::chrono::seconds {10};
	constexpr std::array<Codec::Header, 4> kRequest {
		{{":method", "POST"}, {":scheme", "https"}, {":authority", "localhost"}, {":path", "/work"}}};

	template <typename Predicate>
	bool waitUntil(TimePoint deadline, Predicate predicate)
	{
		while (!predicate())
		{
			if (Clock::now() >= deadline)
			{
				return false;
			}
			std::this_thread::yield();
		}
		return true;
	}

	class RunningServer
	{
	public:
		explicit RunningServer(bool http2 = false, size_t threads = 1) : server(0, threads)
		{
			server.setIdleTimeout(0);
			server.setGcInterval(0);
			server.setShutdownTimeout(1);
			if (http2)
			{
				server.enableSsl("test_server.crt", "test_server.key");
				server.enableHttp2();
			}
		}

		~RunningServer()
		{
			server.stop();
			if (thread_.joinable())
			{
				thread_.join();
			}
		}

		bool start(TimePoint deadline)
		{
			auto ready = std::make_shared<std::promise<void>>();
			auto started = ready->get_future();
			// 等 start 完成配置并启动所有 worker 后再通知就绪，避免探测连接干扰计数。
			boost::asio::post(server.ioContext(),
							  [ready]
							  {
								  ready->set_value();
							  });
			finished_ = done_.get_future();
			thread_ = std::thread(
				[this]
				{
					try
					{
						server.start();
						done_.set_value();
					}
					catch (...)
					{
						done_.set_exception(std::current_exception());
					}
				});
			return started.wait_until(deadline) == std::future_status::ready;
		}

		bool stopped(TimePoint deadline)
		{
			if (finished_.wait_until(deadline) != std::future_status::ready)
			{
				return false;
			}
			finished_.get();
			return true;
		}

		HttpServer server;

	private:
		std::thread thread_;
		std::promise<void> done_;
		std::future<void> finished_;
	};

	struct Peer : Codec::Listener
	{
		explicit Peer(const Http2Options& options = {}) : codec(Codec::Role::hClient, options, this)
		{
		}

		Codec codec;
		bool settings {false};
		bool goaway {false};
		int32_t lastAccepted {-1};
		std::map<int32_t, uint16_t> status;
		std::map<int32_t, std::string> body;
		std::map<int32_t, Http2Code> closed;
		std::set<int32_t> ended;

		void onSettings(bool ack) override
		{
			settings = settings || !ack;
		}

		void onGoaway(int32_t lastStream, Http2Code code, std::string_view) override
		{
			goaway = true;
			lastAccepted = lastStream;
			EXPECT_EQ(code, Http2Code::hNoError);
		}

		Http2Code onEndHeaders(const Codec::HeaderBlock& block) override
		{
			if (block.kind == Codec::HeaderKind::hResponse && block.statusCode >= 200)
			{
				status[block.streamId] = block.statusCode;
			}
			return Http2Code::hNoError;
		}

		Http2Code onData(int32_t id, std::string_view bytes) override
		{
			body[id].append(bytes);
			EXPECT_FALSE(codec.consume(id, bytes.size()));
			return Http2Code::hNoError;
		}

		void onEndStream(int32_t id) override
		{
			ended.insert(id);
		}

		void onStreamClose(int32_t id, Http2Code code) override
		{
			closed[id] = code;
		}
	};

	struct Client
	{
		boost::asio::io_context io;
		boost::asio::ssl::context context {boost::asio::ssl::context::tls_client};
		boost::asio::ssl::stream<boost::asio::ip::tcp::socket> socket {io, context};

		Client()
		{
			socket.set_verify_mode(boost::asio::ssl::verify_none);
		}

		Awaitable<void> flush(Peer& peer, TimePoint deadline)
		{
			for (;;)
			{
				auto batch = peer.codec.memSend();
				if (batch.error)
				{
					throw std::runtime_error(batch.error.message);
				}
				if (batch.bytes.empty())
				{
					co_return;
				}
				co_await boost::asio::async_write(socket,
												  boost::asio::buffer(batch.bytes),
												  boost::asio::cancel_at(deadline, boost::asio::use_awaitable));
			}
		}

		Awaitable<void> read(Peer& peer, TimePoint deadline)
		{
			std::array<char, 16384> buffer;
			boost::system::error_code error;
			auto size = co_await socket.async_read_some(
				boost::asio::buffer(buffer),
				boost::asio::cancel_at(deadline, boost::asio::redirect_error(boost::asio::use_awaitable, error)));
			if (error)
			{
				HICAL_LOG_ERROR("排空读失败 code={} GOAWAY={} last={} statuses={} bodies={} ends={}",
								error.value(),
								peer.goaway,
								peer.lastAccepted,
								peer.status.size(),
								peer.body.size(),
								peer.ended.size());
				throw boost::system::system_error(error);
			}
			auto result = peer.codec.memRecv({buffer.data(), size});
			EXPECT_FALSE(result.error);
			EXPECT_EQ(result.consumed, size);
		}

		Awaitable<void> connect(uint16_t port, Peer& peer, TimePoint deadline)
		{
			co_await handshake(port, deadline);
			co_await flush(peer, deadline);
			while (!peer.settings)
			{
				co_await read(peer, deadline);
			}
			co_await flush(peer, deadline);
		}

		Awaitable<void> handshake(uint16_t port, TimePoint deadline)
		{
			if (auto error =
					SslContext::configureClientHttpProtocols(socket.native_handle(), HttpProtocolMode::hHttp2Only))
			{
				throw std::runtime_error(error.message);
			}
			co_await socket.next_layer().async_connect({boost::asio::ip::address_v4::loopback(), port},
													   boost::asio::cancel_at(deadline, boost::asio::use_awaitable));
			co_await socket.async_handshake(boost::asio::ssl::stream_base::client,
											boost::asio::cancel_at(deadline, boost::asio::use_awaitable));
		}

		void run(Awaitable<void> scenario)
		{
			auto result = boost::asio::co_spawn(io, std::move(scenario), boost::asio::use_future);
			io.run();
			EXPECT_NO_THROW(result.get());
		}
	};

	struct PartialHeaderHarness
	{
		static boost::asio::ssl::context makeServerContext()
		{
			boost::asio::ssl::context context(boost::asio::ssl::context::tls_server);
			context.use_certificate_chain_file("test_server.crt");
			context.use_private_key_file("test_server.key", boost::asio::ssl::context::pem);
			return context;
		}

		Client client;
		boost::asio::ssl::context serverContext {makeServerContext()};
		boost::asio::ip::tcp::acceptor acceptor {client.io, {boost::asio::ip::address_v4::loopback(), 0}};
		Http2Connection::TlsSocket server {client.io, serverContext};
		Router router;
		MiddlewarePipeline middleware;
		Http2Options options;
		std::shared_ptr<Http2ServiceBudget> service = std::make_shared<Http2ServiceBudget>(64);
		std::shared_ptr<Http2ServerSession> session;
		std::future<NetworkError> finished;

		void close()
		{
			boost::system::error_code ignored;
			server.next_layer().close(ignored);
			client.socket.next_layer().close(ignored);
		}

		Awaitable<void> acceptAndHandshake(TimePoint deadline)
		{
			co_await acceptor.async_accept(server.next_layer(),
										   boost::asio::cancel_at(deadline, boost::asio::use_awaitable));
			co_await server.async_handshake(boost::asio::ssl::stream_base::server,
											boost::asio::cancel_at(deadline, boost::asio::use_awaitable));
		}

		Awaitable<void> start(Peer& peer, TimePoint deadline)
		{
			// 在 TCP 协商前缩小接收窗口；握手完成后只读响应头，后续 DATA 必须遇到实际 TLS 背压。
			client.socket.next_layer().open(boost::asio::ip::tcp::v4());
			client.socket.next_layer().set_option(boost::asio::socket_base::receive_buffer_size(1024));
			co_await (acceptAndHandshake(deadline) && client.handshake(acceptor.local_endpoint().port(), deadline));
			server.next_layer().set_option(boost::asio::socket_base::send_buffer_size(1024));
			session = Http2ServerSession::create(server, options, {router, middleware, {}, {}, service, {}});
			finished = boost::asio::co_spawn(client.io, session->run(), boost::asio::use_future);
			co_await client.flush(peer, deadline);
			while (!peer.settings)
			{
				co_await client.read(peer, deadline);
			}
			co_await client.flush(peer, deadline);
		}

		Awaitable<void> waitForUsage(size_t expected, TimePoint deadline)
		{
			while (service->used() != expected)
			{
				if (Clock::now() >= deadline)
				{
					throw std::runtime_error("半截初始头没有达到预期应用预算状态");
				}
				co_await boost::asio::post(boost::asio::use_awaitable);
			}
		}

		static Awaitable<void> execute(PartialHeaderHarness& harness, Awaitable<void> scenario)
		{
			try
			{
				co_await std::move(scenario);
			}
			catch (...)
			{
				harness.close();
				throw;
			}
			harness.close();
		}

		void run(Awaitable<void> scenario)
		{
			auto result =
				boost::asio::co_spawn(client.io, execute(*this, std::move(scenario)), boost::asio::use_future);
			client.io.run();
			EXPECT_NO_THROW(result.get());
			if (finished.valid())
			{
				EXPECT_NO_THROW(static_cast<void>(finished.get()));
			}
			EXPECT_EQ(service->used(), 0U);
		}
	};

	class Http2DrainTlsTest : public testing::Test
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
} // namespace

TEST(Http2DrainControlTest, EmptyGracefulStopFinishesBeforeTheNextOwnerLoopMarker)
{
	RunningServer running;
	const auto deadline = Clock::now() + kTestTimeout;
	ASSERT_TRUE(running.start(deadline));
	auto observed = std::make_shared<std::promise<bool>>();
	auto future = observed->get_future();
	boost::asio::post(running.server.ioContext(),
					  [&running, observed]
					  {
						  running.server.gracefulStop();
						  boost::asio::post(running.server.ioContext(),
											[&running, observed]
											{
												observed->set_value(running.server.isRunning());
											});
					  });
	ASSERT_EQ(future.wait_until(deadline), std::future_status::ready);
	EXPECT_FALSE(future.get()) << "空连接排空不应等待配置的强关期限";
	running.server.stop();
	EXPECT_TRUE(running.stopped(deadline));
}

TEST(Http2DrainControlTest, CrossThreadDrainIsIdempotentAndUnstartedCallsHaveNoEffect)
{
	RunningServer running(false, 3);
	running.server.gracefulStop();
	running.server.gracefulStop();
	running.server.setShutdownTimeout(60);
	const auto deadline = Clock::now() + kTestTimeout;
	ASSERT_TRUE(running.start(deadline));
	std::array<std::thread, 4> callers;
	for (auto& caller : callers)
	{
		caller = std::thread(
			[&running]
			{
				running.server.gracefulStop();
				running.server.gracefulStop();
			});
	}
	for (auto& caller : callers)
	{
		caller.join();
	}
	EXPECT_TRUE(running.stopped(deadline));
	running.server.stop();
	running.server.gracefulStop();
	EXPECT_FALSE(running.server.isRunning());
}

TEST(Http2DrainControlTest, GracefulStopLetsAnActivePeriodicCallbackFinishItsFiniteSleep)
{
	RunningServer running;
	running.server.setShutdownTimeout(60);
	std::atomic<size_t> calls {0};
	std::atomic<bool> completed {false};
	running.server.addPeriodicTask("drain-cleanup",
								   std::chrono::milliseconds {1},
								   [&]() -> Awaitable<void>
								   {
									   calls.fetch_add(1);
									   running.server.gracefulStop();
									   co_await hical::sleep(std::chrono::milliseconds {25});
									   completed.store(true);
								   });
	const auto deadline = Clock::now() + kTestTimeout;
	ASSERT_TRUE(running.start(deadline));
	EXPECT_TRUE(running.stopped(deadline));
	EXPECT_EQ(calls.load(), 1U);
	EXPECT_TRUE(completed.load());
}

TEST(Http2DrainControlTest, ShutdownTimeoutRejectsInvalidValuesAndAcceptsImmediateOrLargeDeadlines)
{
	HttpServer server;
	EXPECT_THROW(server.setShutdownTimeout(-1), std::invalid_argument);
	EXPECT_THROW(server.setShutdownTimeout(std::numeric_limits<double>::infinity()), std::invalid_argument);
	EXPECT_THROW(server.setShutdownTimeout(std::numeric_limits<double>::quiet_NaN()), std::invalid_argument);
	EXPECT_NO_THROW(server.setShutdownTimeout(0));
	EXPECT_NO_THROW(server.setShutdownTimeout(std::numeric_limits<double>::max()));
}

TEST_F(Http2DrainTlsTest, AcceptedStreamTrailersStillCompleteAfterGoaway)
{
	RunningServer running(true);
	const auto deadline = Clock::now() + kTestTimeout;
	auto entered = std::make_shared<std::promise<void>>();
	auto enteredFuture = entered->get_future();
	running.server.use(
		[entered](HttpRequest&) -> SyncMiddlewareResult
		{
			entered->set_value();
			return std::nullopt;
		});
	running.server.router().post("/work",
								 [](const HttpRequest&)
								 {
									 return HttpResponse::ok("done");
								 });
	ASSERT_TRUE(running.start(deadline));
	Client client;
	Peer peer;
	auto scenario = [](Client& client,
					   Peer& peer,
					   RunningServer& running,
					   std::future<void>& entered,
					   TimePoint deadline) -> Awaitable<void>
	{
		co_await client.connect(running.server.port(), peer, deadline);
		auto request = peer.codec.submitRequest(kRequest, false);
		EXPECT_FALSE(request.error);
		co_await client.flush(peer, deadline);
		if (entered.wait_until(deadline) != std::future_status::ready)
		{
			throw std::runtime_error("排空测试请求未进入中间件");
		}
		running.server.gracefulStop();
		while (!peer.goaway)
		{
			co_await client.read(peer, deadline);
		}
		EXPECT_EQ(peer.lastAccepted, request.streamId);
		const std::array<Codec::Header, 1> trailers {{{"x-finish", "done"}}};
		EXPECT_FALSE(peer.codec.submitTrailers(request.streamId, trailers));
		co_await client.flush(peer, deadline);
		while (!peer.ended.contains(request.streamId) && !peer.closed.contains(request.streamId))
		{
			co_await client.read(peer, deadline);
		}
		EXPECT_TRUE(peer.ended.contains(request.streamId));
		EXPECT_EQ(peer.status[request.streamId], 200);
		EXPECT_EQ(peer.body[request.streamId], "done");
	};
	client.run(scenario(client, peer, running, enteredFuture, deadline));
	EXPECT_TRUE(running.stopped(deadline));
}

TEST_F(Http2DrainTlsTest, IdleConnectionCompletesGoawayAndCancelsItsLongShutdownDeadline)
{
	RunningServer running(true, 3);
	running.server.setShutdownTimeout(std::numeric_limits<double>::max());
	const auto deadline = Clock::now() + kTestTimeout;
	ASSERT_TRUE(running.start(deadline));
	Client client;
	Peer peer;
	auto scenario = [](Client& client, Peer& peer, RunningServer& running, TimePoint deadline) -> Awaitable<void>
	{
		co_await client.connect(running.server.port(), peer, deadline);
		EXPECT_EQ(running.server.activeConnectionCount(), 1U);
		running.server.gracefulStop();
		running.server.gracefulStop();
		while (!peer.goaway)
		{
			co_await client.read(peer, deadline);
		}
		EXPECT_EQ(peer.lastAccepted, 0);
	};
	client.run(scenario(client, peer, running, deadline));
	EXPECT_TRUE(running.stopped(deadline));
	EXPECT_EQ(running.server.activeConnectionCount(), 0U);
	EXPECT_EQ(running.server.controlledConnectionCount(), 0U);
}

TEST_F(Http2DrainTlsTest, WindowBlocked400404And405AreIncludedInGoawayAndFinishBeforeClose)
{
	RunningServer running(true);
	running.server.setShutdownTimeout(60);
	running.server.router().get("/get-only",
								[](const HttpRequest&)
								{
									return HttpResponse::ok("unused");
								});
	const auto deadline = Clock::now() + kTestTimeout;
	ASSERT_TRUE(running.start(deadline));
	Client client;
	Peer peer;
	auto scenario = [](Client& client, Peer& peer, RunningServer& running, TimePoint deadline) -> Awaitable<void>
	{
		const std::array<Codec::Setting, 1> blocked {{{Http2Setting::hInitialWindowSize, 0}}};
		EXPECT_FALSE(peer.codec.submitSettings(blocked));
		co_await client.connect(running.server.port(), peer, deadline);
		const std::array<Codec::Header, 4> missing {
			{{":method", "GET"}, {":scheme", "https"}, {":authority", "localhost"}, {":path", "/missing"}}};
		const std::array<Codec::Header, 4> wrongMethod {
			{{":method", "POST"}, {":scheme", "https"}, {":authority", "localhost"}, {":path", "/get-only"}}};
		auto first = peer.codec.submitRequest(missing, true);
		auto second = peer.codec.submitRequest(wrongMethod, true);
		std::string deepPath;
		for (size_t index = 0; index <= Router::kMaxPathSegments; ++index)
		{
			deepPath.append("/segment");
		}
		const std::array<Codec::Header, 4> invalidPath {
			{{":method", "GET"}, {":scheme", "https"}, {":authority", "localhost"}, {":path", deepPath}}};
		auto third = peer.codec.submitRequest(invalidPath, true);
		EXPECT_FALSE(first.error);
		EXPECT_FALSE(second.error);
		EXPECT_FALSE(third.error);
		co_await client.flush(peer, deadline);
		while (peer.status.size() != 3)
		{
			co_await client.read(peer, deadline);
		}
		EXPECT_EQ(peer.status[first.streamId], 404);
		EXPECT_EQ(peer.status[second.streamId], 405);
		EXPECT_EQ(peer.status[third.streamId], 400);
		running.server.gracefulStop();
		while (!peer.goaway)
		{
			co_await client.read(peer, deadline);
		}
		EXPECT_EQ(peer.lastAccepted, third.streamId);
		EXPECT_TRUE(peer.ended.empty());
		EXPECT_TRUE(running.server.isRunning());
		const std::array<Codec::Setting, 1> opened {
			{{Http2Setting::hInitialWindowSize, Http2Options::kDefaultWindowSize}}};
		EXPECT_FALSE(peer.codec.submitSettings(opened));
		co_await client.flush(peer, deadline);
		while (!peer.ended.contains(first.streamId) || !peer.ended.contains(second.streamId)
			   || !peer.ended.contains(third.streamId))
		{
			co_await client.read(peer, deadline);
		}
		EXPECT_FALSE(peer.body[first.streamId].empty());
		EXPECT_FALSE(peer.body[second.streamId].empty());
		EXPECT_FALSE(peer.body[third.streamId].empty());
	};
	client.run(scenario(client, peer, running, deadline));
	EXPECT_TRUE(running.stopped(deadline));
}

TEST_F(Http2DrainTlsTest, AcceptedHandlerNaturallyCompletesAfterGoawayAndFiniteSleep)
{
	RunningServer running(true);
	running.server.setShutdownTimeout(60);
	auto release = std::make_shared<boost::asio::steady_timer>(running.server.ioContext());
	release->expires_at(TimePoint::max());
	auto entered = std::make_shared<std::promise<void>>();
	auto enteredFuture = entered->get_future();
	std::atomic<bool> completed {false};
	running.server.router().post("/work",
								 [release, entered, &completed](const HttpRequest&) -> Awaitable<HttpResponse>
								 {
									 entered->set_value();
									 boost::system::error_code ignored;
									 co_await release->async_wait(
										 boost::asio::redirect_error(boost::asio::use_awaitable, ignored));
									 co_await hical::sleep(std::chrono::milliseconds {25});
									 completed.store(true);
									 co_return HttpResponse::ok("finished");
								 });
	const auto deadline = Clock::now() + kTestTimeout;
	ASSERT_TRUE(running.start(deadline));
	Client client;
	Peer peer;
	auto scenario = [&](Client& client, Peer& peer) -> Awaitable<void>
	{
		co_await client.connect(running.server.port(), peer, deadline);
		auto request = peer.codec.submitRequest(kRequest, true);
		EXPECT_FALSE(request.error);
		co_await client.flush(peer, deadline);
		if (enteredFuture.wait_until(deadline) != std::future_status::ready)
		{
			throw std::runtime_error("排空测试 handler 未启动");
		}
		running.server.gracefulStop();
		while (!peer.goaway)
		{
			co_await client.read(peer, deadline);
		}
		EXPECT_EQ(peer.lastAccepted, request.streamId);
		EXPECT_FALSE(completed.load());
		boost::asio::post(running.server.ioContext(),
						  [release]
						  {
							  release->cancel();
						  });
		while (!peer.ended.contains(request.streamId))
		{
			co_await client.read(peer, deadline);
		}
		EXPECT_EQ(peer.status[request.streamId], 200);
		EXPECT_EQ(peer.body[request.streamId], "finished");
	};
	client.run(scenario(client, peer));
	// 即使前面的断言失败，也要放开业务等待，免得协程一直挂着。
	boost::asio::post(running.server.ioContext(),
					  [release]
					  {
						  release->cancel();
					  });
	EXPECT_TRUE(running.stopped(deadline));
	EXPECT_TRUE(completed.load());
}

TEST_F(Http2DrainTlsTest, StrongStopSupersedesDrainWhileAResponseWaitsForWindow)
{
	RunningServer running(true);
	running.server.setShutdownTimeout(60);
	running.server.router().post("/work",
								 [](const HttpRequest&)
								 {
									 return HttpResponse::ok("blocked response");
								 });
	const auto deadline = Clock::now() + kTestTimeout;
	ASSERT_TRUE(running.start(deadline));
	Client client;
	Peer peer;
	auto scenario = [](Client& client, Peer& peer, RunningServer& running, TimePoint deadline) -> Awaitable<void>
	{
		const std::array<Codec::Setting, 1> blocked {{{Http2Setting::hInitialWindowSize, 0}}};
		EXPECT_FALSE(peer.codec.submitSettings(blocked));
		co_await client.connect(running.server.port(), peer, deadline);
		auto request = peer.codec.submitRequest(kRequest, true);
		EXPECT_FALSE(request.error);
		co_await client.flush(peer, deadline);
		while (!peer.status.contains(request.streamId))
		{
			co_await client.read(peer, deadline);
		}
		running.server.gracefulStop();
		while (!peer.goaway)
		{
			co_await client.read(peer, deadline);
		}
		running.server.gracefulStop();
		running.server.stop();
		running.server.stop();
		running.server.gracefulStop();
		EXPECT_FALSE(peer.ended.contains(request.streamId));
	};
	client.run(scenario(client, peer, running, deadline));
	EXPECT_TRUE(running.stopped(deadline));
	EXPECT_EQ(running.server.activeConnectionCount(), 0U);
}

TEST_F(Http2DrainTlsTest, HandshakeCompletingAfterDrainDoesNotStartAnApplicationStream)
{
	RunningServer running(true);
	running.server.setShutdownTimeout(60);
	std::atomic<size_t> calls {0};
	running.server.router().post("/work",
								 [&calls](const HttpRequest&)
								 {
									 calls.fetch_add(1);
									 return HttpResponse::ok("unexpected");
								 });
	const auto deadline = Clock::now() + kTestTimeout;
	ASSERT_TRUE(running.start(deadline));
	Client client;
	auto scenario = [](Client& client, RunningServer& running, TimePoint deadline) -> Awaitable<void>
	{
		co_await client.socket.next_layer().async_connect(
			{boost::asio::ip::address_v4::loopback(), running.server.port()},
			boost::asio::cancel_at(deadline, boost::asio::use_awaitable));
		if (!waitUntil(deadline,
					   [&running]
					   {
						   return running.server.controlledConnectionCount() == 1;
					   }))
		{
			throw std::runtime_error("握手连接未登记到控制表");
		}
		running.server.gracefulStop();
		EXPECT_FALSE(
			SslContext::configureClientHttpProtocols(client.socket.native_handle(), HttpProtocolMode::hHttp2Only));
		boost::system::error_code error;
		co_await client.socket.async_handshake(
			boost::asio::ssl::stream_base::client,
			boost::asio::cancel_at(deadline, boost::asio::redirect_error(boost::asio::use_awaitable, error)));
		if (!error)
		{
			std::array<char, 64> bytes;
			co_await client.socket.async_read_some(
				boost::asio::buffer(bytes),
				boost::asio::cancel_at(deadline, boost::asio::redirect_error(boost::asio::use_awaitable, error)));
		}
		EXPECT_TRUE(error);
	};
	client.run(scenario(client, running, deadline));
	EXPECT_TRUE(running.stopped(deadline));
	EXPECT_EQ(calls.load(), 0U);
	EXPECT_EQ(running.server.activeConnectionCount(), 0U);
}

TEST_F(Http2DrainTlsTest, ZeroShutdownDeadlineClosesAnIdleDisabledStalledHandshake)
{
	RunningServer running(true);
	running.server.setShutdownTimeout(0);
	const auto deadline = Clock::now() + kTestTimeout;
	ASSERT_TRUE(running.start(deadline));
	Client client;
	auto scenario = [](Client& client, RunningServer& running, TimePoint deadline) -> Awaitable<void>
	{
		co_await client.socket.next_layer().async_connect(
			{boost::asio::ip::address_v4::loopback(), running.server.port()},
			boost::asio::cancel_at(deadline, boost::asio::use_awaitable));
		if (!waitUntil(deadline,
					   [&running]
					   {
						   return running.server.controlledConnectionCount() == 1;
					   }))
		{
			throw std::runtime_error("停滞握手未登记到控制表");
		}
		running.server.gracefulStop();
	};
	client.run(scenario(client, running, deadline));
	EXPECT_TRUE(running.stopped(deadline));
	EXPECT_EQ(running.server.activeConnectionCount(), 0U);
	EXPECT_EQ(running.server.controlledConnectionCount(), 0U);
}

TEST_F(Http2DrainTlsTest, ANewStreamEncodedBeforeGoawayCannotStartAfterDrain)
{
	RunningServer running(true);
	const auto deadline = Clock::now() + kTestTimeout;
	auto entered = std::make_shared<std::promise<void>>();
	auto enteredFuture = entered->get_future();
	std::atomic<size_t> newCalls {0};
	running.server.use(
		[entered](HttpRequest& request) -> SyncMiddlewareResult
		{
			if (request.path() == "/work")
			{
				entered->set_value();
			}
			return std::nullopt;
		});
	running.server.router().post("/work",
								 [](const HttpRequest&)
								 {
									 return HttpResponse::ok("accepted");
								 });
	running.server.router().post("/new",
								 [&newCalls](const HttpRequest&)
								 {
									 newCalls.fetch_add(1);
									 return HttpResponse::ok("unexpected");
								 });
	ASSERT_TRUE(running.start(deadline));
	Client client;
	Peer peer;
	auto scenario = [&](Client& client, Peer& peer) -> Awaitable<void>
	{
		co_await client.connect(running.server.port(), peer, deadline);
		auto accepted = peer.codec.submitRequest(kRequest, false);
		EXPECT_FALSE(accepted.error);
		co_await client.flush(peer, deadline);
		if (enteredFuture.wait_until(deadline) != std::future_status::ready)
		{
			throw std::runtime_error("已有请求未进入 body 门");
		}
		const std::array<Codec::Header, 4> newHeaders {
			{{":method", "POST"}, {":scheme", "https"}, {":authority", "localhost"}, {":path", "/new"}}};
		auto refused = peer.codec.submitRequest(newHeaders, true);
		EXPECT_FALSE(refused.error);
		auto delayed = peer.codec.memSend();
		EXPECT_FALSE(delayed.error);
		EXPECT_FALSE(delayed.bytes.empty());
		running.server.gracefulStop();
		while (!peer.goaway)
		{
			co_await client.read(peer, deadline);
		}
		EXPECT_EQ(peer.lastAccepted, accepted.streamId);
		// 用提前保存的 HEADERS 在 GOAWAY 后发送新流，检查服务端是否拒绝。
		co_await boost::asio::async_write(client.socket,
										  boost::asio::buffer(delayed.bytes),
										  boost::asio::cancel_at(deadline, boost::asio::use_awaitable));
		const std::array<Codec::Header, 1> trailers {{{"x-finish", "done"}}};
		EXPECT_FALSE(peer.codec.submitTrailers(accepted.streamId, trailers));
		co_await client.flush(peer, deadline);
		while (!peer.ended.contains(accepted.streamId))
		{
			co_await client.read(peer, deadline);
		}
		EXPECT_EQ(peer.status[accepted.streamId], 200);
		EXPECT_EQ(peer.body[accepted.streamId], "accepted");
	};
	client.run(scenario(client, peer));
	EXPECT_TRUE(running.stopped(deadline));
	EXPECT_EQ(newCalls.load(), 0U);
}

TEST_F(Http2DrainTlsTest, APartialInitialHeaderCompletedAfterDrainCannotExecuteBeyondTheGoawayLimit)
{
	PartialHeaderHarness harness;
	harness.options.maxHeaderBytes = 64 * 1024;
	Http2Options peerOptions = harness.options;
	peerOptions.initialStreamWindow = Http2Options::kMaxWindowSize;
	peerOptions.initialConnectionWindow = Http2Options::kMaxWindowSize;
	Peer peer(peerOptions);
	constexpr size_t kPayloadBytes = 1024 * 1024;
	size_t sideEffects = 0;
	harness.router.post("/work",
						[](const HttpRequest&)
						{
							return HttpResponse::ok(std::string(kPayloadBytes, 's'));
						});
	harness.router.post("/effect",
						[&sideEffects](const HttpRequest&)
						{
							++sideEffects;
							return HttpResponse::ok("side effect");
						});
	const auto deadline = Clock::now() + kTestTimeout;
	auto scenario = [&]() -> Awaitable<void>
	{
		co_await harness.start(peer, deadline);
		auto first = peer.codec.submitRequest(kRequest, true);
		EXPECT_FALSE(first.error);
		EXPECT_EQ(first.streamId, 1);
		co_await harness.client.flush(peer, deadline);
		while (!peer.status.contains(first.streamId))
		{
			co_await harness.client.read(peer, deadline);
		}
		EXPECT_EQ(peer.status[first.streamId], 200);
		EXPECT_FALSE(peer.ended.contains(first.streamId));
		// 响应头返回时应用 guard 已释放，剩余发送工作不再占应用配额。
		const auto baseline = harness.service->used();
		EXPECT_EQ(baseline, 0U);
		while (harness.client.socket.next_layer().available() == 0)
		{
			if (Clock::now() >= deadline)
			{
				throw std::runtime_error("大响应尚未进入被对端慢读阻塞的 TLS 写入");
			}
			co_await boost::asio::post(boost::asio::use_awaitable);
		}
		const std::string value(32 * 1024, 'x');
		const std::array<Codec::Header, 5> effectHeaders {{{":method", "POST"},
														   {":scheme", "https"},
														   {":authority", "localhost"},
														   {":path", "/effect"},
														   {"x-long", value}}};
		auto pending = peer.codec.submitRequest(effectHeaders, true);
		EXPECT_FALSE(pending.error);
		EXPECT_EQ(pending.streamId, 3);
		std::string bytes;
		for (;;)
		{
			auto part = peer.codec.memSend();
			if (part.error)
			{
				throw std::runtime_error(part.error.message);
			}
			if (part.bytes.empty())
			{
				break;
			}
			bytes.append(part.bytes);
		}
		constexpr size_t kFrameHeaderBytes = 9;
		if (bytes.size() < kFrameHeaderBytes)
		{
			throw std::runtime_error("测试请求缺少 HEADERS 帧");
		}
		const auto payload = (static_cast<size_t>(static_cast<unsigned char>(bytes[0])) << 16)
							 | (static_cast<size_t>(static_cast<unsigned char>(bytes[1])) << 8)
							 | static_cast<unsigned char>(bytes[2]);
		const auto firstFrame = kFrameHeaderBytes + payload;
		if (firstFrame >= bytes.size() || bytes[3] != 1 || (bytes[4] & 0x04) != 0)
		{
			throw std::runtime_error("测试初始头未拆成 HEADERS 与 CONTINUATION");
		}
		co_await boost::asio::async_write(harness.client.socket,
										  boost::asio::buffer(bytes.data(), firstFrame),
										  boost::asio::cancel_at(deadline, boost::asio::use_awaitable));
		co_await harness.waitForUsage(baseline + 1, deadline);
		EXPECT_EQ(sideEffects, 0U);
		std::promise<void> drained;
		auto drainReady = drained.get_future();
		boost::asio::post(harness.client.io,
						  [&]
						  {
							  harness.session->drain();
							  drained.set_value();
						  });
		while (drainReady.wait_for(std::chrono::seconds {0}) != std::future_status::ready)
		{
			co_await boost::asio::post(boost::asio::use_awaitable);
		}
		co_await boost::asio::async_write(harness.client.socket,
										  boost::asio::buffer(bytes.data() + firstFrame, bytes.size() - firstFrame),
										  boost::asio::cancel_at(deadline, boost::asio::use_awaitable));
		// 配额归还说明补齐的头块已处理；若错误接纳了请求，排队的业务也会在此之前执行。
		co_await harness.waitForUsage(baseline, deadline);
		EXPECT_EQ(sideEffects, 0U) << "GOAWAY 的 last-stream-id 之外不能再发生业务副作用";
		// 检查业务副作用后再放开慢读，避免 GOAWAY 先发出后由协议库直接拒绝新流。
		harness.client.socket.next_layer().set_option(boost::asio::socket_base::receive_buffer_size(kPayloadBytes));
		while (!peer.goaway)
		{
			co_await harness.client.read(peer, deadline);
		}
		EXPECT_EQ(peer.lastAccepted, first.streamId);
	};
	harness.run(scenario());
}
