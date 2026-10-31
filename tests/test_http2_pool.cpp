/**
 * @file test_http2_pool.cpp
 * @brief 连接池参数持有、身份隔离、独占归还和请求期限测试
 */

#include "asio/HttpClientPool.h"
#ifdef HICAL_HAS_HTTP2
	#include "core/http2/Http2Connection.h"
#endif
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/read_until.hpp>
#include <boost/asio/use_future.hpp>
#include <gtest/gtest.h>
#include <charconv>
#include <fstream>
#include <functional>
#include <optional>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace hical;

namespace
{
	using Clock = std::chrono::steady_clock;
	using Tcp = boost::asio::ip::tcp;
	using TlsSocket = boost::asio::ssl::stream<Tcp::socket>;
	constexpr auto kTestTimeout = std::chrono::seconds {5};
	constexpr size_t kMaxRequestBytes = 1024 * 1024;

	std::string headerValue(std::string_view head, std::string_view name)
	{
		for (size_t offset = head.find("\r\n") + 2; offset < head.size();)
		{
			const auto end = head.find("\r\n", offset);
			const auto colon = head.find(':', offset);
			if (end == std::string_view::npos || colon == std::string_view::npos || colon >= end)
			{
				return {};
			}
			if (HeaderMap::iequals(head.substr(offset, colon - offset), name))
			{
				auto value = head.substr(colon + 1, end - colon - 1);
				while (!value.empty() && value.front() == ' ')
				{
					value.remove_prefix(1);
				}
				return std::string(value);
			}
			offset = end + 2;
		}
		return {};
	}

	struct Harness
	{
		boost::asio::io_context io;
		Tcp::acceptor acceptor {io, {boost::asio::ip::address_v4::loopback(), 0}};
		boost::asio::steady_timer watchdog {io};
		boost::asio::steady_timer responseRelease {io, Clock::time_point::max()};
		std::vector<std::shared_ptr<Tcp::socket>> sockets;
		std::vector<std::shared_ptr<TlsSocket>> tlsSockets;
		std::shared_ptr<SslContext> serverContext;
		std::function<void()> onRequest;
		std::function<bool(size_t)> disconnectAfterRequest;
		std::function<void()> onTimeout;
		bool holdResponses {false};
		bool timedOut {false};
		size_t requests {0};
		size_t accepts {0};

		explicit Harness(bool tls = false)
		{
			if (tls)
			{
				if (!std::ifstream("test_server.crt") || !std::ifstream("test_server.key"))
				{
					throw std::runtime_error("连接池 TLS 测试必须在工作目录提供测试证书和私钥");
				}
				serverContext = std::make_shared<SslContext>(boost::asio::ssl::context::tls_server);
				serverContext->loadCertificate("test_server.crt");
				serverContext->loadPrivateKey("test_server.key");
			}
		}

		uint16_t port() const
		{
			return acceptor.local_endpoint().port();
		}

		static std::shared_ptr<SslContext> clientContext()
		{
			auto context = std::make_shared<SslContext>(boost::asio::ssl::context::tls_client);
			context->loadCaCertificate("test_server.crt");
			return context;
		}

		void stop()
		{
			boost::system::error_code ignored;
			acceptor.close(ignored);
			for (const auto& socket : sockets)
			{
				socket->close(ignored);
			}
			for (const auto& socket : tlsSockets)
			{
				socket->next_layer().close(ignored);
			}
			responseRelease.cancel();
			watchdog.cancel();
		}

		template <typename Socket>
		Awaitable<void> serve(std::shared_ptr<Socket> socket, size_t connection)
		{
			boost::system::error_code error;
			std::string buffer;
			for (;;)
			{
				const auto headBytes = co_await boost::asio::async_read_until(
					*socket,
					boost::asio::dynamic_buffer(buffer, kMaxRequestBytes),
					"\r\n\r\n",
					boost::asio::redirect_error(boost::asio::use_awaitable, error));
				if (error)
				{
					co_return;
				}
				const auto head = buffer.substr(0, headBytes);
				const auto length = headerValue(head, "Content-Length");
				size_t bodyBytes = 0;
				if (!length.empty())
				{
					const auto parsed = std::from_chars(length.data(), length.data() + length.size(), bodyBytes);
					if (parsed.ec != std::errc {} || bodyBytes > kMaxRequestBytes - headBytes)
					{
						co_return;
					}
				}
				if (buffer.size() < headBytes + bodyBytes)
				{
					co_await boost::asio::async_read(
						*socket,
						boost::asio::dynamic_buffer(buffer, kMaxRequestBytes),
						boost::asio::transfer_exactly(headBytes + bodyBytes - buffer.size()),
						boost::asio::redirect_error(boost::asio::use_awaitable, error));
					if (error)
					{
						co_return;
					}
				}
				const auto first = head.find(' ');
				const auto second = head.find(' ', first + 1);
				const auto body = head.substr(first + 1, second - first - 1) + "|" + headerValue(head, "X-Owned") + "|"
								  + buffer.substr(headBytes, bodyBytes) + "|" + headerValue(head, "Content-Type");
				buffer.erase(0, headBytes + bodyBytes);
				++requests;
				if (onRequest)
				{
					onRequest();
				}
				if (disconnectAfterRequest && disconnectAfterRequest(requests))
				{
					socket->lowest_layer().close(error);
					co_return;
				}
				if (holdResponses)
				{
					co_await responseRelease.async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, error));
					if (error)
					{
						co_return;
					}
				}
				const auto response = "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(body.size())
									  + "\r\nX-Connection: " + std::to_string(connection) + "\r\n\r\n" + body;
				co_await boost::asio::async_write(*socket,
												  boost::asio::buffer(response),
												  boost::asio::redirect_error(boost::asio::use_awaitable, error));
				if (error)
				{
					co_return;
				}
			}
		}

		Awaitable<void> serveTls(std::shared_ptr<TlsSocket> socket, size_t connection)
		{
			boost::system::error_code error;
			co_await socket->async_handshake(boost::asio::ssl::stream_base::server,
											 boost::asio::redirect_error(boost::asio::use_awaitable, error));
			if (!error)
			{
				co_await serve(std::move(socket), connection);
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
				sockets.push_back(socket);
				const auto connection = ++accepts;
				if (serverContext)
				{
					auto tls = std::make_shared<TlsSocket>(std::move(*socket), serverContext->native());
					tlsSockets.push_back(tls);
					coSpawn(io, serveTls(std::move(tls), connection));
				}
				else
				{
					coSpawn(io, serve(std::move(socket), connection));
				}
			}
		}

		void run(Awaitable<void> scenario, std::function<void()> cleanup = {})
		{
			runWithAcceptor(std::move(scenario), accept(), std::move(cleanup));
		}

		void runWithAcceptor(Awaitable<void> scenario, Awaitable<void> acceptTask, std::function<void()> cleanup)
		{
			watchdog.expires_at(Clock::now() + kTestTimeout);
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
			coSpawn(io, std::move(acceptTask));
			auto future = boost::asio::co_spawn(io, std::move(scenario), boost::asio::use_future);
			while (!timedOut && future.wait_for(Clock::duration::zero()) != std::future_status::ready)
			{
				if (io.run_one() == 0)
				{
					break;
				}
			}
			if (cleanup)
			{
				cleanup();
			}
			stop();
			io.restart();
			io.run();
			ASSERT_EQ(future.wait_for(Clock::duration::zero()), std::future_status::ready);
			EXPECT_NO_THROW(future.get());
			EXPECT_FALSE(timedOut);
		}
	};

#ifdef HICAL_HAS_HTTP2
	struct CompletionHook
	{
		bool armed {false};
		bool fired {false};
		std::function<void()> held;
		std::function<bool()> ready;

		void resume()
		{
			if (held)
			{
				auto continuation = std::exchange(held, {});
				continuation();
			}
		}
	};

	/** 只延后同一个事件循环上的请求恢复回调，不替换任何网络 I/O。 */
	class HookExecutor
	{
	public:
		HookExecutor() = default;

		HookExecutor(boost::asio::any_io_executor inner, std::shared_ptr<CompletionHook> hook)
			: inner_(std::move(inner)), hook_(std::move(hook))
		{
		}

		template <typename Function>
		void execute(Function&& function) const
		{
			inner_.execute(
				[hook = hook_,
				 task = std::make_shared<std::decay_t<Function>>(std::forward<Function>(function))]() mutable
				{
					if (hook->armed && (!hook->ready || hook->ready()))
					{
						hook->armed = false;
						hook->fired = true;
						// Asio completion 可能仅可移动；shared task 让持有它的 std::function 可复制。
						hook->held = [task = std::move(task)]() mutable
						{
							(*task)();
						};
						return;
					}
					(*task)();
				});
		}

		template <typename Property>
			requires boost::asio::can_query<boost::asio::any_io_executor, Property>::value
		decltype(auto) query(const Property& property) const noexcept(noexcept(boost::asio::query(inner_, property)))
		{
			return boost::asio::query(inner_, property);
		}

		template <typename Property>
			requires boost::asio::can_require<boost::asio::any_io_executor, Property>::value
		HookExecutor require(const Property& property) const
		{
			return {boost::asio::require(inner_, property), hook_};
		}

		template <typename Property>
			requires boost::asio::can_prefer<boost::asio::any_io_executor, Property>::value
		HookExecutor prefer(const Property& property) const
		{
			return {boost::asio::prefer(inner_, property), hook_};
		}

		friend bool operator==(const HookExecutor& left, const HookExecutor& right) noexcept
		{
			return left.inner_ == right.inner_ && left.hook_ == right.hook_;
		}

	private:
		boost::asio::any_io_executor inner_;
		std::shared_ptr<CompletionHook> hook_;
	};

	struct H2Peer : detail::Http2Codec::Listener
	{
		using Codec = detail::Http2Codec;
		std::shared_ptr<TlsSocket> socket;
		std::shared_ptr<detail::Http2Connection> connection;
		std::unordered_map<int32_t, HeaderMap> headers;
		std::unordered_map<int32_t, std::string> bodies;
		std::function<void(H2Peer&, int32_t)> received;
		size_t settingsAcks {0};

		void onSettings(bool ack) override
		{
			settingsAcks += ack;
		}

		detail::Http2Code onHeader(int32_t id, Codec::Header field) override
		{
			headers[id].insert(field.name, field.value);
			return detail::Http2Code::hNoError;
		}

		detail::Http2Code onData(int32_t id, std::string_view data) override
		{
			bodies[id].append(data);
			EXPECT_FALSE(connection->codec().consume(id, data.size()));
			return detail::Http2Code::hNoError;
		}

		void onEndStream(int32_t id) override
		{
			received(*this, id);
		}

		void send(int32_t id, std::string body)
		{
			const auto length = std::to_string(body.size());
			const Codec::Header fields[] {{":status", "200"}, {"content-length", length}};
			auto opened = connection->output().submitResponse(id, fields);
			EXPECT_FALSE(opened.error) << opened.error.message;
			if (!opened.error)
			{
				EXPECT_FALSE(opened.producer->appendOwned(std::move(body)));
				EXPECT_FALSE(opened.producer->finish());
			}
		}

		void reset(int32_t id, detail::Http2Code code)
		{
			EXPECT_FALSE(connection->codec().reset(id, code));
			connection->wakeWriter();
		}

		void goaway(int32_t lastId, detail::Http2Code code = detail::Http2Code::hNoError)
		{
			EXPECT_FALSE(connection->codec().goaway(lastId, code));
			connection->wakeWriter();
		}
	};

	struct H2Harness : Harness
	{
		struct Request
		{
			H2Peer* peer;
			int32_t id;
			std::string target;
			std::string body;

			void respond() const
			{
				peer->send(id, target);
			}
		};

		detail::Http2ServiceBudget service {64};
		Http2Options serverOptions;
		std::vector<std::shared_ptr<H2Peer>> peers;
		std::vector<Request> received;
		std::function<void(const Request&)> onH2Request;
		boost::asio::steady_timer handshakeRelease {io, Clock::time_point::max()};
		bool holdHandshakes {false};
		size_t hellos {0};
		size_t handshakes {0};
		size_t joined {0};

		H2Harness() : Harness(true)
		{
			serverContext->setServerHttpProtocolMode(HttpProtocolMode::hNegotiateHttp2);
			serverContext->freezeServerHttpProtocolConfig();
			onH2Request = [](const Request& request)
			{
				request.respond();
			};
		}

		Awaitable<void> serveH2(std::shared_ptr<TlsSocket> socket)
		{
			boost::system::error_code error;
			if (holdHandshakes)
			{
				// 只确认 ClientHello 已到，不消费握手字节；释放门后仍执行真实 TLS 握手。
				co_await socket->next_layer().async_wait(
					Tcp::socket::wait_read,
					boost::asio::redirect_error(boost::asio::use_awaitable, error));
				if (error)
				{
					co_return;
				}
				++hellos;
				while (holdHandshakes)
				{
					co_await handshakeRelease.async_wait(
						boost::asio::redirect_error(boost::asio::use_awaitable, error));
				}
			}
			co_await socket->async_handshake(boost::asio::ssl::stream_base::server,
											 boost::asio::redirect_error(boost::asio::use_awaitable, error));
			if (error)
			{
				co_return;
			}
			++handshakes;
			auto peer = std::make_shared<H2Peer>();
			peer->socket = socket;
			peer->received = [this](H2Peer& owner, int32_t id)
			{
				received.push_back({&owner, id, std::string(owner.headers[id].find(":path")), owner.bodies[id]});
				onH2Request(received.back());
			};
			peer->connection = detail::Http2Connection::create(
				*socket,
				detail::Http2Codec::Role::hServer,
				serverOptions,
				detail::Http2Budget(detail::Http2Budget::Role::hServer, serverOptions, &service));
			EXPECT_FALSE(peer->connection->setListener(peer.get()));
			peers.push_back(peer);
			co_await peer->connection->run();
			++joined;
		}

		Awaitable<void> acceptH2()
		{
			for (;;)
			{
				auto socket = std::make_shared<TlsSocket>(io, serverContext->native());
				boost::system::error_code error;
				co_await acceptor.async_accept(socket->next_layer(),
											   boost::asio::redirect_error(boost::asio::use_awaitable, error));
				if (error)
				{
					co_return;
				}
				++accepts;
				tlsSockets.push_back(socket);
				coSpawn(io, serveH2(std::move(socket)));
			}
		}

		template <typename Predicate>
		Awaitable<bool> waitFor(Predicate predicate)
		{
			while (!predicate())
			{
				if (timedOut)
				{
					co_return false;
				}
				co_await boost::asio::post(boost::asio::use_awaitable);
			}
			co_return true;
		}

		void releaseHandshakes()
		{
			holdHandshakes = false;
			handshakeRelease.cancel();
		}

		Awaitable<bool> changeLimit(uint32_t limit)
		{
			if (!co_await waitFor(
					[&]
					{
						return !peers.empty() && peers.front()->settingsAcks != 0;
					}))
			{
				co_return false;
			}
			auto& peer = *peers.front();
			const auto acknowledged = peer.settingsAcks;
			const H2Peer::Codec::Setting setting[] {{detail::Http2Setting::hMaxConcurrentStreams, limit}};
			EXPECT_FALSE(peer.connection->codec().submitSettings(setting));
			peer.connection->wakeWriter();
			co_return co_await waitFor(
				[&]
				{
					return peer.settingsAcks > acknowledged;
				});
		}

		void launch(Awaitable<HttpClientResult> request,
					std::optional<HttpClientResult>& destination,
					boost::asio::cancellation_slot slot = {})
		{
			boost::asio::co_spawn(io,
								  std::move(request),
								  boost::asio::bind_cancellation_slot(
									  slot,
									  [result = &destination](std::exception_ptr error, HttpClientResult value)
									  {
										  EXPECT_FALSE(error);
										  *result = std::move(value);
									  }));
		}

		void respond(std::string_view target)
		{
			for (const auto& request : received)
			{
				if (request.target == target)
				{
					request.respond();
					return;
				}
			}
			ADD_FAILURE() << "没有对应的真实入站流：" << target;
		}

		void run(Awaitable<void> scenario, std::function<void()> cleanup)
		{
			runWithAcceptor(std::move(scenario),
							acceptH2(),
							[this, cleanup = std::move(cleanup)]
							{
								releaseHandshakes();
								cleanup();
							});
		}
	};

	HttpClientOptions h2Options()
	{
		HttpClientOptions options;
		options.tlsContext = Harness::clientContext();
		options.protocolMode = HttpProtocolMode::hHttp2Only;
		options.http2.maxConnectionsPerOrigin = 1;
		return options;
	}
#endif
} // namespace

TEST(Http2PoolTest, RequestOwnsParametersAndOptionsBeforeTheAwaitableStarts)
{
	Harness harness;
	HttpClientPool pool(harness.io);
	std::string host = "127.0.0.1", target = "/original", body = "original";
	HeaderMap headers;
	headers.set("X-Owned", "original");
	HttpClientOptions options;
	auto pending = pool.request(host, harness.port(), HttpMethod::hPost, target, body, headers, options);
	target.assign("/mutatedx");
	body.assign("mutatedx");
	headers.set("X-Owned", "mutatedx");
	options.protocolMode = static_cast<HttpProtocolMode>(255);
	auto scenario = [&]() -> Awaitable<void>
	{
		const auto result = co_await std::move(pending);
		EXPECT_TRUE(result.ok()) << result.error.message;
		EXPECT_EQ(result.body, "/original|original|original|");
		pool.shutdown();
	};
	harness.run(scenario(),
				[&]
				{
					pool.shutdown();
				});
}

TEST(Http2PoolTest, GetOwnsHostTargetAndHeadersBeforeTheAwaitableStarts)
{
	Harness harness;
	HttpClientPool pool(harness.io);
	std::string host = "127.0.0.1", target = "/original";
	HeaderMap headers;
	headers.set("X-Owned", "original");
	auto pending = pool.get(host, harness.port(), target, headers);
	host.assign("127.0.0.2");
	target.assign("/mutatedx");
	headers.set("X-Owned", "mutatedx");
	auto scenario = [&]() -> Awaitable<void>
	{
		const auto result = co_await std::move(pending);
		EXPECT_TRUE(result.ok()) << result.error.message;
		EXPECT_EQ(result.body, "/original|original||");
		pool.shutdown();
	};
	harness.run(scenario(),
				[&]
				{
					pool.shutdown();
				});
}

TEST(Http2PoolTest, PostOwnsTemporaryArgumentsAndItsContentType)
{
	Harness harness;
	HttpClientPool pool(harness.io);
	auto pending = pool.post(std::string("127.0.0.1"),
							 harness.port(),
							 std::string("/owned"),
							 std::string("body"),
							 std::string("text/owned"));
	auto scenario = [&]() -> Awaitable<void>
	{
		const auto result = co_await std::move(pending);
		EXPECT_TRUE(result.ok()) << result.error.message;
		EXPECT_EQ(result.body, "/owned||body|text/owned");
		pool.shutdown();
	};
	harness.run(scenario(),
				[&]
				{
					pool.shutdown();
				});
}

TEST(Http2PoolTest, DestroyedPoolMakesASavedAwaitableFailWithoutConnecting)
{
	Harness harness;
	auto pool = std::make_unique<HttpClientPool>(harness.io);
	auto pending = pool->get("127.0.0.1", harness.port(), "/saved");
	pool.reset();
	auto scenario = [&]() -> Awaitable<void>
	{
		const auto result = co_await std::move(pending);
		EXPECT_EQ(result.error.code, ErrorCode::hOperationAborted);
		EXPECT_EQ(result.requestDisposition, HttpRequestDisposition::hNotSent);
	};
	harness.run(scenario());
	EXPECT_EQ(harness.accepts, 0U);
}

TEST(Http2PoolTest, SavedUnstartedRequestPreventsLegacyReleaseFromAdvertisingIdleCapacity)
{
	Harness harness;
	HttpClientPool pool(harness.io);
	auto client = pool.acquire("127.0.0.1", harness.port());
	auto scenario = [&]() -> Awaitable<void>
	{
		EXPECT_TRUE((co_await client->get("/warm")).ok());
		auto pending = client->get("/saved");
		pool.release(client);
		EXPECT_EQ(pool.idleCount(), 0U);
		auto other = pool.acquire("127.0.0.1", harness.port());
		EXPECT_NE(other, client);
		const auto result = co_await std::move(pending);
		EXPECT_TRUE(result.ok()) << result.error.message;
		EXPECT_EQ(result.body, "/saved|||");
		pool.release(client);
		pool.release(other);
		pool.shutdown();
	};
	harness.run(scenario(),
				[&]
				{
					pool.shutdown();
				});
}

TEST(Http2PoolTest, DestroyingASavedRequestOnAnotherThreadReturnsItsPendingTicket)
{
	Harness harness;
	HttpClientPool pool(harness.io);
	auto client = pool.acquire("127.0.0.1", harness.port());
	auto scenario = [&]() -> Awaitable<void>
	{
		EXPECT_TRUE((co_await client->get("/warm")).ok());
		auto pending = client->get("/unused");
		pool.release(client);
		EXPECT_EQ(pool.idleCount(), 0U);
		std::thread dispose(
			[request = std::move(pending)]() mutable
			{
			});
		dispose.join();
		pool.release(client);
		EXPECT_EQ(pool.idleCount(), 1U);
		pool.shutdown();
	};
	harness.run(scenario(),
				[&]
				{
					pool.shutdown();
				});
	EXPECT_EQ(harness.requests, 1U);
}

TEST(Http2PoolTest, BusyReleaseDoesNotCloseAnotherOwnersActiveExchange)
{
	Harness harness;
	HttpClientPool pool(harness.io);
	auto client = pool.acquire("127.0.0.1", harness.port());
	harness.onRequest = [&]
	{
		pool.release(client);
		EXPECT_EQ(pool.idleCount(), 0U);
		auto other = pool.acquire("127.0.0.1", harness.port());
		EXPECT_NE(other, client);
		pool.release(other);
	};
	auto scenario = [&]() -> Awaitable<void>
	{
		EXPECT_TRUE((co_await client->get("/active")).ok());
		pool.release(client);
		pool.shutdown();
	};
	harness.run(scenario(),
				[&]
				{
					pool.shutdown();
				});
}

TEST(Http2PoolTest, ForeignAndOtherPoolsClientsCannotEnterTheIdleBucket)
{
	Harness harness;
	HttpClientPool pool(harness.io), otherPool(harness.io);
	auto foreign = std::make_shared<HttpClient>(harness.io, "127.0.0.1", harness.port());
	auto other = otherPool.acquire("127.0.0.1", harness.port());
	auto scenario = [&]() -> Awaitable<void>
	{
		EXPECT_TRUE((co_await foreign->get("/foreign")).ok());
		EXPECT_TRUE((co_await other->get("/other")).ok());
		pool.release(foreign);
		pool.release(other);
		EXPECT_EQ(pool.idleCount(), 0U);
		EXPECT_TRUE(foreign->isAlive());
		EXPECT_TRUE(other->isAlive());
		otherPool.release(other);
		pool.shutdown();
		otherPool.shutdown();
	};
	harness.run(scenario(),
				[&]
				{
					pool.shutdown();
					otherPool.shutdown();
				});
}

TEST(Http2PoolTest, FullTlsIdentitySeparatesContextSniVerificationAndProtocolPolicy)
{
	Harness harness(true);
	HttpClientPool pool(harness.io);
	HttpClientOptions original;
	original.tlsContext = Harness::clientContext();
	auto scenario = [&]() -> Awaitable<void>
	{
		auto client = pool.acquire("127.0.0.1", harness.port(), original);
		EXPECT_TRUE((co_await client->get("/warm")).ok());
		pool.release(client);
		std::vector<HttpClientOptions> different(4, original);
		different[0].tlsContext = Harness::clientContext();
		different[1].sniHost = "route.example";
		different[2].verifyPeer = false;
		different[3].protocolMode = HttpProtocolMode::hNegotiateHttp2;
		for (const auto& options : different)
		{
			auto candidate = pool.acquire("127.0.0.1", harness.port(), options);
			EXPECT_NE(candidate, client);
			pool.release(candidate);
		}
		auto equivalent = original;
		equivalent.sniHost = "127.0.0.1";
		equivalent.requestTimeout = std::chrono::milliseconds {13};
		equivalent.connectTimeout = std::chrono::milliseconds {17};
		equivalent.http2.maxHeaderBytes += 1;
		auto reused = pool.acquire("127.0.0.1", harness.port(), equivalent);
		EXPECT_EQ(reused, client);
		pool.release(reused);
		pool.shutdown();
	};
	harness.run(scenario(),
				[&]
				{
					pool.shutdown();
				});
}

#ifdef HICAL_HAS_HTTP2
TEST(Http2PoolTest, NegotiatingClientsWithDifferentFrozenHttp2LimitsDoNotShareAH1FallbackConnection)
{
	Harness harness(true);
	HttpClientPool pool(harness.io);
	HttpClientOptions original;
	original.tlsContext = Harness::clientContext();
	original.protocolMode = HttpProtocolMode::hNegotiateHttp2;
	auto scenario = [&]() -> Awaitable<void>
	{
		auto client = pool.acquire("127.0.0.1", harness.port(), original);
		EXPECT_TRUE((co_await client->get("/fallback")).ok());
		pool.release(client);
		std::vector<HttpClientOptions> different;
		auto add = [&](auto member, auto value)
		{
			auto options = original;
			options.http2.*member = value;
			different.push_back(std::move(options));
		};
		add(&Http2Options::maxConcurrentStreams, 3U);
		add(&Http2Options::maxApplicationTasks, 3U);
		add(&Http2Options::maxServerApplicationTasks, size_t {3});
		add(&Http2Options::maxHeaderBytes, size_t {4096});
		add(&Http2Options::maxHeaderFields, size_t {32});
		add(&Http2Options::maxBodyBytes, size_t {1024});
		add(&Http2Options::maxConnectionReceiveBytes, size_t {16 * 1024 * 1024});
		add(&Http2Options::maxConnectionSendBytes, size_t {16 * 1024 * 1024});
		add(&Http2Options::maxOutputBatchBytes, size_t {1024});
		add(&Http2Options::maxFilePrefetchBytes, size_t {1024});
		add(&Http2Options::maxResponseBytes, size_t {1024});
		add(&Http2Options::maxClientConnectionReceiveBytes, size_t {64 * 1024 * 1024});
		add(&Http2Options::maxConnectionsPerOrigin, size_t {3});
		add(&Http2Options::maxQueuedRequestsPerOrigin, size_t {3});
		add(&Http2Options::initialStreamWindow, 32768U);
		add(&Http2Options::initialConnectionWindow, 131072U);
		add(&Http2Options::maxFrameBytes, 32768U);
		add(&Http2Options::headerTableBytes, 1024U);
		add(&Http2Options::negotiationTimeout, std::chrono::seconds {3});
		add(&Http2Options::headerTimeout, std::chrono::seconds {3});
		add(&Http2Options::bodyTimeout, std::chrono::seconds {3});
		add(&Http2Options::writeTimeout, std::chrono::seconds {3});
		add(&Http2Options::queueTimeout, std::chrono::seconds {3});
		for (size_t index = 0; index < different.size(); ++index)
		{
			SCOPED_TRACE(index);
			EXPECT_TRUE(different[index].http2.validationError().empty());
			auto candidate = pool.acquire("127.0.0.1", harness.port(), different[index]);
			EXPECT_NE(candidate, client);
			pool.release(candidate);
		}
		pool.shutdown();
	};
	harness.run(scenario(),
				[&]
				{
					pool.shutdown();
				});
}
#endif

TEST(Http2PoolTest, ReusedConnectionUsesThisCallsRequestTimeout)
{
	Harness harness;
	HttpClientPool pool(harness.io);
	HttpClientOptions options;
	options.requestTimeout = std::chrono::seconds {10};
	auto scenario = [&]() -> Awaitable<void>
	{
		EXPECT_TRUE((co_await pool.get("127.0.0.1", harness.port(), "/warm", {}, options)).ok());
		harness.holdResponses = true;
		options.requestTimeout = std::chrono::milliseconds {40};
		const auto result = co_await pool.get("127.0.0.1", harness.port(), "/timeout", {}, options);
		EXPECT_EQ(result.error.code, ErrorCode::hTimedOut);
		pool.shutdown();
	};
	harness.run(scenario(),
				[&]
				{
					pool.shutdown();
				});
}

TEST(Http2PoolTest, SubmissionDeadlineCanExpireBeforeADeferredAwaitableStarts)
{
	Harness harness;
	HttpClientPool pool(harness.io);
	HttpClientOptions options;
	options.requestTimeout = std::chrono::milliseconds {1};
	auto pending = pool.get("127.0.0.1", harness.port(), "/late", {}, options);
	const auto resumeAt = Clock::now() + std::chrono::milliseconds {10};
	auto scenario = [&]() -> Awaitable<void>
	{
		boost::asio::steady_timer ready(harness.io, resumeAt);
		co_await ready.async_wait(boost::asio::use_awaitable);
		const auto result = co_await std::move(pending);
		EXPECT_EQ(result.error.code, ErrorCode::hTimedOut);
		EXPECT_EQ(result.requestDisposition, HttpRequestDisposition::hNotSent);
		pool.shutdown();
	};
	harness.run(scenario(),
				[&]
				{
					pool.shutdown();
				});
	EXPECT_EQ(harness.accepts, 0U);
}

TEST(Http2PoolTest, ConsecutiveHighLevelRequestsReuseTheSameIdleConnection)
{
	Harness harness;
	HttpClientPool pool(harness.io);
	auto scenario = [&]() -> Awaitable<void>
	{
		const auto first = co_await pool.get("127.0.0.1", harness.port(), "/first");
		EXPECT_TRUE(first.ok()) << first.error.message;
		EXPECT_EQ(pool.idleCount(), 1U);
		const auto second = co_await pool.get("127.0.0.1", harness.port(), "/second");
		EXPECT_TRUE(second.ok()) << second.error.message;
		EXPECT_EQ(first.headers.find("X-Connection"), second.headers.find("X-Connection"));
		EXPECT_EQ(pool.idleCount(), 1U);
	};
	harness.run(scenario(),
				[&]
				{
					pool.shutdown();
				});
	EXPECT_EQ(harness.accepts, 1U);
}

TEST(Http2PoolTest, PoolDestructionCancelsItsManagedExchangeWithDisabledTimeouts)
{
	Harness harness;
	harness.holdResponses = true;
	auto pool = std::make_unique<HttpClientPool>(harness.io);
	HttpClientOptions options;
	options.requestTimeout = options.connectTimeout = Clock::duration::zero();
	auto pending = pool->get("127.0.0.1", harness.port(), "/held", {}, options);
	harness.onRequest = [&]
	{
		pool.reset();
	};
	auto scenario = [&]() -> Awaitable<void>
	{
		const auto result = co_await std::move(pending);
		EXPECT_EQ(result.error.code, ErrorCode::hOperationAborted);
		EXPECT_EQ(result.requestDisposition, HttpRequestDisposition::hPossiblyProcessed);
	};
	harness.run(scenario(),
				[&]
				{
					pool.reset();
				});
	EXPECT_EQ(harness.requests, 1U);
}

TEST(Http2PoolTest, ShutdownLeavesAnExternalLegacyOwnersActiveExchangeRunning)
{
	Harness harness;
	HttpClientPool pool(harness.io);
	auto client = pool.acquire("127.0.0.1", harness.port());
	harness.onRequest = [&]
	{
		pool.shutdown();
	};
	auto scenario = [&]() -> Awaitable<void>
	{
		const auto result = co_await client->get("/legacy");
		EXPECT_TRUE(result.ok()) << result.error.message;
		EXPECT_TRUE(client->isAlive());
		pool.release(client);
		EXPECT_EQ(pool.idleCount(), 0U);
	};
	harness.run(scenario(),
				[&]
				{
					pool.shutdown();
				});
}

TEST(Http2PoolTest, ExpiredLegacyCheckoutReleasesItsRetainedContextOnTheCleanupLoop)
{
	Harness harness;
	HttpClientPoolOptions options;
	options.cleanupInterval = std::chrono::milliseconds {1};
	HttpClientPool pool(harness.io, options);
	std::weak_ptr<SslContext> context;
	{
		HttpClientOptions clientOptions;
		clientOptions.tlsContext = std::make_shared<SslContext>(boost::asio::ssl::context::tls_client);
		context = clientOptions.tlsContext;
		auto client = pool.acquire("127.0.0.1", harness.port(), clientOptions);
	}
	auto scenario = [&]() -> Awaitable<void>
	{
		while (!context.expired() && !harness.timedOut)
		{
			co_await boost::asio::post(boost::asio::use_awaitable);
		}
		EXPECT_TRUE(context.expired());
	};
	harness.run(scenario(),
				[&]
				{
					pool.shutdown();
				});
	EXPECT_EQ(harness.accepts, 0U);
}

TEST(Http2PoolRetryTest, AReusedHttp1PostThatWasProcessedBeforeDisconnectIsNeverReplayed)
{
	Harness harness;
	harness.disconnectAfterRequest = [](size_t count)
	{
		return count == 2;
	};
	HttpClientPool pool(harness.io);
	auto scenario = [&]() -> Awaitable<void>
	{
		EXPECT_TRUE((co_await pool.get("127.0.0.1", harness.port(), "/warmup")).ok());
		auto result = co_await pool.post("127.0.0.1", harness.port(), "/side-effect", "owned-payload");
		EXPECT_TRUE(result.error);
		EXPECT_EQ(result.requestDisposition, HttpRequestDisposition::hPossiblyProcessed);
		EXPECT_EQ(result.status, 0);
		EXPECT_EQ(harness.requests, 2U);
	};
	harness.run(scenario(),
				[&]
				{
					pool.shutdown();
				});
	EXPECT_EQ(harness.requests, 2U);
	EXPECT_EQ(harness.accepts, 1U);
}

TEST(Http2PoolRetryTest, AReusedHttp1GetKeepsItsSingleTransportRetry)
{
	Harness harness;
	harness.disconnectAfterRequest = [](size_t count)
	{
		return count == 2;
	};
	HttpClientPool pool(harness.io);
	auto scenario = [&]() -> Awaitable<void>
	{
		EXPECT_TRUE((co_await pool.get("127.0.0.1", harness.port(), "/warmup")).ok());
		auto result = co_await pool.get("127.0.0.1", harness.port(), "/idempotent");
		EXPECT_TRUE(result.ok()) << result.error.message;
		EXPECT_EQ(result.body, "/idempotent|||");
	};
	harness.run(scenario(),
				[&]
				{
					pool.shutdown();
				});
	EXPECT_EQ(harness.requests, 3U);
	EXPECT_EQ(harness.accepts, 2U);
}

#ifdef HICAL_HAS_HTTP2
TEST(Http2PoolSchedulingTest, ConcurrentHighLevelRequestsUseOneConnectionAndCompleteOutOfOrder)
{
	constexpr size_t kRequests = 4;
	H2Harness harness;
	HttpClientPool pool(harness.io);
	const auto options = h2Options();
	std::vector<std::optional<HttpClientResult>> results(kRequests);
	std::vector<size_t> completed;
	harness.onH2Request = [&](const H2Harness::Request&)
	{
		if (harness.received.size() == kRequests)
		{
			for (const auto& request : harness.received)
			{
				if (request.target == "/3")
				{
					request.respond();
				}
			}
		}
	};
	auto scenario = [&]() -> Awaitable<void>
	{
		for (size_t index = 0; index < kRequests; ++index)
		{
			boost::asio::co_spawn(harness.io,
								  pool.get("127.0.0.1", harness.port(), "/" + std::to_string(index), {}, options),
								  [&, index](std::exception_ptr error, HttpClientResult result)
								  {
									  EXPECT_FALSE(error);
									  results[index] = std::move(result);
									  completed.push_back(index);
									  if (index > 0)
									  {
										  for (const auto& request : harness.received)
										  {
											  if (request.target == "/" + std::to_string(index - 1))
											  {
												  request.respond();
											  }
										  }
									  }
								  });
		}
		EXPECT_TRUE(co_await harness.waitFor(
			[&]
			{
				return completed.size() == kRequests;
			}));
		for (size_t index = 0; index < kRequests; ++index)
		{
			EXPECT_TRUE(results[index].has_value());
			if (results[index])
			{
				EXPECT_TRUE(results[index]->ok()) << results[index]->error.message;
				EXPECT_EQ(results[index]->protocol, HttpProtocol::hHttp2);
				EXPECT_EQ(results[index]->body, "/" + std::to_string(index));
			}
		}
	};
	harness.run(scenario(),
				[&]
				{
					pool.shutdown();
				});
	EXPECT_EQ(completed, (std::vector<size_t> {3, 2, 1, 0}));
	EXPECT_EQ(harness.accepts, 1U);
	EXPECT_EQ(harness.handshakes, 1U);
}

TEST(Http2PoolSchedulingTest, FirstRequestsShareTheHandshakeBeforeSettingsAreAvailable)
{
	constexpr size_t kRequests = 3;
	H2Harness harness;
	harness.holdHandshakes = true;
	HttpClientPool pool(harness.io);
	auto options = h2Options();
	options.http2.maxConnectionsPerOrigin = 2;
	std::vector<std::optional<HttpClientResult>> results(kRequests);
	size_t completed = 0;
	auto scenario = [&]() -> Awaitable<void>
	{
		for (size_t index = 0; index < kRequests; ++index)
		{
			boost::asio::co_spawn(harness.io,
								  pool.get("127.0.0.1", harness.port(), "/first-" + std::to_string(index), {}, options),
								  [&, index](std::exception_ptr error, HttpClientResult result)
								  {
									  EXPECT_FALSE(error);
									  results[index] = std::move(result);
									  ++completed;
								  });
		}
		if (!co_await harness.waitFor(
				[&]
				{
					return harness.hellos > 0;
				}))
		{
			ADD_FAILURE() << "首条连接没有到达真实 ClientHello 门";
			co_return;
		}
		EXPECT_EQ(harness.handshakes, 0U);
		harness.releaseHandshakes();
		EXPECT_TRUE(co_await harness.waitFor(
			[&]
			{
				return completed == kRequests;
			}));
		for (size_t index = 0; index < kRequests; ++index)
		{
			EXPECT_TRUE(results[index].has_value());
			if (results[index])
			{
				EXPECT_TRUE(results[index]->ok()) << results[index]->error.message;
				EXPECT_EQ(results[index]->body, "/first-" + std::to_string(index));
			}
		}
	};
	harness.run(scenario(),
				[&]
				{
					pool.shutdown();
				});
	EXPECT_EQ(harness.accepts, 1U);
	EXPECT_EQ(harness.handshakes, 1U);
}

TEST(Http2PoolSchedulingTest, AFullPerOriginQueueRejectsOnlyTheOverflowWithoutOpeningAnotherConnection)
{
	H2Harness harness;
	harness.serverOptions.maxConcurrentStreams = 1;
	harness.onH2Request = [](const H2Harness::Request& request)
	{
		if (request.target != "/held")
		{
			request.respond();
		}
	};
	HttpClientPool pool(harness.io);
	auto options = h2Options();
	options.http2.maxQueuedRequestsPerOrigin = 1;
	std::optional<HttpClientResult> held;
	std::optional<HttpClientResult> queued;
	std::optional<HttpClientResult> overflow;
	auto scenario = [&]() -> Awaitable<void>
	{
		const auto warmup = co_await pool.get("127.0.0.1", harness.port(), "/warmup", {}, options);
		EXPECT_TRUE(warmup.ok()) << warmup.error.message;
		auto launch = [&](std::string_view target, std::optional<HttpClientResult>& result)
		{
			boost::asio::co_spawn(harness.io,
								  pool.get("127.0.0.1", harness.port(), target, {}, options),
								  [destination = &result](std::exception_ptr error, HttpClientResult value)
								  {
									  EXPECT_FALSE(error);
									  *destination = std::move(value);
								  });
		};
		launch("/held", held);
		if (!co_await harness.waitFor(
				[&]
				{
					return harness.received.size() == 2;
				}))
		{
			ADD_FAILURE() << "受限连接没有收到挂起请求";
			co_return;
		}
		launch("/queued", queued);
		launch("/overflow", overflow);
		if (!co_await harness.waitFor(
				[&]
				{
					return overflow.has_value();
				}))
		{
			ADD_FAILURE() << "超过有限队列的请求没有及时完成拒绝";
			co_return;
		}
		EXPECT_EQ(overflow->error.code, ErrorCode::hResourceLimitExceeded);
		EXPECT_EQ(overflow->requestDisposition, HttpRequestDisposition::hNotSent);
		EXPECT_FALSE(held.has_value());
		EXPECT_FALSE(queued.has_value());
		EXPECT_EQ(harness.received.size(), 2U) << "排队和溢出请求此时都不能成为 wire stream";
		for (const auto& request : harness.received)
		{
			if (request.target == "/held")
			{
				request.respond();
			}
		}
		EXPECT_TRUE(co_await harness.waitFor(
			[&]
			{
				return held.has_value() && queued.has_value();
			}));
		if (held && queued)
		{
			EXPECT_TRUE(held->ok()) << held->error.message;
			EXPECT_TRUE(queued->ok()) << queued->error.message;
			EXPECT_EQ(queued->body, "/queued");
		}
	};
	harness.run(scenario(),
				[&]
				{
					pool.shutdown();
				});
	EXPECT_EQ(harness.received.size(), 3U);
	EXPECT_EQ(harness.accepts, 1U);
	EXPECT_EQ(harness.handshakes, 1U);
}

TEST(Http2PoolSchedulingTest, ZeroAndDecreasedPeerLimitsWakeTheSameBoundedQueueWithoutOversending)
{
	H2Harness harness;
	harness.onH2Request = [](const H2Harness::Request& request)
	{
		if (request.target == "/warmup")
		{
			request.respond();
		}
	};
	HttpClientPool pool(harness.io);
	auto options = h2Options();
	options.http2.maxConcurrentStreams = 2;
	std::optional<HttpClientResult> first, second, third;
	auto scenario = [&]() -> Awaitable<void>
	{
		EXPECT_TRUE((co_await pool.get("127.0.0.1", harness.port(), "/warmup", {}, options)).ok());
		EXPECT_TRUE(co_await harness.changeLimit(0));
		harness.launch(pool.get("127.0.0.1", harness.port(), "/first", {}, options), first);
		harness.launch(pool.get("127.0.0.1", harness.port(), "/second", {}, options), second);
		if (!co_await harness.waitFor(
				[&]
				{
					return pool.stats().queuedRequests == 2;
				}))
		{
			co_return;
		}
		EXPECT_EQ(harness.received.size(), 1U);
		EXPECT_EQ(pool.stats().activeRequests, 0U);
		EXPECT_TRUE(co_await harness.changeLimit(2));
		if (!co_await harness.waitFor(
				[&]
				{
					return harness.received.size() == 3;
				}))
		{
			co_return;
		}
		EXPECT_EQ(pool.stats().activeRequests, 2U);
		EXPECT_TRUE(co_await harness.changeLimit(1));
		harness.launch(pool.get("127.0.0.1", harness.port(), "/third", {}, options), third);
		if (!co_await harness.waitFor(
				[&]
				{
					return pool.stats().queuedRequests == 1;
				}))
		{
			co_return;
		}
		harness.respond("/first");
		if (!co_await harness.waitFor(
				[&]
				{
					return first.has_value();
				}))
		{
			co_return;
		}
		EXPECT_TRUE(first->ok()) << first->error.message;
		EXPECT_EQ(pool.stats().activeRequests, 1U);
		EXPECT_EQ(pool.stats().queuedRequests, 1U);
		EXPECT_EQ(harness.received.size(), 3U);
		harness.respond("/second");
		if (!co_await harness.waitFor(
				[&]
				{
					return harness.received.size() == 4;
				}))
		{
			co_return;
		}
		EXPECT_EQ(harness.received.back().target, "/third");
		harness.respond("/third");
		EXPECT_TRUE(co_await harness.waitFor(
			[&]
			{
				return second.has_value() && third.has_value();
			}));
		if (second && third)
		{
			EXPECT_TRUE(second->ok());
			EXPECT_TRUE(third->ok());
		}
		EXPECT_EQ(pool.stats().queuedRequests, 0U);
		EXPECT_EQ(pool.stats().activeRequests, 0U);
		EXPECT_EQ(pool.stats().peakQueuedRequests, 2U);
	};
	harness.run(scenario(),
				[&]
				{
					pool.shutdown();
				});
	EXPECT_EQ(harness.accepts, 1U);
}

TEST(Http2PoolSchedulingTest, AQueuedRequestsOriginalDeadlineExpiresWithoutSendingOrClosingItsBusyNeighbor)
{
	H2Harness harness;
	harness.serverOptions.maxConcurrentStreams = 1;
	harness.onH2Request = [](const H2Harness::Request& request)
	{
		if (request.target != "/held")
		{
			request.respond();
		}
	};
	HttpClientPool pool(harness.io);
	const auto options = h2Options();
	auto shortOptions = options;
	shortOptions.requestTimeout = std::chrono::milliseconds {20};
	std::optional<HttpClientResult> held, expired, next;
	auto scenario = [&]() -> Awaitable<void>
	{
		EXPECT_TRUE((co_await pool.get("127.0.0.1", harness.port(), "/warmup", {}, options)).ok());
		harness.launch(pool.get("127.0.0.1", harness.port(), "/held", {}, options), held);
		if (!co_await harness.waitFor(
				[&]
				{
					return harness.received.size() == 2;
				}))
		{
			co_return;
		}
		harness.launch(pool.get("127.0.0.1", harness.port(), "/expired", {}, shortOptions), expired);
		if (!co_await harness.waitFor(
				[&]
				{
					return expired.has_value();
				}))
		{
			co_return;
		}
		EXPECT_EQ(expired->error.code, ErrorCode::hTimedOut);
		EXPECT_EQ(expired->requestDisposition, HttpRequestDisposition::hNotSent);
		EXPECT_EQ(pool.stats().queuedRequests, 0U);
		EXPECT_EQ(pool.stats().activeRequests, 1U);
		EXPECT_EQ(harness.received.size(), 2U);
		harness.launch(pool.get("127.0.0.1", harness.port(), "/next", {}, options), next);
		if (!co_await harness.waitFor(
				[&]
				{
					return pool.stats().queuedRequests == 1;
				}))
		{
			co_return;
		}
		harness.respond("/held");
		EXPECT_TRUE(co_await harness.waitFor(
			[&]
			{
				return held.has_value() && next.has_value();
			}));
		if (held && next)
		{
			EXPECT_TRUE(held->ok());
			EXPECT_TRUE(next->ok());
			EXPECT_EQ(next->body, "/next");
		}
	};
	harness.run(scenario(),
				[&]
				{
					pool.shutdown();
				});
	EXPECT_EQ(harness.accepts, 1U);
	EXPECT_EQ(harness.received.size(), 3U);
}

TEST(Http2PoolSchedulingTest, NativeCancellationRemovesAQueuedRequestAndReturnsItsCapacityOnce)
{
	H2Harness harness;
	harness.serverOptions.maxConcurrentStreams = 1;
	harness.onH2Request = [](const H2Harness::Request& request)
	{
		if (request.target != "/held")
		{
			request.respond();
		}
	};
	HttpClientPool pool(harness.io);
	auto options = h2Options();
	options.http2.maxQueuedRequestsPerOrigin = 1;
	boost::asio::cancellation_signal cancellation;
	std::optional<HttpClientResult> held, cancelled, next;
	auto scenario = [&]() -> Awaitable<void>
	{
		EXPECT_TRUE((co_await pool.get("127.0.0.1", harness.port(), "/warmup", {}, options)).ok());
		harness.launch(pool.get("127.0.0.1", harness.port(), "/held", {}, options), held);
		if (!co_await harness.waitFor(
				[&]
				{
					return harness.received.size() == 2;
				}))
		{
			co_return;
		}
		harness.launch(pool.get("127.0.0.1", harness.port(), "/cancelled", {}, options),
					   cancelled,
					   cancellation.slot());
		if (!co_await harness.waitFor(
				[&]
				{
					return pool.stats().queuedRequests == 1;
				}))
		{
			co_return;
		}
		cancellation.emit(boost::asio::cancellation_type::terminal);
		if (!co_await harness.waitFor(
				[&]
				{
					return cancelled.has_value();
				}))
		{
			co_return;
		}
		EXPECT_EQ(cancelled->error.code, ErrorCode::hOperationAborted);
		EXPECT_EQ(cancelled->requestDisposition, HttpRequestDisposition::hNotSent);
		EXPECT_EQ(pool.stats().queuedRequests, 0U);
		EXPECT_EQ(pool.stats().activeRequests, 1U);
		harness.launch(pool.get("127.0.0.1", harness.port(), "/next", {}, options), next);
		if (!co_await harness.waitFor(
				[&]
				{
					return pool.stats().queuedRequests == 1;
				}))
		{
			co_return;
		}
		harness.respond("/held");
		EXPECT_TRUE(co_await harness.waitFor(
			[&]
			{
				return held.has_value() && next.has_value();
			}));
		if (held && next)
		{
			EXPECT_TRUE(held->ok());
			EXPECT_TRUE(next->ok());
		}
		EXPECT_EQ(pool.stats().activeRequests, 0U);
		EXPECT_EQ(pool.stats().queuedRequests, 0U);
	};
	harness.run(scenario(),
				[&]
				{
					pool.shutdown();
				});
	EXPECT_EQ(harness.accepts, 1U);
	EXPECT_EQ(harness.received.size(), 3U);
}

TEST(Http2PoolSchedulingTest, LegacyAcquireNeverBorrowsAnActiveOrIdleManagedHttp2Connection)
{
	H2Harness harness;
	harness.onH2Request = [](const H2Harness::Request& request)
	{
		if (request.target != "/held")
		{
			request.respond();
		}
	};
	HttpClientPool pool(harness.io);
	const auto options = h2Options();
	std::optional<HttpClientResult> held;
	auto scenario = [&]() -> Awaitable<void>
	{
		harness.launch(pool.get("127.0.0.1", harness.port(), "/held", {}, options), held);
		if (!co_await harness.waitFor(
				[&]
				{
					return harness.received.size() == 1;
				}))
		{
			co_return;
		}
		auto legacy = pool.acquire("127.0.0.1", harness.port(), options);
		EXPECT_TRUE((co_await legacy->get("/legacy")).ok());
		pool.release(legacy);
		EXPECT_TRUE((co_await pool.get("127.0.0.1", harness.port(), "/shared", {}, options)).ok());
		EXPECT_EQ(harness.received.size(), 3U);
		if (harness.received.size() == 3)
		{
			EXPECT_EQ(harness.received[0].peer, harness.received[2].peer);
			EXPECT_NE(harness.received[0].peer, harness.received[1].peer);
		}
		harness.respond("/held");
		EXPECT_TRUE(co_await harness.waitFor(
			[&]
			{
				return held.has_value();
			}));
		auto idleLegacy = pool.acquire("127.0.0.1", harness.port(), options);
		EXPECT_EQ(idleLegacy, legacy);
		pool.release(idleLegacy);
		EXPECT_EQ(pool.stats().managedConnections, 1U);
	};
	harness.run(scenario(),
				[&]
				{
					pool.shutdown();
				});
	EXPECT_EQ(harness.accepts, 2U);
}

TEST(Http2PoolSchedulingTest, NegotiationFallbackKeepsEachHttp1ExchangeExclusive)
{
	Harness harness(true);
	HttpClientPool pool(harness.io);
	auto options = h2Options();
	options.protocolMode = HttpProtocolMode::hNegotiateHttp2;
	harness.onRequest = [&]
	{
		EXPECT_EQ(pool.stats().activeRequests, 1U);
	};
	std::optional<HttpClientResult> first, second;
	auto scenario = [&]() -> Awaitable<void>
	{
		auto launch = [&](std::string_view target, std::optional<HttpClientResult>& result)
		{
			boost::asio::co_spawn(harness.io,
								  pool.get("127.0.0.1", harness.port(), target, {}, options),
								  [destination = &result](std::exception_ptr error, HttpClientResult value)
								  {
									  EXPECT_FALSE(error);
									  *destination = std::move(value);
								  });
		};
		launch("/first", first);
		launch("/second", second);
		while ((!first || !second) && !harness.timedOut)
		{
			co_await boost::asio::post(boost::asio::use_awaitable);
		}
		EXPECT_TRUE(first.has_value());
		EXPECT_TRUE(second.has_value());
		if (first && second)
		{
			EXPECT_TRUE(first->ok());
			EXPECT_TRUE(second->ok());
			EXPECT_EQ(first->protocol, HttpProtocol::hHttp1);
			EXPECT_EQ(first->body, "/first|||");
			EXPECT_EQ(second->body, "/second|||");
		}
	};
	harness.run(scenario(),
				[&]
				{
					pool.shutdown();
				});
	EXPECT_EQ(harness.accepts, 1U);
}

TEST(Http2PoolSchedulingTest, AShortConnectWaiterTimesOutWithoutAbortingTheSharedLongHandshake)
{
	H2Harness harness;
	harness.holdHandshakes = true;
	HttpClientPool pool(harness.io);
	auto options = h2Options();
	options.connectTimeout = std::chrono::seconds {2};
	options.requestTimeout = std::chrono::seconds {3};
	auto shortOptions = options;
	shortOptions.connectTimeout = std::chrono::milliseconds {20};
	std::optional<HttpClientResult> longRequest, shortRequest;
	auto scenario = [&]() -> Awaitable<void>
	{
		harness.launch(pool.get("127.0.0.1", harness.port(), "/long", {}, options), longRequest);
		if (!co_await harness.waitFor(
				[&]
				{
					return harness.hellos == 1;
				}))
		{
			co_return;
		}
		harness.launch(pool.get("127.0.0.1", harness.port(), "/short", {}, shortOptions), shortRequest);
		if (!co_await harness.waitFor(
				[&]
				{
					return pool.stats().queuedRequests == 1;
				}))
		{
			co_return;
		}
		boost::asio::steady_timer observation(harness.io, Clock::now() + std::chrono::milliseconds {100});
		co_await observation.async_wait(boost::asio::use_awaitable);
		EXPECT_TRUE(shortRequest.has_value()) << "单独的建连等待子期限必须先于共享握手完成";
		EXPECT_FALSE(longRequest.has_value());
		if (shortRequest)
		{
			EXPECT_EQ(shortRequest->error.code, ErrorCode::hTimedOut);
			EXPECT_EQ(shortRequest->requestDisposition, HttpRequestDisposition::hNotSent);
		}
		// 断言失败也要放行握手，让剩余请求正常收尾。
		harness.releaseHandshakes();
		EXPECT_TRUE(co_await harness.waitFor(
			[&]
			{
				return longRequest.has_value() && shortRequest.has_value();
			}));
		if (longRequest)
		{
			EXPECT_TRUE(longRequest->ok()) << longRequest->error.message;
		}
	};
	harness.run(scenario(),
				[&]
				{
					pool.shutdown();
				});
	EXPECT_EQ(harness.accepts, 1U);
	EXPECT_EQ(harness.handshakes, 1U);
	EXPECT_EQ(harness.received.size(), 1U);
}

TEST(Http2PoolSchedulingTest, AConnectedStreamQueueDoesNotUseTheCallersConnectTimeout)
{
	H2Harness harness;
	harness.serverOptions.maxConcurrentStreams = 1;
	harness.onH2Request = [](const H2Harness::Request& request)
	{
		if (request.target != "/held")
		{
			request.respond();
		}
	};
	HttpClientPool pool(harness.io);
	const auto options = h2Options();
	auto shortOptions = options;
	shortOptions.connectTimeout = std::chrono::nanoseconds {1};
	std::optional<HttpClientResult> held, queued;
	auto scenario = [&]() -> Awaitable<void>
	{
		EXPECT_TRUE((co_await pool.get("127.0.0.1", harness.port(), "/warmup", {}, options)).ok());
		harness.launch(pool.get("127.0.0.1", harness.port(), "/held", {}, options), held);
		if (!co_await harness.waitFor(
				[&]
				{
					return harness.received.size() == 2;
				}))
		{
			co_return;
		}
		harness.launch(pool.get("127.0.0.1", harness.port(), "/queued", {}, shortOptions), queued);
		if (!co_await harness.waitFor(
				[&]
				{
					return pool.stats().queuedRequests == 1;
				}))
		{
			co_return;
		}
		boost::asio::steady_timer observation(harness.io, Clock::now() + std::chrono::milliseconds {20});
		co_await observation.async_wait(boost::asio::use_awaitable);
		EXPECT_FALSE(queued.has_value());
		EXPECT_EQ(pool.stats().queuedRequests, 1U);
		harness.respond("/held");
		EXPECT_TRUE(co_await harness.waitFor(
			[&]
			{
				return held.has_value() && queued.has_value();
			}));
		if (queued)
		{
			EXPECT_TRUE(queued->ok()) << queued->error.message;
		}
	};
	harness.run(scenario(),
				[&]
				{
					pool.shutdown();
				});
	EXPECT_EQ(harness.accepts, 1U);
}

TEST(Http2PoolSchedulingTest, AConnectionPublishedInTimeCancelsOnlyTheQueuedCallersConnectDeadline)
{
	H2Harness harness;
	harness.holdHandshakes = true;
	harness.serverOptions.maxConcurrentStreams = 1;
	harness.onH2Request = [](const H2Harness::Request& request)
	{
		if (request.target != "/held")
		{
			request.respond();
		}
	};
	HttpClientPool pool(harness.io);
	auto options = h2Options();
	options.connectTimeout = std::chrono::seconds {2};
	options.requestTimeout = std::chrono::seconds {3};
	auto queuedOptions = options;
	queuedOptions.connectTimeout = std::chrono::milliseconds {200};
	std::optional<HttpClientResult> held, queued;
	auto scenario = [&]() -> Awaitable<void>
	{
		harness.launch(pool.get("127.0.0.1", harness.port(), "/held", {}, options), held);
		if (!co_await harness.waitFor(
				[&]
				{
					return harness.hellos == 1;
				}))
		{
			co_return;
		}
		harness.launch(pool.get("127.0.0.1", harness.port(), "/queued", {}, queuedOptions), queued);
		if (!co_await harness.waitFor(
				[&]
				{
					return pool.stats().queuedRequests == 1;
				}))
		{
			co_return;
		}
		harness.releaseHandshakes();
		if (!co_await harness.waitFor(
				[&]
				{
					return harness.received.size() == 1;
				}))
		{
			co_return;
		}
		// 连接已建立、首流占满容量后，建连子期限到点不能变成流槽等待的期限。
		boost::asio::steady_timer observation(harness.io, Clock::now() + std::chrono::milliseconds {300});
		co_await observation.async_wait(boost::asio::use_awaitable);
		EXPECT_FALSE(queued.has_value());
		EXPECT_EQ(pool.stats().queuedRequests, 1U);
		EXPECT_EQ(harness.received.size(), 1U);
		harness.respond("/held");
		EXPECT_TRUE(co_await harness.waitFor(
			[&]
			{
				return held.has_value() && queued.has_value();
			}));
		if (held && queued)
		{
			EXPECT_TRUE(held->ok()) << held->error.message;
			EXPECT_TRUE(queued->ok()) << queued->error.message;
		}
	};
	harness.run(scenario(),
				[&]
				{
					pool.shutdown();
				});
	EXPECT_EQ(harness.accepts, 1U);
	EXPECT_EQ(harness.handshakes, 1U);
}

TEST(Http2PoolSchedulingTest, ShutdownCannotSendAQueuedGrantWhoseCompletionWasDelayedUntilAfterClose)
{
	H2Harness harness;
	harness.serverOptions.maxConcurrentStreams = 1;
	harness.onH2Request = [](const H2Harness::Request& request)
	{
		if (request.target != "/held")
		{
			request.respond();
		}
	};
	HttpClientPool pool(harness.io);
	const auto options = h2Options();
	auto hook = std::make_shared<CompletionHook>();
	const HookExecutor executor(harness.io.get_executor(), hook);
	std::optional<HttpClientResult> held, cancelled;
	auto scenario = [&]() -> Awaitable<void>
	{
		harness.launch(pool.get("127.0.0.1", harness.port(), "/held", {}, options), held);
		if (!co_await harness.waitFor(
				[&]
				{
					return harness.received.size() == 1;
				}))
		{
			co_return;
		}
		boost::asio::co_spawn(executor,
							  pool.get("127.0.0.1", harness.port(), "/cancelled-effect", {}, options),
							  [&](std::exception_ptr error, HttpClientResult result)
							  {
								  EXPECT_FALSE(error);
								  cancelled = std::move(result);
							  });
		if (!co_await harness.waitFor(
				[&]
				{
					return pool.stats().queuedRequests == 1;
				}))
		{
			co_return;
		}
		hook->armed = true;
		harness.respond("/held");
		if (!co_await harness.waitFor(
				[&]
				{
					return hook->fired && held.has_value();
				}))
		{
			co_return;
		}
		EXPECT_TRUE(held->ok()) << held->error.message;
		EXPECT_FALSE(cancelled.has_value());
		EXPECT_EQ(pool.stats().queuedRequests, 0U);
		EXPECT_EQ(pool.stats().activeRequests, 1U);
		EXPECT_EQ(harness.received.size(), 1U);
		pool.shutdown();
		if (!co_await harness.waitFor(
				[&]
				{
					return harness.joined == 1;
				}))
		{
			co_return;
		}
		// 对端收发任务退出说明关闭已执行；新代次重连仍须等客户端归还旧独占门。
		hook->resume();
		EXPECT_TRUE(co_await harness.waitFor(
			[&]
			{
				return cancelled.has_value();
			}));
		if (cancelled)
		{
			EXPECT_EQ(cancelled->error.code, ErrorCode::hOperationAborted);
			EXPECT_EQ(cancelled->requestDisposition, HttpRequestDisposition::hNotSent);
		}
		EXPECT_EQ(pool.stats().queuedRequests, 0U);
		EXPECT_EQ(pool.stats().activeRequests, 0U);
	};
	harness.run(scenario(),
				[&]
				{
					pool.shutdown();
					hook->armed = false;
					hook->resume();
				});
	EXPECT_EQ(harness.accepts, 1U);
	EXPECT_EQ(harness.handshakes, 1U);
	EXPECT_EQ(harness.received.size(), 1U);
}

TEST(Http2PoolSchedulingTest, AQueuedWaiterKeepsItsOwnConnectTimeoutWhenTheBusyTransportDisconnects)
{
	H2Harness harness;
	harness.serverOptions.maxConcurrentStreams = 1;
	harness.onH2Request = [](const H2Harness::Request& request)
	{
		if (request.target != "/held-post")
		{
			request.respond();
		}
	};
	HttpClientPool pool(harness.io);
	auto longOptions = h2Options();
	longOptions.connectTimeout = std::chrono::seconds {2};
	longOptions.requestTimeout = std::chrono::seconds {3};
	auto shortOptions = longOptions;
	shortOptions.connectTimeout = std::chrono::milliseconds {20};
	std::optional<HttpClientResult> held, longRequest, shortRequest;
	auto scenario = [&]() -> Awaitable<void>
	{
		// 忙流用 POST，断连后不会自动重放并占用重连位置。
		harness.launch(pool.post("127.0.0.1", harness.port(), "/held-post", "effect", "text/plain", longOptions), held);
		if (!co_await harness.waitFor(
				[&]
				{
					return harness.received.size() == 1;
				}))
		{
			co_return;
		}
		harness.launch(pool.get("127.0.0.1", harness.port(), "/long-reconnect", {}, longOptions), longRequest);
		if (!co_await harness.waitFor(
				[&]
				{
					return pool.stats().queuedRequests == 1;
				}))
		{
			co_return;
		}
		harness.launch(pool.get("127.0.0.1", harness.port(), "/short-reconnect", {}, shortOptions), shortRequest);
		if (!co_await harness.waitFor(
				[&]
				{
					return pool.stats().queuedRequests == 2;
				}))
		{
			co_return;
		}
		harness.holdHandshakes = true;
		harness.peers.front()->connection->close();
		if (!co_await harness.waitFor(
				[&]
				{
					return held.has_value() && harness.accepts == 2 && harness.hellos == 1;
				}))
		{
			co_return;
		}
		// 新 ClientHello 已到而握手仍被挡住；等待短调用完成，最多观察 150ms。
		const auto observationEnd = Clock::now() + std::chrono::milliseconds {150};
		EXPECT_TRUE(co_await harness.waitFor(
			[&]
			{
				return shortRequest.has_value() || Clock::now() >= observationEnd;
			}));
		EXPECT_TRUE(shortRequest.has_value()) << "原本等待流槽位的调用也必须使用自己的短建连预算";
		EXPECT_FALSE(longRequest.has_value());
		EXPECT_EQ(harness.received.size(), 1U);
		if (shortRequest)
		{
			EXPECT_EQ(shortRequest->error.code, ErrorCode::hTimedOut);
			EXPECT_EQ(shortRequest->requestDisposition, HttpRequestDisposition::hNotSent);
		}
		// 断言失败也放行握手并等请求完成，避免由 watchdog 中断测试。
		harness.releaseHandshakes();
		EXPECT_TRUE(co_await harness.waitFor(
			[&]
			{
				return longRequest.has_value() && shortRequest.has_value();
			}));
		if (longRequest)
		{
			EXPECT_TRUE(longRequest->ok()) << longRequest->error.message;
			EXPECT_EQ(longRequest->body, "/long-reconnect");
		}
		if (shortRequest)
		{
			EXPECT_EQ(shortRequest->error.code, ErrorCode::hTimedOut);
			EXPECT_EQ(shortRequest->requestDisposition, HttpRequestDisposition::hNotSent);
		}
	};
	harness.run(scenario(),
				[&]
				{
					pool.shutdown();
				});
	EXPECT_EQ(harness.accepts, 2U);
	EXPECT_EQ(harness.handshakes, 2U);
	ASSERT_EQ(harness.received.size(), 2U);
	EXPECT_EQ(harness.received[0].target, "/held-post");
	EXPECT_EQ(harness.received[1].target, "/long-reconnect");
}

TEST(Http2PoolSchedulingTest, AGrantedRequestKeepsItsOwnConnectTimeoutAfterTheAssignedTransportDisconnects)
{
	H2Harness harness;
	harness.serverOptions.maxConcurrentStreams = 1;
	harness.onH2Request = [](const H2Harness::Request& request)
	{
		if (request.target != "/held")
		{
			request.respond();
		}
	};
	HttpClientPool pool(harness.io);
	auto options = h2Options();
	options.connectTimeout = std::chrono::seconds {2};
	options.requestTimeout = std::chrono::seconds {3};
	auto shortOptions = options;
	shortOptions.connectTimeout = std::chrono::milliseconds {20};
	auto hook = std::make_shared<CompletionHook>();
	const HookExecutor executor(harness.io.get_executor(), hook);
	std::optional<HttpClientResult> held, shortRequest;
	auto scenario = [&]() -> Awaitable<void>
	{
		harness.launch(pool.get("127.0.0.1", harness.port(), "/held", {}, options), held);
		if (!co_await harness.waitFor(
				[&]
				{
					return harness.received.size() == 1;
				}))
		{
			co_return;
		}
		boost::asio::co_spawn(
			executor,
			pool.post("127.0.0.1", harness.port(), "/short-effect", "effect", "text/plain", shortOptions),
			[&](std::exception_ptr error, HttpClientResult result)
			{
				EXPECT_FALSE(error);
				shortRequest = std::move(result);
			});
		if (!co_await harness.waitFor(
				[&]
				{
					return pool.stats().queuedRequests == 1;
				}))
		{
			co_return;
		}
		hook->armed = true;
		harness.respond("/held");
		if (!co_await harness.waitFor(
				[&]
				{
					return hook->fired && held.has_value();
				}))
		{
			co_return;
		}
		EXPECT_TRUE(held->ok()) << held->error.message;
		EXPECT_EQ(pool.stats().queuedRequests, 0U);
		EXPECT_EQ(pool.stats().activeRequests, 1U);
		EXPECT_FALSE(shortRequest.has_value());
		harness.holdHandshakes = true;
		harness.peers.front()->connection->close();
		if (!co_await harness.waitFor(
				[&]
				{
					return harness.joined == 1 && pool.stats().connectingConnections == 1;
				}))
		{
			co_return;
		}
		// 收发任务已退出，客户端已收到断连通知；恢复旧连接上已获预留但尚未发送的请求。
		hook->resume();
		if (!co_await harness.waitFor(
				[&]
				{
					return harness.hellos == 1;
				}))
		{
			co_return;
		}
		boost::asio::steady_timer observation(harness.io, Clock::now() + std::chrono::milliseconds {100});
		co_await observation.async_wait(boost::asio::use_awaitable);
		EXPECT_TRUE(shortRequest.has_value()) << "旧传输断连后的新握手必须使用当前调用的短建连预算";
		if (shortRequest)
		{
			EXPECT_EQ(shortRequest->error.code, ErrorCode::hTimedOut);
			EXPECT_EQ(shortRequest->requestDisposition, HttpRequestDisposition::hNotSent);
		}
		EXPECT_EQ(harness.received.size(), 1U);
		// 放行握手并等待完整结果，结合入站记录检查超时请求是否被发送。
		harness.releaseHandshakes();
		EXPECT_TRUE(co_await harness.waitFor(
			[&]
			{
				return shortRequest.has_value();
			}));
		if (shortRequest)
		{
			EXPECT_EQ(shortRequest->error.code, ErrorCode::hTimedOut);
		}
	};
	harness.run(scenario(),
				[&]
				{
					pool.shutdown();
					hook->armed = false;
					hook->resume();
				});
	EXPECT_EQ(harness.accepts, 2U);
	EXPECT_EQ(harness.handshakes, 1U);
	EXPECT_EQ(harness.received.size(), 1U);
}

TEST(Http2PoolRetryTest, ARefusedGetIsRetriedOnceOnTheHealthyMultiplexedConnection)
{
	H2Harness harness;
	harness.onH2Request = [&](const H2Harness::Request& request)
	{
		if (harness.received.size() == 1)
		{
			request.peer->reset(request.id, detail::Http2Code::hRefusedStream);
		}
		else
		{
			request.respond();
		}
	};
	HttpClientPool pool(harness.io);
	const auto options = h2Options();
	auto scenario = [&]() -> Awaitable<void>
	{
		auto result = co_await pool.get("127.0.0.1", harness.port(), "/retry", {}, options);
		EXPECT_TRUE(result.ok()) << result.error.message;
		EXPECT_EQ(result.body, "/retry");
		EXPECT_FALSE(result.error.http2.has_value());
		EXPECT_EQ(pool.stats().activeRequests, 0U);
		EXPECT_EQ(pool.stats().queuedRequests, 0U);
	};
	harness.run(scenario(),
				[&]
				{
					pool.shutdown();
				});
	ASSERT_EQ(harness.received.size(), 2U);
	EXPECT_EQ(harness.accepts, 1U);
	EXPECT_EQ(harness.received[0].id, 1);
	EXPECT_EQ(harness.received[1].id, 3);
	EXPECT_EQ(harness.received[0].peer, harness.received[1].peer);
}

TEST(Http2PoolRetryTest, ConsecutiveRefusalsStopAfterTwoAttemptsAndKeepTheLastRawStreamError)
{
	H2Harness harness;
	harness.onH2Request = [](const H2Harness::Request& request)
	{
		request.peer->reset(request.id, detail::Http2Code::hRefusedStream);
	};
	HttpClientPool pool(harness.io);
	const auto options = h2Options();
	auto scenario = [&]() -> Awaitable<void>
	{
		auto result = co_await pool.get("127.0.0.1", harness.port(), "/refused", {}, options);
		EXPECT_EQ(result.error.code, ErrorCode::hHttp2StreamError);
		EXPECT_EQ(result.requestDisposition, HttpRequestDisposition::hNotProcessed);
		EXPECT_TRUE(result.error.http2.has_value());
		if (result.error.http2)
		{
			EXPECT_EQ(result.error.http2->code, 7U);
			EXPECT_EQ(result.error.http2->streamId, 3);
		}
	};
	harness.run(scenario(),
				[&]
				{
					pool.shutdown();
				});
	EXPECT_EQ(harness.accepts, 1U);
	EXPECT_EQ(harness.received.size(), 2U);
}

TEST(Http2PoolRetryTest, ARefusedPutReplaysTheOwnedBodyAfterTheCallersStorageHasGone)
{
	H2Harness harness;
	harness.onH2Request = [&](const H2Harness::Request& request)
	{
		if (harness.received.size() == 1)
		{
			request.peer->reset(request.id, detail::Http2Code::hRefusedStream);
		}
		else
		{
			request.respond();
		}
	};
	HttpClientPool pool(harness.io);
	const auto options = h2Options();
	static constexpr char kPayloadBytes[] = "owned put body with an embedded byte\0tail";
	constexpr std::string_view kPayload {kPayloadBytes, sizeof(kPayloadBytes) - 1};
	auto submitted = [&]
	{
		std::string body(kPayload);
		std::string target = "/replace";
		return pool.request("127.0.0.1", harness.port(), HttpMethod::hPut, target, body, {}, options);
	}();
	auto scenario = [&]() -> Awaitable<void>
	{
		auto result = co_await std::move(submitted);
		EXPECT_TRUE(result.ok()) << result.error.message;
	};
	harness.run(scenario(),
				[&]
				{
					pool.shutdown();
				});
	ASSERT_EQ(harness.received.size(), 2U);
	EXPECT_EQ(harness.received[0].body, kPayload);
	EXPECT_EQ(harness.received[1].body, kPayload);
	EXPECT_EQ(harness.received[0].peer->headers[1].find(":method"), "PUT");
	EXPECT_EQ(harness.received[1].peer->headers[3].find(":method"), "PUT");
}

TEST(Http2PoolRetryTest, RefusedNonIdempotentMethodsAndProcessedPostDisconnectAreNeverReplayed)
{
	for (const auto method : {HttpMethod::hPost, HttpMethod::hPatch})
	{
		H2Harness harness;
		harness.onH2Request = [](const H2Harness::Request& request)
		{
			request.peer->reset(request.id, detail::Http2Code::hRefusedStream);
		};
		HttpClientPool pool(harness.io);
		const auto options = h2Options();
		auto scenario = [&]() -> Awaitable<void>
		{
			auto result = co_await pool.request("127.0.0.1", harness.port(), method, "/refused", "effect", {}, options);
			EXPECT_EQ(result.error.code, ErrorCode::hHttp2StreamError);
			EXPECT_EQ(result.requestDisposition, HttpRequestDisposition::hNotProcessed);
		};
		harness.run(scenario(),
					[&]
					{
						pool.shutdown();
					});
		EXPECT_EQ(harness.received.size(), 1U);
		EXPECT_EQ(harness.accepts, 1U);
	}
	H2Harness harness;
	size_t sideEffects = 0;
	harness.onH2Request = [&](const H2Harness::Request& request)
	{
		++sideEffects;
		boost::asio::post(harness.io,
						  [peer = request.peer]
						  {
							  peer->connection->close();
						  });
	};
	HttpClientPool pool(harness.io);
	const auto options = h2Options();
	auto scenario = [&]() -> Awaitable<void>
	{
		auto result = co_await pool.post("127.0.0.1", harness.port(), "/processed", "effect", "text/plain", options);
		EXPECT_TRUE(result.error);
		EXPECT_EQ(result.status, 0);
		EXPECT_EQ(result.requestDisposition, HttpRequestDisposition::hPossiblyProcessed);
	};
	harness.run(scenario(),
				[&]
				{
					pool.shutdown();
				});
	EXPECT_EQ(sideEffects, 1U);
	EXPECT_EQ(harness.received.size(), 1U);
	EXPECT_EQ(harness.accepts, 1U);
}

TEST(Http2PoolRetryTest, AnOrdinaryResetOrConnectionEofDoesNotProveThatAGetWasUnprocessed)
{
	for (const bool reset : {false, true})
	{
		H2Harness harness;
		harness.onH2Request = [&](const H2Harness::Request& request)
		{
			if (reset)
			{
				request.peer->reset(request.id, detail::Http2Code::hCancel);
			}
			else
			{
				boost::asio::post(harness.io,
								  [peer = request.peer]
								  {
									  peer->connection->close();
								  });
			}
		};
		HttpClientPool pool(harness.io);
		const auto options = h2Options();
		auto scenario = [&]() -> Awaitable<void>
		{
			auto result = co_await pool.get("127.0.0.1", harness.port(), "/uncertain", {}, options);
			EXPECT_TRUE(result.error);
			EXPECT_EQ(result.requestDisposition, HttpRequestDisposition::hPossiblyProcessed);
			if (reset)
			{
				EXPECT_TRUE(result.error.http2.has_value());
				if (result.error.http2)
				{
					EXPECT_EQ(result.error.http2->code, 8U);
				}
			}
		};
		harness.run(scenario(),
					[&]
					{
						pool.shutdown();
					});
		EXPECT_EQ(harness.received.size(), 1U);
		EXPECT_EQ(harness.accepts, 1U);
	}
}

TEST(Http2PoolRetryTest, GoawayDrainsStreamOneAndRetriesStreamThreeOnANewConnectionWithAMaximumOfOne)
{
	H2Harness harness;
	H2Peer* original = nullptr;
	bool sentGoaway = false;
	harness.onH2Request = [&](const H2Harness::Request& request)
	{
		if (request.target == "/drain")
		{
			original = request.peer;
		}
		else if (request.peer == original)
		{
			request.peer->goaway(1);
			sentGoaway = true;
		}
		else
		{
			request.respond();
		}
	};
	HttpClientPool pool(harness.io);
	const auto options = h2Options();
	std::optional<HttpClientResult> draining, retried;
	auto scenario = [&]() -> Awaitable<void>
	{
		harness.launch(pool.get("127.0.0.1", harness.port(), "/drain", {}, options), draining);
		if (!co_await harness.waitFor(
				[&]
				{
					return original != nullptr;
				}))
		{
			co_return;
		}
		harness.launch(pool.get("127.0.0.1", harness.port(), "/migrate", {}, options), retried);
		if (!co_await harness.waitFor(
				[&]
				{
					return sentGoaway;
				}))
		{
			co_return;
		}
		EXPECT_FALSE(draining.has_value());
		harness.respond("/drain");
		EXPECT_TRUE(co_await harness.waitFor(
			[&]
			{
				return draining && retried;
			}));
		if (draining && retried)
		{
			EXPECT_TRUE(draining->ok()) << draining->error.message;
			EXPECT_TRUE(retried->ok()) << retried->error.message;
			EXPECT_EQ(draining->body, "/drain");
			EXPECT_EQ(retried->body, "/migrate");
		}
		EXPECT_EQ(pool.stats().activeRequests, 0U);
		EXPECT_EQ(pool.stats().queuedRequests, 0U);
	};
	harness.run(scenario(),
				[&]
				{
					pool.shutdown();
				});
	ASSERT_EQ(harness.received.size(), 3U);
	EXPECT_EQ(harness.accepts, 2U);
	EXPECT_EQ(harness.received[0].id, 1);
	EXPECT_EQ(harness.received[1].id, 3);
	EXPECT_EQ(harness.received[2].id, 1);
	EXPECT_NE(harness.received[1].peer, harness.received[2].peer);
}

TEST(Http2PoolRetryTest, DecreasingGoawayLastIdKeepsTheFinalRawErrorAndDoesNotReplayPost)
{
	H2Harness harness;
	harness.onH2Request = [](const H2Harness::Request& request)
	{
		if (request.target == "/post")
		{
			request.peer->goaway(request.id);
			request.peer->goaway(1, detail::Http2Code::hEnhanceYourCalm);
		}
	};
	HttpClientPool pool(harness.io);
	const auto options = h2Options();
	std::optional<HttpClientResult> draining, refused;
	auto scenario = [&]() -> Awaitable<void>
	{
		harness.launch(pool.get("127.0.0.1", harness.port(), "/drain", {}, options), draining);
		if (!co_await harness.waitFor(
				[&]
				{
					return harness.received.size() == 1;
				}))
		{
			co_return;
		}
		harness.launch(pool.post("127.0.0.1", harness.port(), "/post", "effect", "text/plain", options), refused);
		if (!co_await harness.waitFor(
				[&]
				{
					return refused.has_value();
				}))
		{
			co_return;
		}
		EXPECT_EQ(refused->requestDisposition, HttpRequestDisposition::hNotProcessed);
		EXPECT_EQ(refused->error.code, ErrorCode::hHttp2ConnectionError);
		EXPECT_TRUE(refused->error.http2.has_value());
		if (refused->error.http2)
		{
			EXPECT_EQ(refused->error.http2->code, 11U);
			EXPECT_EQ(refused->error.http2->streamId, 0);
		}
		harness.respond("/drain");
		EXPECT_TRUE(co_await harness.waitFor(
			[&]
			{
				return draining.has_value();
			}));
	};
	harness.run(scenario(),
				[&]
				{
					pool.shutdown();
				});
	EXPECT_EQ(harness.accepts, 1U);
	EXPECT_EQ(harness.received.size(), 2U);
}

TEST(Http2PoolRetryTest, NegotiatingHttp1FallbackDoesNotReplayAProcessedReusedPost)
{
	Harness harness(true);
	harness.disconnectAfterRequest = [](size_t count)
	{
		return count == 2;
	};
	HttpClientPool pool(harness.io);
	auto options = h2Options();
	options.protocolMode = HttpProtocolMode::hNegotiateHttp2;
	auto scenario = [&]() -> Awaitable<void>
	{
		EXPECT_TRUE((co_await pool.get("127.0.0.1", harness.port(), "/warmup", {}, options)).ok());
		auto result = co_await pool.post("127.0.0.1", harness.port(), "/side-effect", "effect", "text/plain", options);
		EXPECT_TRUE(result.error);
		EXPECT_EQ(result.protocol, HttpProtocol::hHttp1);
		EXPECT_EQ(result.requestDisposition, HttpRequestDisposition::hPossiblyProcessed);
	};
	harness.run(scenario(),
				[&]
				{
					pool.shutdown();
				});
	EXPECT_EQ(harness.requests, 2U);
	EXPECT_EQ(harness.accepts, 1U);
}

namespace
{
	enum class RetryInterruption
	{
		hCancel,
		hDeadline,
		hShutdown
	};

	void checkInterruptedRetry(RetryInterruption interruption)
	{
		H2Harness harness;
		harness.onH2Request = [](const H2Harness::Request& request)
		{
			request.peer->reset(request.id, detail::Http2Code::hRefusedStream);
		};
		HttpClientPool pool(harness.io);
		auto options = h2Options();
		options.requestTimeout = std::chrono::milliseconds {200};
		auto hook = std::make_shared<CompletionHook>();
		hook->armed = true;
		hook->ready = [&]
		{
			return harness.received.size() == 1 && pool.stats().activeRequests == 0;
		};
		const HookExecutor executor(harness.io.get_executor(), hook);
		boost::asio::cancellation_signal cancellation;
		std::optional<HttpClientResult> result;
		auto scenario = [&]() -> Awaitable<void>
		{
			boost::asio::co_spawn(
				executor,
				pool.get("127.0.0.1", harness.port(), "/interrupted", {}, options),
				boost::asio::bind_cancellation_slot(cancellation.slot(),
													[&](std::exception_ptr error, HttpClientResult value)
													{
														EXPECT_FALSE(error);
														result = std::move(value);
													}));
			if (!co_await harness.waitFor(
					[&]
					{
						return hook->fired;
					}))
			{
				co_return;
			}
			EXPECT_EQ(harness.received.size(), 1U);
			EXPECT_FALSE(result.has_value());
			if (interruption == RetryInterruption::hCancel)
			{
				cancellation.emit(boost::asio::cancellation_type::terminal);
			}
			else if (interruption == RetryInterruption::hShutdown)
			{
				pool.shutdown();
			}
			else
			{
				boost::asio::steady_timer untilExpired(harness.io, Clock::now() + std::chrono::milliseconds {250});
				co_await untilExpired.async_wait(boost::asio::use_awaitable);
			}
			hook->resume();
			EXPECT_TRUE(co_await harness.waitFor(
				[&]
				{
					return result.has_value();
				}));
			if (result)
			{
				EXPECT_EQ(result->error.code,
						  interruption == RetryInterruption::hDeadline ? ErrorCode::hTimedOut
																	   : ErrorCode::hOperationAborted);
				EXPECT_EQ(result->requestDisposition, HttpRequestDisposition::hNotSent);
				EXPECT_FALSE(result->error.http2.has_value());
				EXPECT_EQ(result->status, 0);
			}
			EXPECT_EQ(pool.stats().activeRequests, 0U);
			EXPECT_EQ(pool.stats().queuedRequests, 0U);
		};
		harness.run(scenario(),
					[&]
					{
						pool.shutdown();
						hook->armed = false;
						hook->resume();
					});
		EXPECT_EQ(harness.received.size(), 1U);
		EXPECT_EQ(harness.accepts, 1U);
	}
} // namespace

TEST(Http2PoolRetryTest, NativeCancellationAfterARefusalPreventsTheRetryFromReachingTheWire)
{
	checkInterruptedRetry(RetryInterruption::hCancel);
}

TEST(Http2PoolRetryTest, TheOriginalDeadlineStillAppliesAfterARefusal)
{
	checkInterruptedRetry(RetryInterruption::hDeadline);
}

TEST(Http2PoolRetryTest, ShutdownAfterARefusalPreventsTheRetryFromReachingTheWire)
{
	checkInterruptedRetry(RetryInterruption::hShutdown);
}

TEST(Http2PoolRetryTest, ReenteringThePoolQueueAfterARefusalKeepsTheFirstQueueDeadline)
{
	H2Harness harness;
	harness.serverOptions.maxConcurrentStreams = 1;
	HttpClientPool pool(harness.io);
	auto options = h2Options();
	options.requestTimeout = std::chrono::seconds {2};
	options.http2.queueTimeout = std::chrono::milliseconds {200};
	std::optional<HttpClientResult> held, retried, blocker;
	harness.onH2Request = [&](const H2Harness::Request& request)
	{
		if (request.target == "/retry")
		{
			request.peer->reset(request.id, detail::Http2Code::hRefusedStream);
			// 先排入竞争者；原请求重试时必须重新排队，不能越过已经等候的调用。
			harness.launch(pool.get("127.0.0.1", harness.port(), "/blocker", {}, options), blocker);
		}
	};
	auto scenario = [&]() -> Awaitable<void>
	{
		harness.launch(pool.get("127.0.0.1", harness.port(), "/held", {}, options), held);
		if (!co_await harness.waitFor(
				[&]
				{
					return harness.received.size() == 1;
				}))
		{
			co_return;
		}
		harness.launch(pool.get("127.0.0.1", harness.port(), "/retry", {}, options), retried);
		if (!co_await harness.waitFor(
				[&]
				{
					return pool.stats().queuedRequests == 1;
				}))
		{
			co_return;
		}
		const auto queuedAt = Clock::now();
		boost::asio::steady_timer releaseFirst(harness.io, queuedAt + std::chrono::milliseconds {150});
		co_await releaseFirst.async_wait(boost::asio::use_awaitable);
		harness.respond("/held");
		if (!co_await harness.waitFor(
				[&]
				{
					return harness.received.size() >= 3;
				}))
		{
			co_return;
		}
		boost::asio::steady_timer observation(harness.io, queuedAt + std::chrono::milliseconds {300});
		co_await observation.async_wait(boost::asio::use_awaitable);
		EXPECT_TRUE(retried.has_value()) << "重试不得从第二次入队重新获得完整 queueTimeout";
		if (retried)
		{
			EXPECT_EQ(retried->error.code, ErrorCode::hTimedOut);
			EXPECT_EQ(retried->requestDisposition, HttpRequestDisposition::hNotSent);
			EXPECT_FALSE(retried->error.http2.has_value());
		}
		harness.respond("/blocker");
		EXPECT_TRUE(co_await harness.waitFor(
			[&]
			{
				return held && retried && blocker;
			}));
		EXPECT_EQ(pool.stats().activeRequests, 0U);
		EXPECT_EQ(pool.stats().queuedRequests, 0U);
	};
	harness.run(scenario(),
				[&]
				{
					pool.shutdown();
				});
	EXPECT_EQ(harness.received.size(), 3U);
	EXPECT_EQ(harness.accepts, 1U);
}

TEST(Http2PoolRetryTest, DestructionCancelsAnActiveStreamAndItsQueueWithoutDependingOnTheWatchdog)
{
	H2Harness harness;
	harness.serverOptions.maxConcurrentStreams = 1;
	harness.onH2Request = [](const H2Harness::Request&)
	{
	};
	auto pool = std::make_unique<HttpClientPool>(harness.io);
	auto options = h2Options();
	options.connectTimeout = Clock::duration::zero();
	options.requestTimeout = Clock::duration::zero();
	options.http2.queueTimeout = std::chrono::seconds {30};
	std::optional<HttpClientResult> active, queued;
	auto scenario = [&]() -> Awaitable<void>
	{
		harness.launch(pool->get("127.0.0.1", harness.port(), "/active", {}, options), active);
		if (!co_await harness.waitFor(
				[&]
				{
					return harness.received.size() == 1;
				}))
		{
			co_return;
		}
		harness.launch(pool->get("127.0.0.1", harness.port(), "/queued", {}, options), queued);
		if (!co_await harness.waitFor(
				[&]
				{
					return pool->stats().queuedRequests == 1;
				}))
		{
			co_return;
		}
		pool.reset();
		EXPECT_TRUE(co_await harness.waitFor(
			[&]
			{
				return active && queued && harness.joined == 1;
			}));
		if (active && queued)
		{
			EXPECT_EQ(active->error.code, ErrorCode::hOperationAborted);
			EXPECT_EQ(queued->error.code, ErrorCode::hOperationAborted);
			EXPECT_EQ(queued->requestDisposition, HttpRequestDisposition::hNotSent);
		}
	};
	harness.run(scenario(),
				[&]
				{
					pool.reset();
				});
	EXPECT_EQ(harness.received.size(), 1U);
	EXPECT_EQ(harness.accepts, 1U);
	EXPECT_EQ(harness.joined, 1U);
}
#endif
