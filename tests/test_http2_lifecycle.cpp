/**
 * @file test_http2_lifecycle.cpp
 * @brief HTTP/2 连接控制登记与同步注销行为
 */

#include "core/HttpConnectionRegistry.h"
#include "core/HttpServer.h"
#include <boost/asio/cancel_at.hpp>
#include <boost/asio/buffers_iterator.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/read_until.hpp>
#include <boost/asio/use_future.hpp>
#include <gtest/gtest.h>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using hical::detail::HttpConnectionRegistry;

TEST(Http2RegistryTest, GuardReturnsRegistrationExactlyOnce)
{
	boost::asio::io_context io;
	HttpConnectionRegistry registry;
	auto first = registry.add(io.get_executor(),
							  []
							  {
							  });
	EXPECT_EQ(registry.size(), 1U);
	auto moved = std::move(first);
	first.reset();
	EXPECT_EQ(registry.size(), 1U);
	moved.reset();
	moved.reset();
	EXPECT_EQ(registry.size(), 0U);
}

TEST(Http2RegistryTest, InlineCloseCanUnregisterEverySnapshotEntry)
{
	boost::asio::io_context io;
	HttpConnectionRegistry registry;
	HttpConnectionRegistry::Registration first;
	HttpConnectionRegistry::Registration second;
	size_t calls = 0;
	auto close = [&]
	{
		++calls;
		first.reset();
		second.reset();
	};
	first = registry.add(io.get_executor(), close);
	second = registry.add(io.get_executor(), close);
	boost::asio::post(io,
					  [&]
					  {
						  registry.requestClose();
						  EXPECT_EQ(registry.size(), 0U);
					  });
	io.run();
	EXPECT_EQ(calls, 1U);
}

TEST(Http2RegistryTest, InlineDrainCanUnregisterEverySnapshotEntry)
{
	boost::asio::io_context io;
	HttpConnectionRegistry registry;
	auto first = registry.add(io.get_executor(),
							  []
							  {
							  });
	auto second = registry.add(io.get_executor(),
							   []
							   {
							   });
	size_t calls = 0;
	auto drain = [&]
	{
		++calls;
		first.reset();
		second.reset();
	};
	first.setDrainHandler(drain);
	second.setDrainHandler(drain);
	boost::asio::post(io,
					  [&]
					  {
						  registry.requestDrain();
					  });
	io.run();
	EXPECT_EQ(calls, 1U);
	EXPECT_EQ(registry.size(), 0U);
}

TEST(Http2RegistryTest, QueuedNotificationDoesNotCallExpiredSession)
{
	boost::asio::io_context io;
	HttpConnectionRegistry registry;
	size_t calls = 0;
	auto guard = registry.add(io.get_executor(),
							  [&]
							  {
								  ++calls;
							  });
	registry.requestClose();
	guard.reset();
	io.run();
	EXPECT_EQ(calls, 0U);
}

TEST(Http2RegistryTest, CrossThreadBroadcastRunsOnOwningLoopAndOnlyOnce)
{
	boost::asio::io_context io;
	HttpConnectionRegistry registry;
	const auto owner = std::this_thread::get_id();
	size_t calls = 0;
	auto guard = registry.add(io.get_executor(),
							  [&]
							  {
								  EXPECT_EQ(std::this_thread::get_id(), owner);
								  ++calls;
							  });
	std::thread sender(
		[&]
		{
			registry.requestClose();
			registry.requestClose();
		});
	sender.join();
	io.run();
	EXPECT_EQ(calls, 1U);
}

TEST(Http2RegistryTest, DrainBeforeHandshakeIsDeliveredWhenSessionAttaches)
{
	boost::asio::io_context io;
	HttpConnectionRegistry registry;
	auto guard = registry.add(io.get_executor(),
							  []
							  {
							  });
	registry.requestDrain();
	io.run();
	ASSERT_TRUE(guard.draining());
	size_t calls = 0;
	guard.setDrainHandler(
		[&]
		{
			++calls;
		});
	guard.setDrainHandler(
		[&]
		{
			++calls;
		});
	EXPECT_EQ(calls, 1U);
}

TEST(Http2RegistryTest, CloseSupersedesPendingDrain)
{
	boost::asio::io_context io;
	HttpConnectionRegistry registry;
	size_t closes = 0;
	size_t drains = 0;
	auto guard = registry.add(io.get_executor(),
							  [&]
							  {
								  ++closes;
							  });
	registry.requestDrain();
	registry.requestClose();
	io.run();
	guard.setDrainHandler(
		[&]
		{
			++drains;
		});
	EXPECT_EQ(closes, 1U);
	EXPECT_EQ(drains, 0U);
}

TEST(Http2RegistryTest, GuardCanOutliveFacadeWithoutKeepingCallbacksActive)
{
	boost::asio::io_context io;
	HttpConnectionRegistry::Registration guard;
	{
		HttpConnectionRegistry registry;
		guard = registry.add(io.get_executor(),
							 []
							 {
							 });
		registry.requestClose();
	}
	guard.reset();
	io.run();
}

namespace
{
	using namespace hical;
	using boost::asio::ip::tcp;
	using Deadline = std::chrono::steady_clock::time_point;
	using TlsStream = boost::asio::ssl::stream<tcp::socket>;
	constexpr auto kTestTimeout = std::chrono::seconds {10};
	constexpr const char* kCertificate = "test_server.crt";
	constexpr const char* kPrivateKey = "test_server.key";

	class LifecycleServer
	{
	public:
		explicit LifecycleServer(size_t threads = 1) : server(0, threads)
		{
			server.setIdleTimeout(0);
			server.setGcInterval(0);
		}

		~LifecycleServer()
		{
			server.stop();
			if (thread_.joinable())
			{
				thread_.join();
			}
		}

		bool start(Deadline deadline)
		{
			auto ready = std::make_shared<std::promise<void>>();
			auto readyFuture = ready->get_future();
			// start 完成配置并启动主循环后才执行此回调，此时可以读取初始化结果。
			boost::asio::post(server.ioContext(),
							  [ready]
							  {
								  ready->set_value();
							  });
			doneFuture_ = done_.get_future();
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
			return readyFuture.wait_until(deadline) == std::future_status::ready;
		}

		bool stoppedBefore(Deadline deadline)
		{
			if (doneFuture_.wait_until(deadline) != std::future_status::ready)
			{
				return false;
			}
			doneFuture_.get();
			return true;
		}

		HttpServer server;

	private:
		std::promise<void> done_;
		std::future<void> doneFuture_;
		std::thread thread_;
	};

	template <typename Predicate>
	bool waitUntil(Deadline deadline, Predicate predicate)
	{
		while (!predicate())
		{
			if (std::chrono::steady_clock::now() >= deadline)
			{
				return false;
			}
			std::this_thread::yield();
		}
		return true;
	}

	Awaitable<void> connectClient(tcp::socket& socket, uint16_t port, Deadline deadline)
	{
		co_await socket.async_connect(tcp::endpoint(boost::asio::ip::address_v4::loopback(), port),
									  boost::asio::cancel_at(deadline, boost::asio::use_awaitable));
	}

	Awaitable<std::string> requestHttp1(TlsStream& stream, uint16_t port, Deadline deadline)
	{
		co_await stream.next_layer().async_connect(tcp::endpoint(boost::asio::ip::address_v4::loopback(), port),
												   boost::asio::cancel_at(deadline, boost::asio::use_awaitable));
		co_await stream.async_handshake(boost::asio::ssl::stream_base::client,
										boost::asio::cancel_at(deadline, boost::asio::use_awaitable));
		const std::string request = "GET / HTTP/1.1\r\nHost: localhost\r\nConnection: keep-alive\r\n\r\n";
		co_await boost::asio::async_write(stream,
										  boost::asio::buffer(request),
										  boost::asio::cancel_at(deadline, boost::asio::use_awaitable));
		boost::asio::streambuf response;
		co_await boost::asio::async_read_until(stream,
											   response,
											   "\r\n\r\n",
											   boost::asio::cancel_at(deadline, boost::asio::use_awaitable));
		co_return std::string(boost::asio::buffers_begin(response.data()), boost::asio::buffers_end(response.data()));
	}

	class Http2LifecycleTest : public testing::Test
	{
	protected:
		void SetUp() override
		{
			if (!std::ifstream(kCertificate).good() || !std::ifstream(kPrivateKey).good())
			{
				if (std::getenv("HICAL_REQUIRE_SSL_CERTS") != nullptr)
				{
					FAIL() << "要求 SSL 证书，但 test_server.crt/key 缺失";
				}
				GTEST_SKIP() << "请先在测试工作目录生成 SSL 证书";
			}
		}
	};
} // namespace

TEST(Http2ServerConfigTest, EnableHttp2RejectsInvalidOptionsAndRequiresTlsAtStart)
{
	hical::HttpServer server;
	hical::Http2Options invalid;
	invalid.maxConcurrentStreams = 0;
	EXPECT_THROW(server.enableHttp2(invalid), std::invalid_argument);
	server.enableHttp2();
	EXPECT_THROW(server.start(), std::invalid_argument);
	EXPECT_FALSE(server.isRunning());
	EXPECT_EQ(server.activeConnectionCount(), 0U);
	EXPECT_EQ(server.controlledConnectionCount(), 0U);
}

TEST_F(Http2LifecycleTest, StopClosesStalledTlsHandshakeWithIdleTimeoutDisabled)
{
	const auto deadline = std::chrono::steady_clock::now() + kTestTimeout;
	LifecycleServer running;
	running.server.enableHttp2();
	running.server.enableSsl(kCertificate, kPrivateKey);
	ASSERT_TRUE(running.start(deadline));
	boost::asio::io_context io;
	tcp::socket peer(io);
	auto connected =
		boost::asio::co_spawn(io, connectClient(peer, running.server.port(), deadline), boost::asio::use_future);
	io.run();
	ASSERT_NO_THROW(connected.get());
	ASSERT_TRUE(waitUntil(deadline,
						  [&]
						  {
							  return running.server.controlledConnectionCount() == 1;
						  }));
	EXPECT_EQ(running.server.activeConnectionCount(), 1U);
	running.server.stop();
	EXPECT_TRUE(running.stoppedBefore(deadline));
	peer.close();
	EXPECT_EQ(running.server.controlledConnectionCount(), 0U);
	EXPECT_EQ(running.server.activeConnectionCount(), 0U);
}

TEST(Http2ServerConfigTest, RuntimeDisabledServerDoesNotRegisterConnections)
{
	const auto deadline = std::chrono::steady_clock::now() + kTestTimeout;
	LifecycleServer running;
	ASSERT_TRUE(running.start(deadline));
	boost::asio::io_context io;
	tcp::socket peer(io);
	auto connected =
		boost::asio::co_spawn(io, connectClient(peer, running.server.port(), deadline), boost::asio::use_future);
	io.run();
	ASSERT_NO_THROW(connected.get());
	ASSERT_TRUE(waitUntil(deadline,
						  [&]
						  {
							  return running.server.activeConnectionCount() == 1;
						  }));
	EXPECT_EQ(running.server.controlledConnectionCount(), 0U);
	peer.close();
	ASSERT_TRUE(waitUntil(deadline,
						  [&]
						  {
							  return running.server.activeConnectionCount() == 0;
						  }));
	running.server.stop();
	EXPECT_TRUE(running.stoppedBefore(deadline));
}

TEST_F(Http2LifecycleTest, TlsHttp1FallbackIsControlledAndConfigurationOrderIsIndependent)
{
	for (bool http2First : {false, true})
	{
		const auto deadline = std::chrono::steady_clock::now() + kTestTimeout;
		LifecycleServer running;
		if (http2First)
		{
			running.server.enableHttp2();
		}
		running.server.enableSsl(kCertificate, kPrivateKey);
		running.server.enableHttp2();
		running.server.router().get("/",
									[](const HttpRequest&)
									{
										return HttpResponse::ok("ready");
									});
		ASSERT_TRUE(running.start(deadline));
		EXPECT_THROW(running.server.enableHttp2(), std::logic_error);
		boost::asio::io_context io;
		SslContext context(boost::asio::ssl::context::tls_client);
		context.setVerifyPeer(false);
		TlsStream peer(io, context.native());
		auto response =
			boost::asio::co_spawn(io, requestHttp1(peer, running.server.port(), deadline), boost::asio::use_future);
		io.run();
		ASSERT_NE(response.get().find("HTTP/1.1 200"), std::string::npos);
		EXPECT_EQ(running.server.controlledConnectionCount(), 1U);
		EXPECT_EQ(running.server.activeConnectionCount(), 1U);
		const unsigned char* selected = nullptr;
		unsigned int selectedSize = 0;
		SSL_get0_alpn_selected(peer.native_handle(), &selected, &selectedSize);
		EXPECT_EQ(selectedSize, 0U);
		running.server.stop();
		EXPECT_TRUE(running.stoppedBefore(deadline));
		peer.next_layer().close();
		EXPECT_EQ(running.server.controlledConnectionCount(), 0U);
		EXPECT_EQ(running.server.activeConnectionCount(), 0U);
	}
}

TEST_F(Http2LifecycleTest, FailedHandshakeReturnsItsSlotBeforeNextConnection)
{
	const auto deadline = std::chrono::steady_clock::now() + kTestTimeout;
	LifecycleServer running;
	running.server.enableSsl(kCertificate, kPrivateKey);
	running.server.enableHttp2();
	running.server.setMaxConnections(1);
	ASSERT_TRUE(running.start(deadline));
	boost::asio::io_context io;
	tcp::socket first(io);
	auto connected =
		boost::asio::co_spawn(io, connectClient(first, running.server.port(), deadline), boost::asio::use_future);
	io.run();
	ASSERT_NO_THROW(connected.get());
	ASSERT_TRUE(waitUntil(deadline,
						  [&]
						  {
							  return running.server.controlledConnectionCount() == 1;
						  }));
	first.close();
	ASSERT_TRUE(waitUntil(deadline,
						  [&]
						  {
							  return running.server.activeConnectionCount() == 0;
						  }));
	EXPECT_EQ(running.server.controlledConnectionCount(), 0U);
	io.restart();
	tcp::socket second(io);
	auto reconnected =
		boost::asio::co_spawn(io, connectClient(second, running.server.port(), deadline), boost::asio::use_future);
	io.run();
	ASSERT_NO_THROW(reconnected.get());
	ASSERT_TRUE(waitUntil(deadline,
						  [&]
						  {
							  return running.server.controlledConnectionCount() == 1;
						  }));
	EXPECT_EQ(running.server.activeConnectionCount(), 1U);
	running.server.stop();
	EXPECT_TRUE(running.stoppedBefore(deadline));
	second.close();
	EXPECT_EQ(running.server.activeConnectionCount(), 0U);
}

TEST_F(Http2LifecycleTest, MultiWorkerStopClosesEveryRegisteredHandshakeOnce)
{
	constexpr size_t kConnections = 6;
	const auto deadline = std::chrono::steady_clock::now() + kTestTimeout;
	LifecycleServer running(4);
	running.server.enableSsl(kCertificate, kPrivateKey);
	running.server.enableHttp2();
	ASSERT_TRUE(running.start(deadline));
	boost::asio::io_context io;
	std::vector<tcp::socket> peers;
	for (size_t i = 0; i < kConnections; ++i)
	{
		peers.emplace_back(io);
		io.restart();
		auto connected = boost::asio::co_spawn(io,
											   connectClient(peers.back(), running.server.port(), deadline),
											   boost::asio::use_future);
		io.run();
		ASSERT_NO_THROW(connected.get());
	}
	ASSERT_TRUE(waitUntil(deadline,
						  [&]
						  {
							  return running.server.controlledConnectionCount() == kConnections;
						  }));
	EXPECT_EQ(running.server.activeConnectionCount(), kConnections);
	running.server.stop();
	running.server.stop();
	EXPECT_TRUE(running.stoppedBefore(deadline));
	for (auto& peer : peers)
	{
		peer.close();
	}
	EXPECT_EQ(running.server.controlledConnectionCount(), 0U);
	EXPECT_EQ(running.server.activeConnectionCount(), 0U);
}

TEST_F(Http2LifecycleTest, StopRejectsAcceptCompletionQueuedBehindOwnerLoopBarrier)
{
	const auto deadline = std::chrono::steady_clock::now() + kTestTimeout;
	LifecycleServer running;
	running.server.enableSsl(kCertificate, kPrivateKey);
	running.server.enableHttp2();
	ASSERT_TRUE(running.start(deadline));
	auto entered = std::make_shared<std::promise<void>>();
	auto enteredFuture = entered->get_future();
	auto release = std::make_shared<std::promise<void>>();
	auto releaseFuture = release->get_future().share();
	boost::asio::post(running.server.ioContext(),
					  [entered, releaseFuture, deadline]
					  {
						  entered->set_value();
						  releaseFuture.wait_until(deadline);
					  });
	ASSERT_EQ(enteredFuture.wait_until(deadline), std::future_status::ready);
	boost::asio::io_context io;
	tcp::socket peer(io);
	auto connected =
		boost::asio::co_spawn(io, connectClient(peer, running.server.port(), deadline), boost::asio::use_future);
	io.run();
	ASSERT_NO_THROW(connected.get());
	running.server.stop();
	release->set_value();
	EXPECT_TRUE(running.stoppedBefore(deadline));
	peer.close();
	EXPECT_EQ(running.server.controlledConnectionCount(), 0U);
	EXPECT_EQ(running.server.activeConnectionCount(), 0U);
}
