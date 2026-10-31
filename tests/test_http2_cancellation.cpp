/**
 * @file test_http2_cancellation.cpp
 * @brief HTTP/2 取消后的请求、事务和输出清理测试
 */

#include "core/http2/Http2ServerSession.h"
#include "core/Middleware.h"
#include "core/RequestDispatch.h"
#include "core/Router.h"
#include "core/TimerService.h"
#ifdef HICAL_HAS_DATABASE
	#include "db/DbConnectionPool.h"
	#include "db/DbMiddleware.h"
#endif
#include <boost/asio/cancel_at.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/use_future.hpp>
#include <gtest/gtest.h>
#include <array>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <future>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>

using namespace hical;
using namespace hical::detail;
using namespace boost::asio::experimental::awaitable_operators;

namespace
{
	using Codec = Http2Codec;
	using Clock = Http2Stream::Clock;
	using TimePoint = Clock::time_point;
	using Timer = boost::asio::steady_timer;
	constexpr auto kTestTimeout = std::chrono::seconds {10};
	constexpr auto kFiniteSleep = std::chrono::milliseconds {1};
	constexpr std::array<Codec::Header, 4> kRequest {
		{{":method", "POST"}, {":scheme", "https"}, {":authority", "localhost"}, {":path", "/work"}}};

	struct Lifetime
	{
		explicit Lifetime(boost::asio::any_io_executor executor)
			: enteredWake(executor, Clock::now() + kTestTimeout), destroyedWake(executor, Clock::now() + kTestTimeout)
		{
		}

		Timer enteredWake;
		Timer destroyedWake;
		bool entered {false};
		size_t destroyed {0};

		void enter()
		{
			entered = true;
			enteredWake.cancel();
		}

		Awaitable<void> waitEntered()
		{
			if (!entered)
			{
				boost::system::error_code ignored;
				co_await enteredWake.async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, ignored));
			}
			EXPECT_TRUE(entered);
		}

		Awaitable<void> waitDestroyed()
		{
			if (destroyed == 0)
			{
				boost::system::error_code ignored;
				co_await destroyedWake.async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, ignored));
			}
			EXPECT_EQ(destroyed, 1U);
		}
	};

	struct RequestProbe
	{
		explicit RequestProbe(std::shared_ptr<Lifetime> owner) : lifetime(std::move(owner))
		{
		}

		~RequestProbe()
		{
			++lifetime->destroyed;
			lifetime->destroyedWake.cancel();
		}

		std::shared_ptr<Lifetime> lifetime;
	};

	class Http2DispatchCancellationTest : public testing::Test
	{
	protected:
		boost::asio::io_context io;
		boost::asio::ssl::context tls {boost::asio::ssl::context::tls_server};
		Http2Connection::TlsSocket socket {io, tls};
		Router router;
		MiddlewarePipeline middleware;
		Http2Options options;
		std::shared_ptr<Http2ServiceBudget> service = std::make_shared<Http2ServiceBudget>(64);
		Http2Budget budget {Http2Budget::Role::hServer, options, service.get()};
		std::shared_ptr<Http2ServerSession> session;

		void prepare()
		{
			middleware.build(
				[](HttpRequest& req)
				{
					return runInternalDispatch(req);
				});
			session = Http2ServerSession::create(socket, options, {router, middleware, {}, {}, service, {}});
		}

		std::shared_ptr<Http2Stream> request(bool complete)
		{
			Http2Stream::Config config;
			config.streamId = 1;
			config.options = options;
			auto stream = Http2Stream::create(io.get_executor(), config, budget);
			for (auto field : kRequest)
			{
				EXPECT_FALSE(stream->initialHeaders().append(field.name, field.value));
			}
			EXPECT_FALSE(stream->initialHeaders().seal());
			EXPECT_FALSE(Http2ServerSession::mapRequest(*stream, {}));
			EXPECT_FALSE(stream->appendBody("body"));
			if (complete)
			{
				EXPECT_FALSE(stream->completeBody());
			}
			return stream;
		}

		void observe(const std::shared_ptr<Lifetime>& lifetime)
		{
			middleware.use(
				[lifetime](HttpRequest& req) -> SyncMiddlewareResult
				{
					req.setAttribute("lifetime", std::make_shared<RequestProbe>(lifetime));
					return std::nullopt;
				});
		}
	};

	struct Peer : Codec::Listener
	{
		Codec codec {Codec::Role::hClient, {}, this};
		bool settings {false};
		size_t pingAcks {0};
		std::map<int32_t, uint16_t> status;
		std::map<int32_t, Http2Code> closed;
		std::set<int32_t> ended;

		void onSettings(bool ack) override
		{
			settings = settings || !ack;
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

		void onFrameReceived(const Codec::Frame& frame) override
		{
			pingAcks += frame.type == Codec::FrameType::hPing && (frame.flags & Codec::kAck) != 0;
		}
	};

	struct Harness
	{
		static boost::asio::ssl::context makeServerContext()
		{
			boost::asio::ssl::context result(boost::asio::ssl::context::tls_server);
			result.use_certificate_chain_file("test_server.crt");
			result.use_private_key_file("test_server.key", boost::asio::ssl::context::pem);
			return result;
		}

		boost::asio::io_context io;
		boost::asio::ssl::context serverContext {makeServerContext()};
		boost::asio::ssl::context clientContext {boost::asio::ssl::context::tls_client};
		boost::asio::ip::tcp::acceptor acceptor {io, {boost::asio::ip::address_v4::loopback(), 0}};
		Http2Connection::TlsSocket server {io, serverContext};
		Http2Connection::TlsSocket client {io, clientContext};
		Timer watchdog {io};
		TimerService timers {io.get_executor()};
		Router router;
		MiddlewarePipeline middleware;
		Http2Options options;
		std::shared_ptr<Http2ServiceBudget> service = std::make_shared<Http2ServiceBudget>(64);
		std::shared_ptr<Http2ServerSession> session;
		std::future<NetworkError> finished;
		bool timedOut {false};

		Harness()
		{
			client.set_verify_mode(boost::asio::ssl::verify_none);
		}

		void close()
		{
			boost::system::error_code ignored;
			server.next_layer().close(ignored);
			client.next_layer().close(ignored);
		}

		Awaitable<void> write(std::string_view bytes)
		{
			co_await boost::asio::async_write(client, boost::asio::buffer(bytes), boost::asio::use_awaitable);
		}

		Awaitable<void> flush(Peer& peer)
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
				co_await write(batch.bytes);
			}
		}

		Awaitable<bool> read(Peer& peer, TimePoint deadline = TimePoint::max())
		{
			std::array<char, 16384> buffer;
			boost::system::error_code error;
			const auto size = co_await client.async_read_some(
				boost::asio::buffer(buffer),
				boost::asio::cancel_at(deadline, boost::asio::redirect_error(boost::asio::use_awaitable, error)));
			if (error || size == 0)
			{
				if (deadline == TimePoint::max())
				{
					throw std::runtime_error("TLS 取消测试对端提前关闭");
				}
				co_return false;
			}
			auto result = peer.codec.memRecv({buffer.data(), size});
			EXPECT_FALSE(result.error);
			EXPECT_EQ(result.consumed, size);
			co_return !result.error;
		}

		Awaitable<void> start(Peer& peer)
		{
			co_await (acceptor.async_accept(server.next_layer(), boost::asio::use_awaitable)
					  && client.next_layer().async_connect(acceptor.local_endpoint(), boost::asio::use_awaitable));
			co_await (server.async_handshake(boost::asio::ssl::stream_base::server, boost::asio::use_awaitable)
					  && client.async_handshake(boost::asio::ssl::stream_base::client, boost::asio::use_awaitable));
			middleware.build(
				[](HttpRequest& req)
				{
					return runInternalDispatch(req);
				});
			session = Http2ServerSession::create(server, options, {router, middleware, {}, {}, service, {}});
			finished = boost::asio::co_spawn(io, session->run(), boost::asio::use_future);
			co_await flush(peer);
			while (!peer.settings)
			{
				if (!co_await read(peer))
				{
					throw std::runtime_error("TLS 协议准备失败");
				}
			}
			co_await flush(peer);
		}

		static Awaitable<void> execute(Harness& harness, Awaitable<void> scenario)
		{
			try
			{
				co_await std::move(scenario);
			}
			catch (...)
			{
				harness.close();
				harness.watchdog.cancel();
				throw;
			}
			harness.close();
			harness.watchdog.cancel();
		}

		void run(Awaitable<void> scenario)
		{
			timers.bindToCurrentThread();
			watchdog.expires_after(kTestTimeout);
			watchdog.async_wait(
				[this](const boost::system::error_code& error)
				{
					if (!error)
					{
						timedOut = true;
						close();
					}
				});
			auto result = boost::asio::co_spawn(io, execute(*this, std::move(scenario)), boost::asio::use_future);
			io.run();
			timers.unbindFromCurrentThread();
			EXPECT_FALSE(timedOut);
			EXPECT_NO_THROW(result.get());
			if (finished.valid())
			{
				EXPECT_NO_THROW(static_cast<void>(finished.get()));
			}
			EXPECT_EQ(service->used(), 0U);
			EXPECT_EQ(timers.pendingCount(), 0U);
		}
	};

	class Http2CancellationTlsTest : public testing::Test
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

TEST_F(Http2DispatchCancellationTest, ResetAtBodyGateKeepsRequestUntilOuterMiddlewareUnwinds)
{
	auto lifetime = std::make_shared<Lifetime>(io.get_executor());
	observe(lifetime);
	bool outerReturned = false;
	bool bodyCalled = false;
	middleware.use(
		[&](HttpRequest& req, MiddlewareNext next) -> Awaitable<HttpResponse>
		{
			auto response = co_await next(req);
			outerReturned = true;
			co_return response;
		});
	middleware.useBody(
		[&](HttpRequest&) -> SyncMiddlewareResult
		{
			bodyCalled = true;
			return std::nullopt;
		});
	router.post("/work",
				[](const HttpRequest&)
				{
					return HttpResponse::serverError();
				});
	prepare();
	auto stream = request(false);
	auto response =
		boost::asio::co_spawn(io, session->dispatch(stream, Clock::now() + kTestTimeout), boost::asio::use_future);
	io.poll();
	EXPECT_EQ(service->used(), 1U);
	stream->reset();
	stream->reset();
	EXPECT_EQ(budget.usage().protocolStreams, 0U);
	EXPECT_EQ(service->used(), 1U);
	EXPECT_EQ(budget.usage().receiveBytes, 4U);
	EXPECT_EQ(lifetime->destroyed, 0U);
	EXPECT_EQ(response.wait_for(std::chrono::seconds {0}), std::future_status::timeout);
	std::weak_ptr<Http2Stream> weak = stream;
	stream.reset();
	io.run();
	EXPECT_EQ(response.get().statusCode(), HttpStatusCode::hBadRequest);
	EXPECT_TRUE(outerReturned);
	EXPECT_FALSE(bodyCalled);
	EXPECT_TRUE(weak.expired());
	EXPECT_EQ(lifetime->destroyed, 1U);
	EXPECT_EQ(service->used(), 0U);
	EXPECT_EQ(budget.usage().receiveBytes, 0U);
}

TEST_F(Http2DispatchCancellationTest, FiniteSleepSurvivesResetAndReleasesPayloadExactlyOnce)
{
	TimerService timers(io.get_executor());
	timers.bindToCurrentThread();
	auto lifetime = std::make_shared<Lifetime>(io.get_executor());
	observe(lifetime);
	bool returned = false;
	router.post("/work",
				[&](const HttpRequest& req) -> Awaitable<HttpResponse>
				{
					lifetime->enter();
					co_await hical::sleep(kFiniteSleep);
					EXPECT_EQ(req.body(), "body");
					returned = true;
					co_return HttpResponse::ok("late");
				});
	prepare();
	auto stream = request(true);
	auto response =
		boost::asio::co_spawn(io, session->dispatch(stream, Clock::now() + kTestTimeout), boost::asio::use_future);
	io.poll_one();
	EXPECT_TRUE(lifetime->entered);
	EXPECT_EQ(timers.pendingCount(), 1U);
	stream->reset();
	stream->reset();
	EXPECT_FALSE(stream->completeBody());
	EXPECT_FALSE(stream->protocolOpen());
	EXPECT_EQ(stream->error().code, ErrorCode::hHttp2StreamError);
	EXPECT_EQ(service->used(), 1U);
	EXPECT_EQ(budget.usage().receiveBytes, 4U);
	stream.reset();
	io.run();
	EXPECT_EQ(response.get().body(), "late");
	EXPECT_TRUE(returned);
	EXPECT_EQ(lifetime->destroyed, 1U);
	EXPECT_EQ(service->used(), 0U);
	EXPECT_EQ(budget.usage().receiveBytes, 0U);
	EXPECT_EQ(timers.pendingCount(), 0U);
	timers.unbindFromCurrentThread();
}

TEST_F(Http2CancellationTlsTest, ZeroWindowOutputDeadlineIgnoresPingAndOnlyResetsItsStream)
{
	Harness harness;
	harness.options.writeTimeout = std::chrono::milliseconds {20};
	harness.router.post("/work",
						[](const HttpRequest&)
						{
							return HttpResponse::ok(std::string(1024, 'x'));
						});
	harness.router.get("/empty",
					   [](const HttpRequest&)
					   {
						   return HttpResponse::ok();
					   });
	Peer peer;
	auto scenario = [](Harness& harness, Peer& peer) -> Awaitable<void>
	{
		co_await harness.start(peer);
		const std::array<Codec::Setting, 1> settings {{{Http2Setting::hInitialWindowSize, 0}}};
		EXPECT_FALSE(peer.codec.submitSettings(settings));
		auto request = peer.codec.submitRequest(kRequest);
		EXPECT_FALSE(request.error);
		co_await harness.flush(peer);
		while (!peer.status.contains(request.streamId))
		{
			EXPECT_TRUE(co_await harness.read(peer));
		}
		EXPECT_FALSE(peer.ended.contains(request.streamId));
		constexpr std::array<char, 17> kPing {0, 0, 8, 6};
		co_await harness.write(std::string_view(kPing.data(), kPing.size()));
		const auto deadline = Clock::now() + std::chrono::milliseconds {500};
		while (!peer.closed.contains(request.streamId) && Clock::now() < deadline)
		{
			if (!co_await harness.read(peer, deadline))
			{
				break;
			}
			co_await harness.write(std::string_view(kPing.data(), kPing.size()));
		}
		EXPECT_TRUE(peer.closed.contains(request.streamId)) << "流窗口为零时输出期限仍必须生效";
		if (!peer.closed.contains(request.streamId))
		{
			co_return;
		}
		EXPECT_EQ(peer.closed[request.streamId], Http2Code::hCancel);
		auto headers = kRequest;
		headers[0].value = "GET";
		headers[3].value = "/empty";
		auto neighbor = peer.codec.submitRequest(headers);
		EXPECT_FALSE(neighbor.error);
		co_await harness.flush(peer);
		while (!peer.ended.contains(neighbor.streamId))
		{
			EXPECT_TRUE(co_await harness.read(peer));
		}
		EXPECT_EQ(peer.status[neighbor.streamId], 200);
		EXPECT_GT(peer.pingAcks, 0U);
	};
	harness.run(scenario(harness, peer));
}

TEST_F(Http2CancellationTlsTest, PeerResetAtBodyGateUnwindsWithoutCallingHandler)
{
	Harness harness;
	auto lifetime = std::make_shared<Lifetime>(harness.io.get_executor());
	bool called = false;
	harness.middleware.use(
		[lifetime](HttpRequest& req) -> SyncMiddlewareResult
		{
			if (req.path() == "/work")
			{
				req.setAttribute("lifetime", std::make_shared<RequestProbe>(lifetime));
				lifetime->enter();
			}
			return std::nullopt;
		});
	harness.middleware.useBody(
		[](HttpRequest&) -> SyncMiddlewareResult
		{
			return std::nullopt;
		});
	harness.router.post("/work",
						[&](const HttpRequest&)
						{
							called = true;
							return HttpResponse::ok();
						});
	harness.router.get("/empty",
					   [](const HttpRequest&)
					   {
						   return HttpResponse::ok();
					   });
	Peer peer;
	auto scenario = [](Harness& harness, Peer& peer, std::shared_ptr<Lifetime> lifetime) -> Awaitable<void>
	{
		co_await harness.start(peer);
		auto request = peer.codec.submitRequest(kRequest, false);
		EXPECT_FALSE(request.error);
		co_await harness.flush(peer);
		co_await lifetime->waitEntered();
		EXPECT_EQ(harness.service->used(), 1U);
		EXPECT_FALSE(peer.codec.reset(request.streamId, Http2Code::hCancel));
		co_await harness.flush(peer);
		co_await lifetime->waitDestroyed();
		EXPECT_EQ(harness.service->used(), 0U);
		EXPECT_FALSE(peer.status.contains(request.streamId));
		auto headers = kRequest;
		headers[0].value = "GET";
		headers[3].value = "/empty";
		auto neighbor = peer.codec.submitRequest(headers);
		EXPECT_FALSE(neighbor.error);
		co_await harness.flush(peer);
		while (!peer.ended.contains(neighbor.streamId))
		{
			EXPECT_TRUE(co_await harness.read(peer));
		}
		EXPECT_EQ(peer.status[neighbor.streamId], 200);
	};
	harness.run(scenario(harness, peer, lifetime));
	EXPECT_FALSE(called);
}

TEST_F(Http2CancellationTlsTest, PeerNoErrorResetReleasesWindowBlockedResponseContext)
{
	Harness harness;
	auto lifetime = std::make_shared<Lifetime>(harness.io.get_executor());
	harness.middleware.use(
		[lifetime](HttpRequest& req) -> SyncMiddlewareResult
		{
			if (req.path() == "/work")
			{
				req.setAttribute("lifetime", std::make_shared<RequestProbe>(lifetime));
			}
			return std::nullopt;
		});
	harness.router.post("/work",
						[](const HttpRequest&)
						{
							return HttpResponse::ok(std::string(1024, 'x'));
						});
	harness.router.get("/empty",
					   [](const HttpRequest&)
					   {
						   return HttpResponse::ok();
					   });
	Peer peer;
	auto scenario = [](Harness& harness, Peer& peer, std::shared_ptr<Lifetime> lifetime) -> Awaitable<void>
	{
		co_await harness.start(peer);
		const std::array<Codec::Setting, 1> settings {{{Http2Setting::hInitialWindowSize, 0}}};
		EXPECT_FALSE(peer.codec.submitSettings(settings));
		auto request = peer.codec.submitRequest(kRequest);
		EXPECT_FALSE(request.error);
		co_await harness.flush(peer);
		while (!peer.status.contains(request.streamId))
		{
			EXPECT_TRUE(co_await harness.read(peer));
		}
		EXPECT_FALSE(peer.ended.contains(request.streamId));
		EXPECT_EQ(lifetime->destroyed, 0U);
		EXPECT_EQ(harness.service->used(), 0U);
		EXPECT_FALSE(peer.codec.reset(request.streamId, Http2Code::hNoError));
		co_await harness.flush(peer);
		co_await lifetime->waitDestroyed();
		auto headers = kRequest;
		headers[0].value = "GET";
		headers[3].value = "/empty";
		auto neighbor = peer.codec.submitRequest(headers);
		EXPECT_FALSE(neighbor.error);
		co_await harness.flush(peer);
		while (!peer.ended.contains(neighbor.streamId))
		{
			EXPECT_TRUE(co_await harness.read(peer));
		}
		EXPECT_EQ(peer.status[neighbor.streamId], 200);
	};
	harness.run(scenario(harness, peer, lifetime));
}

TEST_F(Http2CancellationTlsTest, ConnectionFailureJoinsApplicationBeforeItsFiniteSleepCleanup)
{
	Harness harness;
	auto lifetime = std::make_shared<Lifetime>(harness.io.get_executor());
	Timer release(harness.io, Clock::now() + kTestTimeout);
	bool returned = false;
	harness.middleware.use(
		[lifetime](HttpRequest& req) -> SyncMiddlewareResult
		{
			req.setAttribute("lifetime", std::make_shared<RequestProbe>(lifetime));
			return std::nullopt;
		});
	harness.router.post("/work",
						[&](const HttpRequest& req) -> Awaitable<HttpResponse>
						{
							lifetime->enter();
							boost::system::error_code ignored;
							co_await release.async_wait(
								boost::asio::redirect_error(boost::asio::use_awaitable, ignored));
							co_await hical::sleep(kFiniteSleep);
							EXPECT_EQ(req.target(), "/work");
							returned = true;
							co_return HttpResponse::ok("late");
						});
	Peer peer;
	auto scenario =
		[](Harness& harness, Peer& peer, std::shared_ptr<Lifetime> lifetime, Timer& release) -> Awaitable<void>
	{
		co_await harness.start(peer);
		EXPECT_FALSE(peer.codec.submitRequest(kRequest).error);
		co_await harness.flush(peer);
		co_await lifetime->waitEntered();
		boost::system::error_code ignored;
		harness.client.next_layer().close(ignored);
		while (harness.server.next_layer().is_open())
		{
			co_await boost::asio::post(boost::asio::use_awaitable);
		}
		EXPECT_EQ(harness.finished.wait_for(std::chrono::seconds {0}), std::future_status::timeout);
		EXPECT_EQ(harness.service->used(), 1U);
		EXPECT_EQ(lifetime->destroyed, 0U);
		std::weak_ptr<Http2ServerSession> weak = harness.session;
		harness.session.reset();
		EXPECT_FALSE(weak.expired());
		release.cancel();
		co_await lifetime->waitDestroyed();
	};
	harness.run(scenario(harness, peer, lifetime, release));
	EXPECT_TRUE(returned);
	EXPECT_EQ(lifetime->destroyed, 1U);
}

#ifdef HICAL_HAS_DATABASE
namespace
{
	class BlockingDbConnection : public db::DbConnection
	{
	public:
		explicit BlockingDbConnection(boost::asio::io_context& io, bool fail)
			: completion(io, Clock::now() + kTestTimeout), failQuery(fail)
		{
		}

		Awaitable<db::DbResult> query(std::string_view sql) override
		{
			return query(sql, {});
		}

		Awaitable<db::DbResult> query(std::string_view, std::span<const std::string>) override
		{
			queryEntered = true;
			boost::system::error_code ignored;
			co_await completion.async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, ignored));
			if (failQuery)
			{
				throw std::runtime_error("mock query failed");
			}
			co_return db::DbResult::fromRows({"value"}, {{"done"}});
		}

		Awaitable<db::DbResult> execute(std::string_view) override
		{
			co_return db::DbResult::fromDml(1, 0);
		}

		Awaitable<db::DbResult> execute(std::string_view sql, std::span<const std::string>) override
		{
			return execute(sql);
		}

		Awaitable<void> beginTransaction() override
		{
			transaction = true;
			co_return;
		}

		Awaitable<void> commit() override
		{
			++commits;
			transaction = false;
			co_return;
		}

		Awaitable<void> rollback() override
		{
			++rollbacks;
			transaction = false;
			co_return;
		}

		bool inTransaction() const override
		{
			return transaction;
		}

		bool isAlive() const override
		{
			return true;
		}

		Awaitable<bool> ping() override
		{
			co_return true;
		}

		std::string_view backend() const override
		{
			return "mock";
		}

		TimePoint lastActiveTime() const override
		{
			return active;
		}

		TimePoint lastPingTime() const override
		{
			return active;
		}

		void touch() override
		{
			active = Clock::now();
		}

		Timer completion;
		bool failQuery;
		bool queryEntered {false};
		bool transaction {false};
		size_t commits {0};
		size_t rollbacks {0};
		TimePoint active {Clock::now()};
	};

	class Http2DbCancellationTest : public Http2DispatchCancellationTest
	{
	protected:
		void verify(bool fail)
		{
			auto connection = std::make_shared<BlockingDbConnection>(io, fail);
			db::DbConfig config;
			config.minConnections = 1;
			config.maxConnections = 1;
			config.idleCheckInterval = std::chrono::seconds {0};
			config.healthCheckInterval = std::chrono::seconds {0};
			auto pool = std::make_shared<db::DbConnectionPool>(
				io,
				config,
				[connection](boost::asio::io_context&,
							 const db::DbConfig&) -> Awaitable<std::shared_ptr<db::DbConnection>>
				{
					co_return connection;
				});
			auto initialized = boost::asio::co_spawn(io, pool->init(), boost::asio::use_future);
			io.run();
			ASSERT_NO_THROW(initialized.get());
			io.restart();
			auto lifetime = std::make_shared<Lifetime>(io.get_executor());
			observe(lifetime);
			middleware.use(db::makeDbMiddleware(pool, {.autoTransaction = true}));
			router.post("/work",
						[connection](const HttpRequest& req) -> Awaitable<HttpResponse>
						{
							EXPECT_EQ(db::getDbConnection(req), connection);
							EXPECT_TRUE(connection->inTransaction());
							co_await connection->query("SELECT value", {});
							EXPECT_EQ(req.body(), "body");
							co_return HttpResponse::ok("done");
						});
			prepare();
			auto stream = request(true);
			auto result = boost::asio::co_spawn(io,
												session->dispatch(stream, Clock::now() + kTestTimeout),
												boost::asio::use_future);
			io.poll();
			EXPECT_TRUE(connection->queryEntered);
			stream->reset();
			stream->reset();
			EXPECT_TRUE(connection->inTransaction());
			EXPECT_EQ(pool->activeCount(), 1U);
			EXPECT_EQ(pool->idleCount(), 0U);
			EXPECT_EQ(connection->commits, 0U);
			EXPECT_EQ(connection->rollbacks, 0U);
			EXPECT_EQ(service->used(), 1U);
			EXPECT_EQ(budget.usage().receiveBytes, 4U);
			EXPECT_EQ(lifetime->destroyed, 0U);
			stream.reset();
			// 模拟 DB 操作自身完成；RST 没有提前取消该等待或归还事务连接。
			connection->completion.cancel();
			io.run();
			EXPECT_EQ(result.get().statusCode(), fail ? HttpStatusCode::hInternalServerError : HttpStatusCode::hOk);
			EXPECT_FALSE(connection->inTransaction());
			EXPECT_EQ(connection->commits, fail ? 0U : 1U);
			EXPECT_EQ(connection->rollbacks, fail ? 1U : 0U);
			EXPECT_EQ(pool->activeCount(), 0U);
			EXPECT_EQ(pool->idleCount(), 1U);
			EXPECT_EQ(lifetime->destroyed, 1U);
			EXPECT_EQ(service->used(), 0U);
			EXPECT_EQ(budget.usage().receiveBytes, 0U);
			io.restart();
			auto stopped = boost::asio::co_spawn(io, pool->shutdown(), boost::asio::use_future);
			io.run();
			EXPECT_NO_THROW(stopped.get());
		}
	};
} // namespace

TEST_F(Http2DbCancellationTest, ResetKeepsBorrowedTransactionUntilNaturalCommit)
{
	verify(false);
}

TEST_F(Http2DbCancellationTest, ResetKeepsBorrowedTransactionUntilNaturalRollback)
{
	verify(true);
}
#endif
