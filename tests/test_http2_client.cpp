/**
 * @file test_http2_client.cpp
 * @brief 客户端参数持有、共享建连和对象生命周期测试
 */

#include "asio/HttpClient.h"
#ifdef HICAL_HAS_HTTP2
	#include "asio/Http2ClientSession.h"
	#include "core/http2/Http2Receive.h"
#endif
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/read_until.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/asio/use_future.hpp>
#include <gtest/gtest.h>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

using namespace hical;

namespace
{
	using Clock = std::chrono::steady_clock;
	using Tcp = boost::asio::ip::tcp;
	using TlsSocket = boost::asio::ssl::stream<Tcp::socket>;
	constexpr auto kTestTimeout = std::chrono::seconds {5};
	constexpr size_t kMaxRequestBytes = 1024 * 1024;

	struct CompletionHook
	{
		bool armed {false};
		bool fired {false};
		std::function<void()> emit;
	};

	/** 仅包装客户端 completion 的执行时机；底层始终是同一个 io_context，不替换网络 I/O。 */
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
				[hook = hook_, function = std::forward<Function>(function)]() mutable
				{
					if (hook->armed)
					{
						hook->armed = false;
						hook->fired = true;
						hook->emit();
					}
					function();
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

	std::string headerValue(std::string_view head, std::string_view name)
	{
		auto offset = head.find("\r\n") + 2;
		while (offset < head.size())
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
		std::shared_ptr<SslContext> serverTlsContext;
		std::function<void()> onRequest;
		std::function<void()> onTimeout;
		std::function<void()> onStalledHandshake;
		std::function<void()> onTlsHandshake;
		size_t accepts {0};
		size_t requests {0};
		size_t handshakes {0};
		bool holdResponses {false};
		bool holdFirstResponseBody {false};
		bool stallHandshake {false};
		bool holdTlsAfterHandshake {false};
		bool timedOut {false};
		bool stopped {false};

		uint16_t port() const
		{
			return acceptor.local_endpoint().port();
		}

		void release()
		{
			holdResponses = false;
			responseRelease.cancel();
		}

		void stop()
		{
			stopped = true;
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
			release();
			watchdog.cancel();
		}

		template <typename SocketType>
		Awaitable<void> serve(std::shared_ptr<SocketType> socket, size_t connectionId)
		{
			std::string buffer;
			boost::system::error_code error;
			for (;;)
			{
				const auto headerBytes = co_await boost::asio::async_read_until(
					*socket,
					boost::asio::dynamic_buffer(buffer, kMaxRequestBytes),
					"\r\n\r\n",
					boost::asio::redirect_error(boost::asio::use_awaitable, error));
				if (error)
				{
					co_return;
				}
				const std::string head = buffer.substr(0, headerBytes);
				const auto contentLength = headerValue(head, "Content-Length");
				size_t bodyLength = 0;
				if (!contentLength.empty())
				{
					const auto parsed =
						std::from_chars(contentLength.data(), contentLength.data() + contentLength.size(), bodyLength);
					if (parsed.ec != std::errc {} || bodyLength > kMaxRequestBytes - headerBytes)
					{
						co_return;
					}
				}
				if (buffer.size() < headerBytes + bodyLength)
				{
					co_await boost::asio::async_read(
						*socket,
						boost::asio::dynamic_buffer(buffer, kMaxRequestBytes),
						boost::asio::transfer_exactly(headerBytes + bodyLength - buffer.size()),
						boost::asio::redirect_error(boost::asio::use_awaitable, error));
					if (error)
					{
						co_return;
					}
				}
				const auto firstSpace = head.find(' ');
				const auto secondSpace = head.find(' ', firstSpace + 1);
				std::string body = head.substr(firstSpace + 1, secondSpace - firstSpace - 1) + "|"
								   + headerValue(head, "X-Owned") + "|" + buffer.substr(headerBytes, bodyLength) + "|"
								   + headerValue(head, "Content-Type");
				buffer.erase(0, headerBytes + bodyLength);
				++requests;
				if (onRequest)
				{
					onRequest();
				}
				if (holdResponses)
				{
					co_await responseRelease.async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, error));
				}
				std::string response = "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(body.size())
									   + "\r\nX-Connection: " + std::to_string(connectionId) + "\r\n\r\n";
				if (holdFirstResponseBody && requests == 1)
				{
					co_await boost::asio::async_write(*socket,
													  boost::asio::buffer(response),
													  boost::asio::redirect_error(boost::asio::use_awaitable, error));
					if (error)
					{
						co_return;
					}
					co_await responseRelease.async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, error));
					if (error)
					{
						co_return;
					}
					response.clear();
				}
				response.append(body);
				co_await boost::asio::async_write(*socket,
												  boost::asio::buffer(response),
												  boost::asio::redirect_error(boost::asio::use_awaitable, error));
				if (error)
				{
					co_return;
				}
			}
		}

		Awaitable<void> serveTls(std::shared_ptr<TlsSocket> socket, size_t connectionId)
		{
			boost::system::error_code error;
			if (stallHandshake)
			{
				std::array<char, 1> hello;
				co_await socket->next_layer().async_read_some(
					boost::asio::buffer(hello),
					boost::asio::redirect_error(boost::asio::use_awaitable, error));
				if (error || stopped)
				{
					co_return;
				}
				if (onStalledHandshake)
				{
					onStalledHandshake();
				}
				if (stopped)
				{
					co_return;
				}
				co_await responseRelease.async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, error));
				co_return;
			}
			co_await socket->async_handshake(boost::asio::ssl::stream_base::server,
											 boost::asio::redirect_error(boost::asio::use_awaitable, error));
			// 握手完成回调可能晚于 stop；先前的取消不会影响新注册的等待。
			if (error || stopped)
			{
				co_return;
			}
			++handshakes;
			if (onTlsHandshake)
			{
				onTlsHandshake();
			}
			if (stopped)
			{
				co_return;
			}
			if (holdTlsAfterHandshake)
			{
				co_await responseRelease.async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, error));
				co_return;
			}
			co_await serve(std::move(socket), connectionId);
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
				const auto connectionId = ++accepts;
				if (serverTlsContext)
				{
					auto tls = std::make_shared<TlsSocket>(std::move(*socket), serverTlsContext->native());
					tlsSockets.push_back(tls);
					coSpawn(io, serveTls(std::move(tls), connectionId));
				}
				else
				{
					coSpawn(io, serve(std::move(socket), connectionId));
				}
			}
		}

		Awaitable<std::vector<HttpClientResult>> concurrent(std::vector<Awaitable<HttpClientResult>> operations)
		{
			struct Group
			{
				Group(boost::asio::io_context& io, size_t count)
					: done(io, Clock::time_point::max()), results(count), pending(count)
				{
				}

				boost::asio::steady_timer done;
				std::vector<std::optional<HttpClientResult>> results;
				size_t pending;
				std::exception_ptr exception;
			};

			auto group = std::make_shared<Group>(io, operations.size());
			for (size_t index = 0; index < operations.size(); ++index)
			{
				boost::asio::co_spawn(io,
									  std::move(operations[index]),
									  [group, index](std::exception_ptr exception, HttpClientResult result)
									  {
										  if (exception)
										  {
											  group->exception = exception;
										  }
										  group->results[index] = std::move(result);
										  if (--group->pending == 0)
										  {
											  group->done.cancel();
										  }
									  });
			}
			if (group->pending != 0)
			{
				boost::system::error_code ignored;
				co_await group->done.async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, ignored));
			}
			if (group->exception)
			{
				std::rethrow_exception(group->exception);
			}
			std::vector<HttpClientResult> results;
			for (auto& result : group->results)
			{
				results.push_back(std::move(*result));
			}
			co_return results;
		}

		static Awaitable<void> execute(Harness& harness, Awaitable<void> scenario)
		{
			try
			{
				co_await std::move(scenario);
			}
			catch (...)
			{
				harness.stop();
				throw;
			}
			harness.stop();
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
						if (onTimeout)
						{
							onTimeout();
						}
						stop();
					}
				});
			coSpawn(io, accept());
			auto result = boost::asio::co_spawn(io, execute(*this, std::move(scenario)), boost::asio::use_future);
			io.run();
			EXPECT_FALSE(timedOut) << "客户端测试超过绝对截止点";
			EXPECT_NO_THROW(result.get());
		}
	};

	class Http2ClientTlsStateTest : public testing::Test
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

		static std::shared_ptr<SslContext> serverContext()
		{
			auto context = std::make_shared<SslContext>(boost::asio::ssl::context::tls_server);
			context->loadCertificate("test_server.crt");
			context->loadPrivateKey("test_server.key");
			return context;
		}

		static HttpClientOptions options()
		{
			HttpClientOptions result;
			result.tlsContext = std::make_shared<SslContext>(boost::asio::ssl::context::tls_client);
			result.verifyPeer = false;
			return result;
		}
	};
} // namespace

TEST(Http2ClientStateTest, RequestOwnsParametersBeforeAwaitableStarts)
{
	Harness harness;
	HttpClient client(harness.io, "127.0.0.1", harness.port());
	std::string target = "/original";
	std::string body = "original-body";
	HeaderMap headers;
	headers.set("X-Owned", "original-header");
	auto pending = client.request(HttpMethod::hPost, target, body, headers);
	target.assign("/mutated!");
	body.assign("mutated-body!");
	headers.set("X-Owned", "mutated-header!");
	auto scenario = [&]() -> Awaitable<void>
	{
		const auto result = co_await std::move(pending);
		EXPECT_TRUE(result.ok()) << result.error.message;
		EXPECT_EQ(result.body, "/original|original-header|original-body|");
	};
	harness.run(scenario());
}

TEST(Http2ClientStateTest, GetOwnsTargetAndHeadersBeforeAwaitableStarts)
{
	Harness harness;
	HttpClient client(harness.io, "127.0.0.1", harness.port());
	std::string target = "/original";
	HeaderMap headers;
	headers.set("X-Owned", "original-header");
	auto pending = client.get(target, headers);
	target.assign("/mutated!");
	headers.set("X-Owned", "mutated-header!");
	auto scenario = [&]() -> Awaitable<void>
	{
		const auto result = co_await std::move(pending);
		EXPECT_TRUE(result.ok()) << result.error.message;
		EXPECT_EQ(result.body, "/original|original-header||");
	};
	harness.run(scenario());
}

TEST(Http2ClientStateTest, PostOwnsContentTypeAndBodyBeforeAwaitableStarts)
{
	Harness harness;
	HttpClient client(harness.io, "127.0.0.1", harness.port());
	std::string target = "/original";
	std::string body = "original-body";
	std::string contentType = "application/original";
	auto pending = client.post(target, body, contentType);
	target.assign("/mutated!");
	body.assign("mutated-body!");
	contentType.assign("application/mutated!");
	auto scenario = [&]() -> Awaitable<void>
	{
		const auto result = co_await std::move(pending);
		EXPECT_TRUE(result.ok()) << result.error.message;
		EXPECT_EQ(result.body, "/original||original-body|application/original");
	};
	harness.run(scenario());
}

TEST(Http2ClientStateTest, ConcurrentFirstRequestsShareOneConnectionAndDoNotCrossResponses)
{
	Harness harness;
	HttpClient client(harness.io, "127.0.0.1", harness.port());
	harness.onTimeout = [&client]
	{
		client.close();
	};
	auto scenario = [&]() -> Awaitable<void>
	{
		std::vector<Awaitable<HttpClientResult>> operations;
		operations.push_back(client.get("/first"));
		operations.push_back(client.get("/second"));
		operations.push_back(client.post("/third", "body"));
		const auto results = co_await harness.concurrent(std::move(operations));
		const auto& first = results[0];
		const auto& second = results[1];
		const auto& third = results[2];
		EXPECT_TRUE(first.ok()) << first.error.message;
		EXPECT_TRUE(second.ok()) << second.error.message;
		EXPECT_TRUE(third.ok()) << third.error.message;
		EXPECT_EQ(first.body, "/first|||");
		EXPECT_EQ(second.body, "/second|||");
		EXPECT_EQ(third.body, "/third||body|application/json");
		EXPECT_EQ(first.headers.find("X-Connection"), second.headers.find("X-Connection"));
		EXPECT_EQ(first.headers.find("X-Connection"), third.headers.find("X-Connection"));
	};
	harness.run(scenario());
	EXPECT_EQ(harness.accepts, 1U);
	EXPECT_EQ(harness.requests, 3U);
}

TEST(Http2ClientStateTest, DestroyedFacadeMakesSavedAwaitableFailWithoutConnecting)
{
	Harness harness;
	auto client = std::make_unique<HttpClient>(harness.io, "127.0.0.1", harness.port());
	auto pending = client->get("/never-sent");
	client.reset();
	auto scenario = [&]() -> Awaitable<void>
	{
		const auto result = co_await std::move(pending);
		EXPECT_EQ(result.error.code, ErrorCode::hOperationAborted);
		EXPECT_EQ(result.requestDisposition, HttpRequestDisposition::hNotSent);
	};
	harness.run(scenario());
	EXPECT_EQ(harness.accepts, 0U);
}

TEST(Http2ClientStateTest, CloseInvalidatesSavedRequestAndAllowsANewGeneration)
{
	Harness harness;
	HttpClient client(harness.io, "127.0.0.1", harness.port());
	auto obsolete = client.get("/obsolete");
	client.close();
	auto current = client.get("/current");
	auto scenario = [&]() -> Awaitable<void>
	{
		EXPECT_EQ((co_await std::move(obsolete)).error.code, ErrorCode::hOperationAborted);
		const auto result = co_await std::move(current);
		EXPECT_TRUE(result.ok()) << result.error.message;
		EXPECT_EQ(result.body, "/current|||");
	};
	harness.run(scenario());
	EXPECT_EQ(harness.accepts, 1U);
	EXPECT_EQ(harness.requests, 1U);
}

TEST(Http2ClientStateTest, CloseCancelsActiveReadAndQueuedExchangeWithDisabledTimeouts)
{
	Harness harness;
	HttpClientOptions options;
	options.connectTimeout = Clock::duration::zero();
	options.requestTimeout = Clock::duration::zero();
	HttpClient client(harness.io, "127.0.0.1", harness.port(), options);
	harness.holdResponses = true;
	harness.onRequest = [&client]
	{
		client.close();
	};
	harness.onTimeout = [&client]
	{
		client.close();
	};
	auto scenario = [&]() -> Awaitable<void>
	{
		std::vector<Awaitable<HttpClientResult>> operations;
		operations.push_back(client.get("/first"));
		operations.push_back(client.get("/queued"));
		const auto results = co_await harness.concurrent(std::move(operations));
		EXPECT_EQ(results[0].error.code, ErrorCode::hOperationAborted);
		EXPECT_EQ(results[1].error.code, ErrorCode::hOperationAborted);
		EXPECT_FALSE(client.isAlive());
	};
	harness.run(scenario());
	EXPECT_EQ(harness.accepts, 1U);
	EXPECT_EQ(harness.requests, 1U);
}

TEST(Http2ClientStateTest, SavedPostOwnsTemporaryArgumentsAfterFullExpression)
{
	Harness harness;
	HttpClient client(harness.io, "127.0.0.1", harness.port());
	auto pending = client.post(std::string("/temporary"), std::string(256, 'b'), std::string("text/plain"));
	auto scenario = [&]() -> Awaitable<void>
	{
		const auto result = co_await std::move(pending);
		EXPECT_TRUE(result.ok()) << result.error.message;
		EXPECT_EQ(result.body, "/temporary||" + std::string(256, 'b') + "|text/plain");
	};
	harness.run(scenario());
}

TEST(Http2ClientStateTest, ForeignThreadFacadeDestructionCancelsAnActiveResponse)
{
	Harness harness;
	auto client = std::make_unique<HttpClient>(harness.io, "127.0.0.1", harness.port());
	auto pending = client->get("/waiting");
	harness.holdResponses = true;
	harness.onRequest = [&client]
	{
		std::thread destroy(
			[&client]
			{
				client.reset();
			});
		destroy.join();
	};
	auto scenario = [&]() -> Awaitable<void>
	{
		const auto result = co_await std::move(pending);
		EXPECT_EQ(result.error.code, ErrorCode::hOperationAborted);
	};
	harness.run(scenario());
	EXPECT_EQ(harness.requests, 1U);
}

TEST(Http2ClientStateTest, NativeCancellationAtSuccessfulIoCompletionReturnsCancelledAndDiscardsOldResponse)
{
	Harness harness;
	harness.holdFirstResponseBody = true;
	HttpClient client(harness.io, "127.0.0.1", harness.port());
	boost::asio::cancellation_signal cancellation;
	auto hook = std::make_shared<CompletionHook>();
	hook->emit = [&cancellation]
	{
		cancellation.emit(boost::asio::cancellation_type::terminal);
	};
	const HookExecutor executor(harness.io.get_executor(), hook);
	std::optional<HttpClientResult> cancelled;
	std::exception_ptr exception;
	harness.onTimeout = [&client]
	{
		client.close();
	};
	harness.onRequest = [&]
	{
		if (harness.requests == 1)
		{
			hook->armed = true;
		}
	};
	auto scenario = [&]() -> Awaitable<void>
	{
		boost::asio::co_spawn(executor,
							  client.get("/cancelled"),
							  boost::asio::bind_cancellation_slot(cancellation.slot(),
																  [&](std::exception_ptr error, HttpClientResult result)
																  {
																	  exception = error;
																	  cancelled = std::move(result);
																  }));
		while (!cancelled && !harness.timedOut)
		{
			co_await boost::asio::post(boost::asio::use_awaitable);
		}
		EXPECT_TRUE(hook->fired);
		EXPECT_FALSE(exception);
		EXPECT_FALSE(client.isAlive());
		if (cancelled)
		{
			EXPECT_EQ(cancelled->error.code, ErrorCode::hOperationAborted);
			EXPECT_EQ(cancelled->requestDisposition, HttpRequestDisposition::hPossiblyProcessed);
		}
		const auto next = co_await client.get("/next");
		EXPECT_TRUE(next.ok()) << next.error.message;
		EXPECT_EQ(next.body, "/next|||");
	};
	harness.run(scenario());
	EXPECT_EQ(harness.accepts, 2U);
}

TEST_F(Http2ClientTlsStateTest, ConcurrentFirstRequestsUseOneTlsHandshakeAndOneH1ExchangeAtATime)
{
	Harness harness;
	harness.serverTlsContext = serverContext();
	HttpClient client(harness.io, "127.0.0.1", harness.port(), options());
	harness.onTimeout = [&client]
	{
		client.close();
	};
	auto scenario = [&]() -> Awaitable<void>
	{
		std::vector<Awaitable<HttpClientResult>> operations;
		operations.push_back(client.get("/first"));
		operations.push_back(client.get("/second"));
		operations.push_back(client.post("/third", "body"));
		const auto results = co_await harness.concurrent(std::move(operations));
		EXPECT_TRUE(results[0].ok()) << results[0].error.message;
		EXPECT_TRUE(results[1].ok()) << results[1].error.message;
		EXPECT_TRUE(results[2].ok()) << results[2].error.message;
		EXPECT_EQ(results[0].body, "/first|||");
		EXPECT_EQ(results[1].body, "/second|||");
		EXPECT_EQ(results[2].body, "/third||body|application/json");
	};
	harness.run(scenario());
	EXPECT_EQ(harness.accepts, 1U);
	EXPECT_EQ(harness.handshakes, 1U);
	EXPECT_EQ(harness.requests, 3U);
}

TEST_F(Http2ClientTlsStateTest, CloseDuringActualClientHelloCancelsTheSharedHandshake)
{
	Harness harness;
	harness.serverTlsContext = serverContext();
	harness.stallHandshake = true;
	HttpClient client(harness.io, "127.0.0.1", harness.port(), options());
	harness.onStalledHandshake = [&client]
	{
		client.close();
	};
	harness.onTimeout = [&client]
	{
		client.close();
	};
	auto scenario = [&]() -> Awaitable<void>
	{
		std::vector<Awaitable<HttpClientResult>> operations;
		operations.push_back(client.get("/first"));
		operations.push_back(client.get("/second"));
		const auto results = co_await harness.concurrent(std::move(operations));
		EXPECT_EQ(results[0].error.code, ErrorCode::hOperationAborted);
		EXPECT_EQ(results[1].error.code, ErrorCode::hOperationAborted);
		EXPECT_EQ(results[0].requestDisposition, HttpRequestDisposition::hNotSent);
		EXPECT_FALSE(client.isAlive());
	};
	harness.run(scenario());
	EXPECT_EQ(harness.accepts, 1U);
	EXPECT_EQ(harness.handshakes, 0U);
	EXPECT_EQ(harness.requests, 0U);
}

TEST_F(Http2ClientTlsStateTest, FacadeDestructionDuringHandshakeDoesNotLeaveABorrowedTlsStream)
{
	Harness harness;
	harness.serverTlsContext = serverContext();
	harness.stallHandshake = true;
	auto client = std::make_unique<HttpClient>(harness.io, "127.0.0.1", harness.port(), options());
	auto pending = client->get("/waiting");
	harness.onStalledHandshake = [&client]
	{
		client.reset();
	};
	auto scenario = [&]() -> Awaitable<void>
	{
		const auto result = co_await std::move(pending);
		EXPECT_EQ(result.error.code, ErrorCode::hOperationAborted);
		EXPECT_EQ(result.requestDisposition, HttpRequestDisposition::hNotSent);
	};
	harness.run(scenario());
	EXPECT_EQ(harness.accepts, 1U);
	EXPECT_EQ(harness.handshakes, 0U);
}

TEST_F(Http2ClientTlsStateTest, NewGenerationWaitsForCancelledHandshakeAndSurvivesTheOldCloseTask)
{
	Harness harness;
	harness.serverTlsContext = serverContext();
	harness.stallHandshake = true;
	HttpClient client(harness.io, "127.0.0.1", harness.port(), options());
	std::optional<HttpClientResult> current;
	harness.onTimeout = [&client]
	{
		client.close();
	};
	harness.onStalledHandshake = [&]
	{
		client.close();
		harness.stallHandshake = false;
		boost::asio::co_spawn(harness.io,
							  client.get("/new-generation"),
							  [&](std::exception_ptr exception, HttpClientResult result)
							  {
								  EXPECT_FALSE(exception);
								  current = std::move(result);
							  });
	};
	auto scenario = [&]() -> Awaitable<void>
	{
		const auto obsolete = co_await client.get("/obsolete");
		EXPECT_EQ(obsolete.error.code, ErrorCode::hOperationAborted);
		while (!current && !harness.timedOut)
		{
			co_await boost::asio::post(boost::asio::use_awaitable);
		}
		if (current)
		{
			EXPECT_TRUE(current->ok()) << current->error.message;
			EXPECT_EQ(current->body, "/new-generation|||");
		}
	};
	harness.run(scenario());
	EXPECT_EQ(harness.accepts, 2U);
	EXPECT_EQ(harness.handshakes, 1U);
	EXPECT_EQ(harness.requests, 1U);
}

TEST_F(Http2ClientTlsStateTest, StopDuringHandshakeCompletionDoesNotLeaveAPendingWait)
{
	Harness harness;
	harness.serverTlsContext = serverContext();
	harness.holdTlsAfterHandshake = true;
	bool stoppedAfterHandshake = false;
	harness.onTlsHandshake = [&]
	{
		stoppedAfterHandshake = true;
		harness.stop();
	};
	const auto configuration = options();
	TlsSocket socket(harness.io, configuration.tlsContext->native());
	socket.set_verify_mode(boost::asio::ssl::verify_none);
	auto connect = [&]() -> Awaitable<void>
	{
		co_await socket.next_layer().async_connect(harness.acceptor.local_endpoint(), boost::asio::use_awaitable);
		boost::system::error_code ignored;
		co_await socket.async_handshake(boost::asio::ssl::stream_base::client,
										boost::asio::redirect_error(boost::asio::use_awaitable, ignored));
	};
	coSpawn(harness.io, harness.accept());
	auto client = boost::asio::co_spawn(harness.io, connect(), boost::asio::use_future);
	// 在握手完成回调里先停止，再让服务端继续，固定取消早于等待注册的顺序。
	harness.io.run_for(kTestTimeout);
	EXPECT_TRUE(stoppedAfterHandshake);
	EXPECT_TRUE(harness.io.stopped()) << "停止后仍有新注册的等待没有退出";

	// 失败时也清掉迟到的等待，避免夹具问题把整个测试进程挂住。
	boost::system::error_code ignored;
	socket.next_layer().close(ignored);
	harness.stop();
	harness.io.restart();
	harness.io.run_for(kTestTimeout);
	ASSERT_EQ(client.wait_for(std::chrono::seconds::zero()), std::future_status::ready);
	EXPECT_NO_THROW(client.get());
}

#ifdef HICAL_HAS_HTTP2

TEST_F(Http2ClientTlsStateTest, SavedSessionRunOwnsItsSocketUntilBothPumpsJoin)
{
	Harness harness;
	harness.serverTlsContext = serverContext();
	harness.holdTlsAfterHandshake = true;
	const auto configuration = options();
	std::weak_ptr<detail::Http2ClientSession> weakSession;
	std::weak_ptr<TlsSocket> weakSocket;
	bool completed = false;
	NetworkError result;
	harness.onTimeout = [&weakSession]
	{
		if (auto session = weakSession.lock())
		{
			session->close();
		}
	};
	auto scenario = [&]() -> Awaitable<void>
	{
		auto socket = std::make_shared<TlsSocket>(harness.io, configuration.tlsContext->native());
		socket->set_verify_mode(boost::asio::ssl::verify_none);
		co_await socket->next_layer().async_connect(harness.acceptor.local_endpoint(), boost::asio::use_awaitable);
		co_await socket->async_handshake(boost::asio::ssl::stream_base::client, boost::asio::use_awaitable);
		auto session = detail::Http2ClientSession::create(socket);
		weakSession = session;
		weakSocket = socket;
		auto pending = session->run();
		session.reset();
		socket.reset();
		EXPECT_FALSE(weakSession.expired());
		EXPECT_FALSE(weakSocket.expired());
		boost::asio::co_spawn(harness.io,
							  std::move(pending),
							  [&](std::exception_ptr exception, NetworkError error)
							  {
								  EXPECT_FALSE(exception);
								  result = std::move(error);
								  completed = true;
							  });
		while (!completed && !harness.timedOut)
		{
			auto running = weakSession.lock();
			if (running && running->connection().running())
			{
				running->close();
				break;
			}
			running.reset();
			co_await boost::asio::post(boost::asio::use_awaitable);
		}
		while (!completed && !harness.timedOut)
		{
			co_await boost::asio::post(boost::asio::use_awaitable);
		}
		EXPECT_TRUE(weakSession.expired());
		EXPECT_TRUE(weakSocket.expired());
	};
	harness.run(scenario());
	EXPECT_TRUE(completed);
	EXPECT_EQ(result.code, ErrorCode::hOperationAborted);
}

namespace
{
	struct H2Peer : detail::Http2Codec::Listener
	{
		using Codec = detail::Http2Codec;
		std::shared_ptr<TlsSocket> socket;
		std::shared_ptr<detail::Http2Connection> connection;
		std::unordered_map<int32_t, HeaderMap> headers;
		std::unordered_map<int32_t, std::string> bodies;
		std::vector<int32_t> ended;
		std::function<void(H2Peer&, int32_t)> respond;

		detail::Http2Code onBeginHeaders(int32_t id) override
		{
			headers.try_emplace(id);
			bodies.try_emplace(id);
			return detail::Http2Code::hNoError;
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
			ended.push_back(id);
			if (respond)
			{
				respond(*this, id);
			}
		}

		void send(int32_t id, std::string body, std::string_view status = "200", bool trailers = false)
		{
			const auto length = std::to_string(body.size());
			const Codec::Header fields[] {{":status", status}, {"content-length", length}, {"x-final", "initial"}};
			auto opened = connection->output().submitResponse(id, fields);
			EXPECT_FALSE(opened.error) << opened.error.message;
			if (opened.error)
			{
				return;
			}
			EXPECT_FALSE(opened.producer->appendOwned(std::move(body)));
			const Codec::Header tail[] {{"x-final", "trailer"}};
			EXPECT_FALSE(opened.producer->finish(trailers ? std::span(tail) : std::span<const Codec::Header> {}));
		}
	};

	struct H2Harness : Harness
	{
		detail::Http2ServiceBudget service {64};
		std::vector<std::shared_ptr<H2Peer>> peers;
		std::function<void(H2Peer&, int32_t)> respond;

		H2Harness()
		{
			serverTlsContext = std::make_shared<SslContext>(boost::asio::ssl::context::tls_server);
			serverTlsContext->loadCertificate("test_server.crt");
			serverTlsContext->loadPrivateKey("test_server.key");
			serverTlsContext->setServerHttpProtocolMode(HttpProtocolMode::hNegotiateHttp2);
			serverTlsContext->freezeServerHttpProtocolConfig();
			respond = [](H2Peer& peer, int32_t id)
			{
				peer.send(id, peer.bodies[id]);
			};
		}

		Awaitable<void> serveH2(std::shared_ptr<TlsSocket> socket)
		{
			boost::system::error_code error;
			co_await socket->async_handshake(boost::asio::ssl::stream_base::server,
											 boost::asio::redirect_error(boost::asio::use_awaitable, error));
			if (error)
			{
				co_return;
			}
			++handshakes;
			auto peer = std::make_shared<H2Peer>();
			peer->socket = socket;
			peer->respond = respond;
			peer->connection = detail::Http2Connection::create(
				*socket,
				detail::Http2Codec::Role::hServer,
				Http2Options {},
				detail::Http2Budget(detail::Http2Budget::Role::hServer, Http2Options {}, &service));
			EXPECT_FALSE(peer->connection->setListener(peer.get()));
			peers.push_back(peer);
			co_await peer->connection->run();
		}

		Awaitable<void> acceptH2()
		{
			for (;;)
			{
				auto socket = std::make_shared<TlsSocket>(io, serverTlsContext->native());
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

		void run(Awaitable<void> scenario)
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
			coSpawn(io, acceptH2());
			auto result = boost::asio::co_spawn(io, execute(*this, std::move(scenario)), boost::asio::use_future);
			io.run();
			EXPECT_FALSE(timedOut);
			EXPECT_NO_THROW(result.get());
		}
	};

	HttpClientOptions h2Options()
	{
		HttpClientOptions options;
		options.tlsContext = std::make_shared<SslContext>(boost::asio::ssl::context::tls_client);
		options.tlsContext->loadCaCertificate("test_server.crt");
		options.protocolMode = HttpProtocolMode::hHttp2Only;
		return options;
	}

	/** 内存对端通过协议字节驱动，不使用 socket 或直接注入回调。 */
	struct MemoryClientPeer
	{
		boost::asio::io_context io;
		SslContext tls {boost::asio::ssl::context::tls_client};
		std::shared_ptr<TlsSocket> socket {std::make_shared<TlsSocket>(io, tls.native())};
		std::shared_ptr<detail::Http2ClientSession> session {detail::Http2ClientSession::create(socket)};
		detail::Http2Codec peer;

		explicit MemoryClientPeer(const Http2Options& options) : peer(detail::Http2Codec::Role::hServer, options)
		{
			fromPeer();
		}

		~MemoryClientPeer()
		{
			session->close();
			poll();
		}

		void poll()
		{
			io.restart();
			io.poll();
		}

		void fromPeer()
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
		}

		void toPeer()
		{
			for (size_t n = 0; n != 16; ++n)
			{
				auto wire = session->connection().output().nextBatch();
				ASSERT_FALSE(wire.error);
				if (wire.bytes.empty())
				{
					return;
				}
				ASSERT_FALSE(peer.memRecv(wire.bytes).error);
				ASSERT_FALSE(session->connection().output().acknowledge(wire.bytes.size()).error);
			}
			FAIL() << "客户端输出未在有限批次内排空";
		}

		std::future<HttpClientResult> start(detail::Http2ClientSession::Request request)
		{
			auto result = boost::asio::co_spawn(io, session->request(std::move(request)), boost::asio::use_future);
			poll();
			toPeer();
			return result;
		}
	};
} // namespace

TEST_F(Http2ClientTlsStateTest, Http2ConcurrentStreamsCompleteOutOfOrderOnOneTlsConnection)
{
	H2Harness harness;
	harness.respond = [](H2Peer& peer, int32_t)
	{
		if (peer.ended.size() == 2)
		{
			for (const auto& [id, headers] : peer.headers)
			{
				if (headers.find(":path") == "/second")
				{
					peer.send(id, "second");
				}
			}
		}
	};
	HttpClient client(harness.io, "127.0.0.1", harness.port(), h2Options());
	std::optional<HttpClientResult> first, second;
	auto scenario = [&]() -> Awaitable<void>
	{
		boost::asio::co_spawn(harness.io,
							  client.get("/first"),
							  [&](std::exception_ptr error, HttpClientResult value)
							  {
								  EXPECT_FALSE(error);
								  first = std::move(value);
							  });
		boost::asio::co_spawn(harness.io,
							  client.get("/second"),
							  [&](std::exception_ptr error, HttpClientResult value)
							  {
								  EXPECT_FALSE(error);
								  second = std::move(value);
								  EXPECT_FALSE(first.has_value());
								  if (second->ok() && !harness.peers.empty())
								  {
									  auto& peer = *harness.peers[0];
									  for (const auto& [id, headers] : peer.headers)
									  {
										  if (headers.find(":path") == "/first")
										  {
											  peer.send(id, "first");
										  }
									  }
								  }
							  });
		while ((!first || !second) && !harness.timedOut)
		{
			co_await boost::asio::post(boost::asio::use_awaitable);
		}
		EXPECT_TRUE(first.has_value());
		if (!first)
		{
			co_return;
		}
		EXPECT_TRUE(second.has_value());
		if (!second)
		{
			co_return;
		}
		EXPECT_TRUE(first->ok()) << first->error.message;
		EXPECT_TRUE(second->ok()) << second->error.message;
		EXPECT_EQ(first->body, "first");
		EXPECT_EQ(second->body, "second");
		EXPECT_EQ(first->protocol, HttpProtocol::hHttp2);
		EXPECT_TRUE(first->statusText.empty());
		client.close();
	};
	harness.run(scenario());
	EXPECT_EQ(harness.accepts, 1U);
	EXPECT_EQ(harness.handshakes, 1U);
}

TEST_F(Http2ClientTlsStateTest, Http2InformationalFinalBodyAndTrailersRemainSeparateAndOwned)
{
	H2Harness harness;
	harness.respond = [](H2Peer& peer, int32_t id)
	{
		const H2Peer::Codec::Header early[] {{":status", "103"}, {"x-early", "temporary"}};
		EXPECT_FALSE(peer.connection->codec().submitResponse(id, early, false));
		EXPECT_FALSE(peer.connection->codec().submitResponse(id, early, false));
		peer.send(id, "complete", "200", true);
	};
	auto client = std::make_unique<HttpClient>(harness.io, "127.0.0.1", harness.port(), h2Options());
	HttpClientResult saved;
	auto scenario = [&]() -> Awaitable<void>
	{
		saved = co_await client->get("/trailers");
		EXPECT_TRUE(saved.ok()) << saved.error.message;
		client.reset();
	};
	harness.run(scenario());
	EXPECT_EQ(saved.status, 200);
	EXPECT_TRUE(saved.statusText.empty());
	EXPECT_EQ(saved.body, "complete");
	EXPECT_TRUE(saved.headers.find("x-early").empty());
	EXPECT_EQ(saved.headers.find("x-final"), "initial");
	EXPECT_EQ(saved.trailers.find("x-final"), "trailer");
}

TEST_F(Http2ClientTlsStateTest, Http2LargePostAndResponseCrossBothFlowControlWindows)
{
	H2Harness harness;
	HttpClient client(harness.io, "127.0.0.1", harness.port(), h2Options());
	std::string body(256 * 1024, 'x');
	auto pending = client.post(std::string("/large"), body, std::string("text/owned"));
	body.assign(body.size(), 'z');
	auto scenario = [&]() -> Awaitable<void>
	{
		const auto result = co_await std::move(pending);
		EXPECT_TRUE(result.ok()) << result.error.message;
		EXPECT_EQ(result.body.size(), 256U * 1024U);
		EXPECT_TRUE(result.body == std::string(256 * 1024, 'x'));
		client.close();
	};
	harness.run(scenario());
}

TEST_F(Http2ClientTlsStateTest, Http2HeadNoContentAndNotModifiedHaveNoResponseBody)
{
	H2Harness harness;
	harness.respond = [](H2Peer& peer, int32_t id)
	{
		const auto path = peer.headers[id].find(":path");
		const auto status = path == "/head" ? "200" : path == "/204" ? "204" : "304";
		std::vector<H2Peer::Codec::Header> fields {{":status", status}};
		if (path != "/204")
		{
			fields.push_back({"content-length", "123"});
		}
		EXPECT_FALSE(peer.connection->output().submitResponseHeaders(id, fields).error);
	};
	HttpClient client(harness.io, "127.0.0.1", harness.port(), h2Options());
	auto scenario = [&]() -> Awaitable<void>
	{
		for (const auto target : {"/head", "/204", "/304"})
		{
			const auto result =
				co_await client.request(target == std::string_view("/head") ? HttpMethod::hHead : HttpMethod::hGet,
										target);
			EXPECT_FALSE(result.error) << result.error.message;
			EXPECT_TRUE(result.body.empty());
			EXPECT_EQ(result.status,
					  target == std::string_view("/head")  ? 200
					  : target == std::string_view("/204") ? 204
														   : 304);
		}
		client.close();
	};
	harness.run(scenario());
}

TEST_F(Http2ClientTlsStateTest, Http2ResponseLimitRejectsOneStreamAndKeepsTheConnectionUsable)
{
	H2Harness harness;
	harness.respond = [](H2Peer& peer, int32_t id)
	{
		peer.send(id, peer.headers[id].find(":path") == "/large" ? "123456789" : "ok");
	};
	auto options = h2Options();
	options.http2.maxResponseBytes = 8;
	HttpClient client(harness.io, "127.0.0.1", harness.port(), options);
	auto scenario = [&]() -> Awaitable<void>
	{
		const auto rejected = co_await client.get("/large");
		EXPECT_TRUE(rejected.error);
		EXPECT_TRUE(rejected.body.empty());
		EXPECT_TRUE(rejected.error.http2.has_value());
		if (!rejected.error.http2)
		{
			co_return;
		}
		EXPECT_EQ(rejected.error.http2->code, static_cast<uint32_t>(detail::Http2Code::hEnhanceYourCalm));
		const auto good = co_await client.get("/small");
		EXPECT_TRUE(good.ok()) << good.error.message;
		EXPECT_EQ(good.body, "ok");
		client.close();
	};
	harness.run(scenario());
	EXPECT_EQ(harness.accepts, 1U);
}

TEST_F(Http2ClientTlsStateTest, Http2RefusedStreamExposesRealProtocolEvidence)
{
	H2Harness harness;
	harness.respond = [](H2Peer& peer, int32_t id)
	{
		EXPECT_FALSE(peer.connection->codec().reset(id, detail::Http2Code::hRefusedStream));
		peer.connection->wakeWriter();
	};
	HttpClient client(harness.io, "127.0.0.1", harness.port(), h2Options());
	auto scenario = [&]() -> Awaitable<void>
	{
		const auto result = co_await client.get("/refused");
		EXPECT_EQ(result.protocol, HttpProtocol::hHttp2);
		EXPECT_EQ(result.requestDisposition, HttpRequestDisposition::hNotProcessed);
		EXPECT_TRUE(result.error.http2.has_value());
		if (!result.error.http2)
		{
			co_return;
		}
		EXPECT_EQ(result.error.http2->code, 7U);
		EXPECT_GT(result.error.http2->streamId, 0);
		client.close();
	};
	harness.run(scenario());
}

TEST_F(Http2ClientTlsStateTest, Http2GoawayRejectsOnlyStreamsAboveItsLastProcessedId)
{
	H2Harness harness;
	harness.respond = [](H2Peer& peer, int32_t)
	{
		if (peer.ended.size() == 2)
		{
			const auto first = *std::min_element(peer.ended.begin(), peer.ended.end());
			EXPECT_FALSE(peer.connection->codec().goaway(first));
			peer.send(first, "accepted");
			peer.connection->wakeWriter();
		}
	};
	HttpClient client(harness.io, "127.0.0.1", harness.port(), h2Options());
	std::optional<HttpClientResult> first, second;
	auto scenario = [&]() -> Awaitable<void>
	{
		boost::asio::co_spawn(harness.io,
							  client.get("/first"),
							  [&](std::exception_ptr error, HttpClientResult value)
							  {
								  EXPECT_FALSE(error);
								  first = std::move(value);
							  });
		boost::asio::co_spawn(harness.io,
							  client.get("/second"),
							  [&](std::exception_ptr error, HttpClientResult value)
							  {
								  EXPECT_FALSE(error);
								  second = std::move(value);
							  });
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
		EXPECT_TRUE(first->ok()) << first->error.message;
		EXPECT_EQ(first->body, "accepted");
		EXPECT_EQ(first->requestDisposition, HttpRequestDisposition::hPossiblyProcessed);
		EXPECT_EQ(second->requestDisposition, HttpRequestDisposition::hNotProcessed);
		EXPECT_TRUE(second->error.http2.has_value());
		if (second->error.http2)
		{
			EXPECT_EQ(second->error.http2->code, 0U);
			EXPECT_EQ(second->error.http2->streamId, 0);
		}
		client.close();
	};
	harness.run(scenario());
	EXPECT_EQ(harness.accepts, 1U);
}

TEST_F(Http2ClientTlsStateTest, Http2OrdinaryResetKeepsPossiblyProcessedAndDoesNotCloseOtherStreams)
{
	H2Harness harness;
	harness.respond = [](H2Peer& peer, int32_t id)
	{
		if (peer.headers[id].find(":path") == "/reset")
		{
			EXPECT_FALSE(peer.connection->codec().reset(id, detail::Http2Code::hCancel));
			peer.connection->wakeWriter();
		}
		else
		{
			peer.send(id, "good");
		}
	};
	HttpClient client(harness.io, "127.0.0.1", harness.port(), h2Options());
	auto scenario = [&]() -> Awaitable<void>
	{
		const auto reset = co_await client.get("/reset");
		EXPECT_TRUE(reset.error);
		EXPECT_EQ(reset.status, 0);
		EXPECT_EQ(reset.requestDisposition, HttpRequestDisposition::hPossiblyProcessed);
		EXPECT_TRUE(reset.error.http2.has_value());
		if (reset.error.http2)
		{
			EXPECT_EQ(reset.error.http2->code, 8U);
		}
		const auto good = co_await client.get("/good");
		EXPECT_TRUE(good.ok()) << good.error.message;
		EXPECT_EQ(good.body, "good");
		client.close();
	};
	harness.run(scenario());
	EXPECT_EQ(harness.accepts, 1U);
}

TEST_F(Http2ClientTlsStateTest, Http2IncompleteResponseReturnsAnErrorWithoutPartialBody)
{
	H2Harness harness;
	harness.respond = [](H2Peer& peer, int32_t id)
	{
		const H2Peer::Codec::Header headers[] {{":status", "200"}, {"content-length", "4"}};
		auto opened = peer.connection->output().submitResponse(id, headers);
		EXPECT_FALSE(opened.error);
		if (opened.error)
		{
			return;
		}
		EXPECT_FALSE(opened.producer->append("bad"));
		EXPECT_FALSE(opened.producer->finish());
	};
	HttpClient client(harness.io, "127.0.0.1", harness.port(), h2Options());
	auto scenario = [&]() -> Awaitable<void>
	{
		const auto result = co_await client.get("/truncated");
		EXPECT_TRUE(result.error);
		EXPECT_EQ(result.status, 200);
		EXPECT_TRUE(result.body.empty());
		EXPECT_EQ(result.requestDisposition, HttpRequestDisposition::hPossiblyProcessed);
		client.close();
	};
	harness.run(scenario());
}

TEST(Http2ClientSessionTest, CapacityUsesQueuedAdmissionsAndPeerZeroAndDecreaseSettings)
{
	boost::asio::io_context io;
	SslContext tls(boost::asio::ssl::context::tls_client);
	auto socket = std::make_shared<TlsSocket>(io, tls.native());
	auto session = detail::Http2ClientSession::create(socket);
	detail::Http2Codec peer(detail::Http2Codec::Role::hServer);
	auto feedSettings = [&](uint32_t limit)
	{
		const detail::Http2Codec::Setting setting {detail::Http2Setting::hMaxConcurrentStreams, limit};
		EXPECT_FALSE(peer.submitSettings(std::span(&setting, 1)));
		for (size_t n = 0; n != 16; ++n)
		{
			auto wire = peer.memSend();
			EXPECT_FALSE(wire.error);
			if (wire.bytes.empty())
			{
				break;
			}
			EXPECT_FALSE(session->connection().codec().memRecv(wire.bytes).error);
		}
	};
	EXPECT_FALSE(session->capacity().settingsSeen);
	feedSettings(1);
	EXPECT_EQ(session->capacity().available, 1U);
	detail::Http2ClientSession::Request request;
	request.authority = "localhost";
	request.target = "/pending";
	auto pending = boost::asio::co_spawn(io, session->request(request), boost::asio::use_future);
	io.poll();
	EXPECT_EQ(session->capacity().admitted, 1U);
	EXPECT_EQ(session->connection().codec().activeLocalStreamCount(), 0U);
	EXPECT_EQ(session->capacity().available, 0U);
	feedSettings(0);
	EXPECT_EQ(session->capacity().peerLimit, 0U);
	EXPECT_EQ(session->capacity().admitted, 1U);
	boost::asio::cancellation_signal cancellation;
	auto rejected =
		boost::asio::co_spawn(io,
							  session->request(request),
							  boost::asio::bind_cancellation_slot(cancellation.slot(), boost::asio::use_future));
	io.restart();
	io.poll();
	EXPECT_EQ(rejected.wait_for(std::chrono::seconds::zero()), std::future_status::timeout);
	EXPECT_EQ(session->capacity().admitted, 1U);
	EXPECT_EQ(session->connection().codec().activeLocalStreamCount(), 0U);
	cancellation.emit(boost::asio::cancellation_type::terminal);
	io.restart();
	io.poll();
	ASSERT_EQ(rejected.wait_for(std::chrono::seconds::zero()), std::future_status::ready);
	const auto result = rejected.get();
	EXPECT_EQ(result.error.code, ErrorCode::hOperationAborted);
	EXPECT_EQ(result.requestDisposition, HttpRequestDisposition::hNotSent);
	EXPECT_FALSE(result.error.http2.has_value());
	feedSettings(2);
	EXPECT_EQ(session->capacity().available, 1U);
	session->close();
	io.restart();
	io.poll();
	ASSERT_EQ(pending.wait_for(std::chrono::seconds::zero()), std::future_status::ready);
	EXPECT_TRUE(pending.get().error);
}

TEST(Http2ClientSessionTest, CompleteResponseThenResetInTheSameWireReadKeepsTheFirstCompleteResult)
{
	boost::asio::io_context io;
	SslContext tls(boost::asio::ssl::context::tls_client);
	auto socket = std::make_shared<TlsSocket>(io, tls.native());
	auto session = detail::Http2ClientSession::create(socket);
	Http2Options peerOptions;
	peerOptions.initialStreamWindow = 0;
	detail::Http2Codec peer(detail::Http2Codec::Role::hServer, peerOptions);
	for (size_t n = 0; n != 16; ++n)
	{
		auto wire = peer.memSend();
		ASSERT_FALSE(wire.error);
		if (wire.bytes.empty())
		{
			break;
		}
		ASSERT_FALSE(session->connection().codec().memRecv(wire.bytes).error);
	}
	detail::Http2ClientSession::Request request;
	request.method = HttpMethod::hPost;
	request.authority = "localhost";
	request.target = "/early";
	request.body.assign(128, 'x');
	auto pending = boost::asio::co_spawn(io, session->request(std::move(request)), boost::asio::use_future);
	io.poll();
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
	ASSERT_EQ(session->connection().codec().activeLocalStreamCount(), 1U);
	const detail::Http2Codec::Header final[] {{":status", "200"}, {"content-length", "0"}};
	ASSERT_FALSE(peer.submitResponse(1, final));
	std::string response;
	for (size_t n = 0; n != 16; ++n)
	{
		auto wire = peer.memSend();
		ASSERT_FALSE(wire.error);
		if (wire.bytes.empty())
		{
			break;
		}
		response += wire.bytes;
	}
	// 真实 RST_STREAM 帧紧跟最终 HEADERS，同一次 memRecv 先 END_STREAM 后 RST(NO_ERROR)。
	const std::array<char, 13> reset {0, 0, 4, 3, 0, 0, 0, 0, 1, 0, 0, 0, 0};
	response.append(reset.data(), reset.size());
	ASSERT_FALSE(session->connection().codec().memRecv(response).error);
	io.restart();
	io.poll();
	ASSERT_EQ(pending.wait_for(std::chrono::seconds::zero()), std::future_status::ready);
	const auto result = pending.get();
	EXPECT_TRUE(result.ok()) << result.error.message;
	EXPECT_EQ(result.status, 200);
	EXPECT_TRUE(result.body.empty());
	EXPECT_EQ(result.requestDisposition, HttpRequestDisposition::hPossiblyProcessed);
	// 发完控制帧后，RST 应取消尚未写完的本地 END，不能等 close() 再清理。
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
	EXPECT_EQ(session->connection().output().queuedBytes(), 0U);
	EXPECT_FALSE(session->connection().output().hasPendingOutput());
	EXPECT_EQ(session->capacity().admitted, 0U);
	EXPECT_EQ(session->capacity().available, 64U);
	session->close();
	io.restart();
	io.poll();
}

TEST(Http2ClientSessionTest, EarlyCompleteResponseStopsWindowBlockedUploadAndReturnsItsOnlySlot)
{
	Http2Options peerOptions;
	peerOptions.initialStreamWindow = 0;
	peerOptions.maxConcurrentStreams = 1;
	MemoryClientPeer harness(peerOptions);
	detail::Http2ClientSession::Request request;
	request.method = HttpMethod::hPost;
	request.authority = "localhost";
	request.target = "/early";
	request.body.assign(128, 'x');
	auto first = harness.start(std::move(request));
	ASSERT_EQ(harness.session->capacity().admitted, 1U);
	ASSERT_EQ(harness.session->connection().output().queuedBytes(), 128U);
	const detail::Http2Codec::Header final[] {{":status", "200"}, {"content-length", "0"}};
	// 对端只结束响应，不发送 RST 或 WINDOW_UPDATE，客户端须自行结束剩余上传。
	ASSERT_FALSE(harness.peer.submitResponse(1, final));
	harness.fromPeer();
	harness.poll();
	ASSERT_EQ(first.wait_for(std::chrono::seconds::zero()), std::future_status::ready);
	EXPECT_TRUE(first.get().ok());
	harness.toPeer();
	EXPECT_EQ(harness.session->connection().output().queuedBytes(), 0U);
	EXPECT_FALSE(harness.session->connection().output().hasPendingOutput());
	EXPECT_EQ(harness.session->capacity().admitted, 0U);
	EXPECT_EQ(harness.session->capacity().available, 1U);
	detail::Http2ClientSession::Request next;
	next.authority = "localhost";
	next.target = "/next";
	auto second = harness.start(std::move(next));
	EXPECT_EQ(second.wait_for(std::chrono::seconds::zero()), std::future_status::timeout);
	if (second.wait_for(std::chrono::seconds::zero()) == std::future_status::ready)
	{
		EXPECT_TRUE(second.get().ok());
		return;
	}
	ASSERT_FALSE(harness.peer.submitResponse(3, final));
	harness.fromPeer();
	harness.poll();
	ASSERT_EQ(second.wait_for(std::chrono::seconds::zero()), std::future_status::ready);
	EXPECT_TRUE(second.get().ok());
}

TEST(Http2ClientSessionTest, ExplicitNoErrorResetBeforeCompleteResponseRetainsZeroProtocolCode)
{
	Http2Options peerOptions;
	peerOptions.initialStreamWindow = 0;
	peerOptions.maxConcurrentStreams = 1;
	MemoryClientPeer harness(peerOptions);
	detail::Http2ClientSession::Request request;
	request.method = HttpMethod::hPost;
	request.authority = "localhost";
	request.target = "/reset";
	request.body.assign(128, 'x');
	auto pending = harness.start(std::move(request));
	ASSERT_FALSE(harness.peer.reset(1, detail::Http2Code::hNoError));
	harness.fromPeer();
	harness.poll();
	ASSERT_EQ(pending.wait_for(std::chrono::seconds::zero()), std::future_status::ready);
	const auto result = pending.get();
	EXPECT_TRUE(result.error);
	EXPECT_EQ(result.status, 0);
	EXPECT_TRUE(result.body.empty());
	EXPECT_EQ(result.requestDisposition, HttpRequestDisposition::hPossiblyProcessed);
	ASSERT_TRUE(result.error.http2.has_value());
	EXPECT_EQ(result.error.http2->code, 0U);
	EXPECT_EQ(result.error.http2->streamId, 1);
	harness.toPeer();
	EXPECT_EQ(harness.session->connection().output().queuedBytes(), 0U);
	EXPECT_FALSE(harness.session->connection().output().hasPendingOutput());
	EXPECT_EQ(harness.session->capacity().admitted, 0U);
	EXPECT_EQ(harness.session->capacity().available, 1U);
}

TEST(Http2ClientSessionTest, ExplicitResetNotifiesItsFrameBeforeProtocolClose)
{
	struct Recorder final : detail::Http2Codec::Listener
	{
		std::vector<int> order;

		void onFrameReceived(const detail::Http2Codec::Frame& frame) override
		{
			if (frame.type == detail::Http2Codec::FrameType::hRstStream)
			{
				order.push_back(1);
			}
		}

		void onStreamClose(int32_t id, detail::Http2Code code) override
		{
			EXPECT_EQ(id, 1);
			EXPECT_EQ(code, detail::Http2Code::hNoError);
			order.push_back(2);
		}
	} recorder;

	detail::Http2Codec client(detail::Http2Codec::Role::hClient, {}, &recorder);
	detail::Http2Codec peer(detail::Http2Codec::Role::hServer);
	const detail::Http2Codec::Header headers[] {{":method", "POST"},
												{":scheme", "https"},
												{":authority", "localhost"},
												{":path", "/"}};
	ASSERT_FALSE(client.submitRequest(headers, false).error);
	for (size_t n = 0; n != 16; ++n)
	{
		auto wire = client.memSend();
		ASSERT_FALSE(wire.error);
		if (wire.bytes.empty())
		{
			break;
		}
		ASSERT_FALSE(peer.memRecv(wire.bytes).error);
	}
	ASSERT_FALSE(peer.reset(1, detail::Http2Code::hNoError));
	for (size_t n = 0; n != 16; ++n)
	{
		auto wire = peer.memSend();
		ASSERT_FALSE(wire.error);
		if (wire.bytes.empty())
		{
			break;
		}
		ASSERT_FALSE(client.memRecv(wire.bytes).error);
	}
	EXPECT_EQ(recorder.order, (std::vector<int> {1, 2}));
}

#endif
