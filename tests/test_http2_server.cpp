/**
 * @file test_http2_server.cpp
 * @brief HTTP/2 服务端协议分流、HTTP/1 回退与清理测试
 */

#include "core/HttpServer.h"
#include "core/http2/Http2Codec.h"
#include "core/http2/Http2ServerSession.h"
#include "core/JsonWriter.h"
#include "core/RequestDispatch.h"
#include <boost/asio/cancel_at.hpp>
#include <boost/asio/co_spawn.hpp>
#ifdef _WIN32
	#include <boost/asio/detail/socket_option.hpp>
#endif
#include <boost/asio/read_until.hpp>
#include <boost/asio/use_future.hpp>
#include <gtest/gtest.h>
#include <array>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <future>
#include <cstring>
#include <map>
#include <set>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using namespace hical;
using namespace hical::detail;

namespace
{
	using Clock = std::chrono::steady_clock;
	using Deadline = Clock::time_point;
	using tcp = boost::asio::ip::tcp;
	using TlsSocket = boost::asio::ssl::stream<tcp::socket>;
	constexpr auto kTestTimeout = std::chrono::seconds {10};
	constexpr const char* kCertificate = "test_server.crt";
	constexpr const char* kPrivateKey = "test_server.key";
	constexpr std::array<Http2Codec::Header, 4> kRequest {
		{{":method", "GET"}, {":scheme", "https"}, {":authority", "localhost"}, {":path", "/"}}};

	auto requestHeaders(std::string_view method, std::string_view path)
	{
		auto headers = kRequest;
		headers[0].value = method;
		headers[3].value = path;
		return headers;
	}

	struct PendingBody : Http2Codec::DataSource
	{
		std::string body;
		size_t offset {0};
		bool finished {false};

		Http2Codec::DataRead read(std::span<char> output) override
		{
			if (!finished)
			{
				return {0, false, true, {}};
			}
			const auto count = std::min(output.size(), body.size() - offset);
			std::memcpy(output.data(), body.data() + offset, count);
			offset += count;
			return {count, offset == body.size(), false, {}};
		}
	};

	class RunningServer
	{
	public:
		explicit RunningServer(bool http2 = true) : server(0, 1)
		{
			server.setGcInterval(0);
			server.setIdleTimeout(0);
			server.enableSsl(kCertificate, kPrivateKey);
			if (http2)
			{
				server.enableHttp2();
			}
			server.router().get("/",
								[](const HttpRequest&)
								{
									return HttpResponse::ok("h1");
								});
		}

		~RunningServer()
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
			auto future = ready->get_future();
			// ready 在配置冻结、worker 启动后执行，再通过通知把端口和配置交给测试线程。
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
			return future.wait_until(deadline) == std::future_status::ready;
		}

		bool stopped(Deadline deadline)
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
		std::thread thread_;
		std::promise<void> done_;
		std::future<void> doneFuture_;
	};

	struct Peer : Http2Codec::Listener
	{
		explicit Peer(const Http2Options& options = {}) : codec(Http2Codec::Role::hClient, options, this)
		{
		}

		Http2Codec codec;
		bool settings {false};
		size_t acknowledgements {0};
		std::optional<Http2Code> closedCode;
		std::map<int32_t, Http2Code> closed;
		std::map<int32_t, uint16_t> status;
		std::map<int32_t, HeaderMap> headers;
		std::map<int32_t, std::string> body;
		std::map<int32_t, std::vector<uint16_t>> informational;
		std::set<int32_t> ended;

		void onSettings(bool ack) override
		{
			settings = settings || !ack;
			acknowledgements += ack;
		}

		Http2Code onHeader(int32_t id, Http2Codec::Header field) override
		{
			if (field.name != ":status")
			{
				headers[id].insert(field.name, field.value);
			}
			return Http2Code::hNoError;
		}

		Http2Code onEndHeaders(const Http2Codec::HeaderBlock& block) override
		{
			if (block.kind == Http2Codec::HeaderKind::hResponse)
			{
				if (block.statusCode < 200)
				{
					informational[block.streamId].push_back(block.statusCode);
				}
				else
				{
					status[block.streamId] = block.statusCode;
				}
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
			closedCode = code;
			closed[id] = code;
		}
	};

	struct Client
	{
		boost::asio::io_context io;
		boost::asio::ssl::context context {boost::asio::ssl::context::tls_client};
		TlsSocket socket {io, context};

		Client()
		{
			socket.set_verify_mode(boost::asio::ssl::verify_none);
		}

		Awaitable<void> connect(uint16_t port, Deadline deadline, std::optional<HttpProtocolMode> mode)
		{
			if (mode)
			{
				auto error = SslContext::configureClientHttpProtocols(socket.native_handle(), *mode);
				if (error)
				{
					throw std::runtime_error(error.message);
				}
			}
			co_await socket.next_layer().async_connect({boost::asio::ip::address_v4::loopback(), port},
													   boost::asio::cancel_at(deadline, boost::asio::use_awaitable));
			co_await socket.async_handshake(boost::asio::ssl::stream_base::client,
											boost::asio::cancel_at(deadline, boost::asio::use_awaitable));
		}

		Awaitable<void> write(std::string_view bytes, Deadline deadline)
		{
			co_await boost::asio::async_write(socket,
											  boost::asio::buffer(bytes),
											  boost::asio::cancel_at(deadline, boost::asio::use_awaitable));
		}

		Awaitable<void> flush(Peer& peer, Deadline deadline)
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
				co_await write(batch.bytes, deadline);
			}
		}

		Awaitable<void> read(Peer& peer, Deadline deadline)
		{
			std::array<char, 16384> buffer;
			auto count = co_await socket.async_read_some(boost::asio::buffer(buffer),
														 boost::asio::cancel_at(deadline, boost::asio::use_awaitable));
			auto result = peer.codec.memRecv({buffer.data(), count});
			EXPECT_FALSE(result.error);
			EXPECT_EQ(result.consumed, count);
		}

		Awaitable<void> negotiate(uint16_t port, Peer& peer, Deadline deadline)
		{
			co_await connect(port, deadline, HttpProtocolMode::hHttp2Only);
			HttpProtocol protocol;
			EXPECT_FALSE(
				SslContext::negotiatedHttpProtocol(socket.native_handle(), HttpProtocolMode::hHttp2Only, protocol));
			EXPECT_EQ(protocol, HttpProtocol::hHttp2);
			co_await flush(peer, deadline);
			while (!peer.settings)
			{
				co_await read(peer, deadline);
			}
			co_await flush(peer, deadline);
		}

		Awaitable<void> expectClosed(Deadline deadline)
		{
			std::array<char, 16384> buffer;
			boost::system::error_code error;
			while (!error)
			{
				co_await socket.async_read_some(
					boost::asio::buffer(buffer),
					boost::asio::cancel_at(deadline, boost::asio::redirect_error(boost::asio::use_awaitable, error)));
			}
			EXPECT_NE(error, boost::asio::error::operation_aborted) << "等待连接关闭超过绝对截止点";
		}

		void run(Awaitable<void> scenario)
		{
			auto result = boost::asio::co_spawn(io, std::move(scenario), boost::asio::use_future);
			io.run();
			EXPECT_NO_THROW(result.get());
		}
	};

	Awaitable<void> http1(Client& client, uint16_t port, Deadline deadline, std::optional<HttpProtocolMode> mode)
	{
		co_await client.connect(port, deadline, mode);
		co_await client.write("GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n", deadline);
		boost::asio::streambuf response;
		co_await boost::asio::async_read_until(client.socket,
											   response,
											   "\r\n\r\n",
											   boost::asio::cancel_at(deadline, boost::asio::use_awaitable));
		auto bytes =
			std::string(boost::asio::buffers_begin(response.data()), boost::asio::buffers_end(response.data()));
		EXPECT_TRUE(bytes.starts_with("HTTP/1.1 200"));
	}

	class Http2ServerTest : public testing::Test
	{
	protected:
		void SetUp() override
		{
			if (!std::ifstream(kCertificate).good() || !std::ifstream(kPrivateKey).good())
			{
				if (std::getenv("HICAL_REQUIRE_SSL_CERTS"))
				{
					FAIL() << "要求 SSL 证书，但 test_server.crt/key 缺失";
				}
				GTEST_SKIP() << "请先在测试工作目录生成 SSL 证书";
			}
		}
	};
} // namespace

TEST_F(Http2ServerTest, NegotiatesHttp2AndExchangesPrefaceSettings)
{
	const auto deadline = Clock::now() + kTestTimeout;
	RunningServer running;
	ASSERT_TRUE(running.start(deadline));
	Client client;
	Peer peer;
	auto scenario = [](Client& client, Peer& peer, uint16_t port, Deadline deadline) -> Awaitable<void>
	{
		co_await client.negotiate(port, peer, deadline);
		auto submitted = peer.codec.submitRequest(kRequest);
		EXPECT_FALSE(submitted.error);
		co_await client.flush(peer, deadline);
		while (!peer.ended.contains(submitted.streamId))
		{
			co_await client.read(peer, deadline);
		}
		EXPECT_EQ(peer.status[submitted.streamId], 200);
		EXPECT_EQ(peer.body[submitted.streamId], "h1");
	};
	client.run(scenario(client, peer, running.server.port(), deadline));
}

TEST_F(Http2ServerTest, FailedBindCanRetryAnotherPortWithFrozenAlpn)
{
	const auto deadline = Clock::now() + kTestTimeout;
	boost::asio::io_context occupiedIo;
	tcp::acceptor occupied(occupiedIo);
	occupied.open(tcp::v4());
#ifdef _WIN32
	// Windows 的 SO_REUSEADDR 可抢占普通绑定；独占保留才能稳定制造真实 bind 失败。
	using ExclusiveAddress = boost::asio::detail::socket_option::boolean<SOL_SOCKET, SO_EXCLUSIVEADDRUSE>;
	occupied.set_option(ExclusiveAddress(true));
#endif
	occupied.bind({boost::asio::ip::address_v4::loopback(), 0});
	occupied.listen();
	RunningServer running;
	running.server.listen(occupied.local_endpoint());
	EXPECT_THROW(running.server.start(), boost::system::system_error);
	EXPECT_FALSE(running.server.isRunning());
	running.server.listen("127.0.0.1", 0);
	ASSERT_TRUE(running.start(deadline));
	Client client;
	Peer peer;
	client.run(client.negotiate(running.server.port(), peer, deadline));
	EXPECT_TRUE(peer.settings);
	running.server.stop();
	EXPECT_TRUE(running.stopped(deadline));
}

TEST_F(Http2ServerTest, Http1AlpnAndNoAlpnShareTheEnabledTlsPort)
{
	const auto deadline = Clock::now() + kTestTimeout;
	RunningServer running;
	ASSERT_TRUE(running.start(deadline));
	for (auto mode : {std::optional {HttpProtocolMode::hHttp1Only}, std::optional<HttpProtocolMode> {}})
	{
		Client client;
		client.run(http1(client, running.server.port(), deadline, mode));
	}
}

TEST_F(Http2ServerTest, RuntimeDisabledHttp2KeepsHttp1AndDoesNotRegisterConnections)
{
	const auto deadline = Clock::now() + kTestTimeout;
	RunningServer running(false);
	ASSERT_TRUE(running.start(deadline));
	Client client;
	client.run(http1(client, running.server.port(), deadline, HttpProtocolMode::hNegotiateHttp2));
	EXPECT_EQ(running.server.controlledConnectionCount(), 0U);
}

TEST_F(Http2ServerTest, InvalidHttp2PrefaceClosesWithoutHttp1Response)
{
	const auto deadline = Clock::now() + kTestTimeout;
	RunningServer running;
	ASSERT_TRUE(running.start(deadline));
	Client client;
	auto scenario = [](Client& client, uint16_t port, Deadline deadline) -> Awaitable<void>
	{
		co_await client.connect(port, deadline, HttpProtocolMode::hHttp2Only);
		co_await client.write("GET / HTTP/1.1\r\nHost: localhost\r\n\r\n", deadline);
		std::array<char, 16384> buffer;
		std::string bytes;
		boost::system::error_code error;
		while (!error)
		{
			auto count = co_await client.socket.async_read_some(
				boost::asio::buffer(buffer),
				boost::asio::cancel_at(deadline, boost::asio::redirect_error(boost::asio::use_awaitable, error)));
			bytes.append(buffer.data(), count);
		}
		EXPECT_NE(error, boost::asio::error::operation_aborted);
		EXPECT_FALSE(bytes.starts_with("HTTP/1."));
	};
	client.run(scenario(client, running.server.port(), deadline));
}

TEST_F(Http2ServerTest, StopClosesHttp2WithIdleTimeoutDisabledAndReturnsOuterCounts)
{
	const auto deadline = Clock::now() + kTestTimeout;
	RunningServer running;
	ASSERT_TRUE(running.start(deadline));
	Client client;
	Peer peer;
	auto scenario = [](Client& client, Peer& peer, RunningServer& running, Deadline deadline) -> Awaitable<void>
	{
		co_await client.negotiate(running.server.port(), peer, deadline);
		EXPECT_EQ(running.server.activeConnectionCount(), 1U);
		EXPECT_EQ(running.server.controlledConnectionCount(), 1U);
		running.server.stop();
		co_await client.expectClosed(deadline);
	};
	client.run(scenario(client, peer, running, deadline));
	EXPECT_TRUE(running.stopped(deadline));
	EXPECT_EQ(running.server.activeConnectionCount(), 0U);
	EXPECT_EQ(running.server.controlledConnectionCount(), 0U);
}

TEST_F(Http2ServerTest, ValidTrafficRefreshesIdleEntryAndSilenceEventuallyCloses)
{
	const auto deadline = Clock::now() + kTestTimeout;
	RunningServer running;
	running.server.setIdleTimeout(0.4);
	ASSERT_TRUE(running.start(deadline));
	Client client;
	Peer peer;
	auto scenario = [](Client& client, Peer& peer, uint16_t port, Deadline deadline) -> Awaitable<void>
	{
		constexpr auto kActivityDuration = std::chrono::milliseconds {1200};
		constexpr auto kHeartbeatInterval = std::chrono::milliseconds {100};
		co_await client.negotiate(port, peer, deadline);
		const auto start = Clock::now();
		boost::asio::steady_timer next(client.io);
		const std::array<Http2Codec::Setting, 1> settings {{{Http2Setting::hHeaderTableSize, 4096}}};
		for (auto at = start; at < start + kActivityDuration; at += kHeartbeatInterval)
		{
			next.expires_at(at);
			co_await next.async_wait(boost::asio::cancel_at(deadline, boost::asio::use_awaitable));
			const auto previous = peer.acknowledgements;
			EXPECT_FALSE(peer.codec.submitSettings(settings));
			co_await client.flush(peer, deadline);
			while (peer.acknowledgements == previous)
			{
				co_await client.read(peer, deadline);
			}
		}
		EXPECT_TRUE(client.socket.next_layer().is_open());
		co_await client.expectClosed(deadline);
	};
	client.run(scenario(client, peer, running.server.port(), deadline));
}

TEST_F(Http2ServerTest, ExistingHeaderAndBodyLimitsConstrainHttp2BeforeAdmission)
{
	const auto deadline = Clock::now() + kTestTimeout;
	RunningServer running;
	running.server.setMaxHeaderSize(256);
	running.server.setMaxBodySize(4);
	ASSERT_TRUE(running.start(deadline));
	Client client;
	Peer peer;
	auto scenario = [](Client& client, Peer& peer, uint16_t port, Deadline deadline) -> Awaitable<void>
	{
		co_await client.negotiate(port, peer, deadline);
		std::array headers {kRequest[0],
							kRequest[1],
							kRequest[2],
							kRequest[3],
							Http2Codec::Header {"content-length", "5"}};
		EXPECT_FALSE(peer.codec.submitRequest(headers, false).error);
		co_await client.flush(peer, deadline);
		while (!peer.closedCode)
		{
			co_await client.read(peer, deadline);
		}
		EXPECT_EQ(*peer.closedCode, Http2Code::hEnhanceYourCalm);
		peer.closedCode.reset();
		const std::string tooLarge(256, 'x');
		headers.back() = {"x-large", tooLarge};
		EXPECT_FALSE(peer.codec.submitRequest(headers).error);
		co_await client.flush(peer, deadline);
		while (!peer.closedCode)
		{
			co_await client.read(peer, deadline);
		}
		EXPECT_EQ(*peer.closedCode, Http2Code::hEnhanceYourCalm);
	};
	client.run(scenario(client, peer, running.server.port(), deadline));
}

TEST_F(Http2ServerTest, SlowBodyDoesNotBlockAnotherStreamResponse)
{
	const auto deadline = Clock::now() + kTestTimeout;
	RunningServer running;
	running.server.router().post("/echo",
								 [](const HttpRequest& req)
								 {
									 return HttpResponse::ok(req.body());
								 });
	ASSERT_TRUE(running.start(deadline));
	Client client;
	Peer peer;
	auto scenario = [](Client& client, Peer& peer, uint16_t port, Deadline deadline) -> Awaitable<void>
	{
		co_await client.negotiate(port, peer, deadline);
		auto pending = std::make_shared<PendingBody>();
		auto slow = peer.codec.submitRequest(requestHeaders("POST", "/echo"), pending);
		auto fast = peer.codec.submitRequest(kRequest);
		EXPECT_FALSE(slow.error);
		EXPECT_FALSE(fast.error);
		co_await client.flush(peer, deadline);
		while (!peer.ended.contains(fast.streamId))
		{
			co_await client.read(peer, deadline);
		}
		EXPECT_EQ(peer.body[fast.streamId], "h1");
		EXPECT_FALSE(peer.ended.contains(slow.streamId));
		pending->body = "late-body";
		pending->finished = true;
		EXPECT_FALSE(peer.codec.resume(slow.streamId));
		co_await client.flush(peer, deadline);
		while (!peer.ended.contains(slow.streamId))
		{
			co_await client.read(peer, deadline);
		}
		EXPECT_EQ(peer.status[slow.streamId], 200);
		EXPECT_EQ(peer.body[slow.streamId], "late-body");
	};
	client.run(scenario(client, peer, running.server.port(), deadline));
}

TEST_F(Http2ServerTest, EarlyRouteAndAuthenticationErrorsFinishBeforeResetAndKeepNeighborUsable)
{
	const auto deadline = Clock::now() + kTestTimeout;
	RunningServer running;
	running.server.use(
		[](HttpRequest& req) -> SyncMiddlewareResult
		{
			EXPECT_TRUE(req.body().empty());
			if (req.path() == "/denied")
			{
				auto response = HttpResponse::badRequest("denied");
				response.setStatus(HttpStatusCode::hUnauthorized);
				return response;
			}
			return std::nullopt;
		});
	running.server.router().get("/only-get",
								[](const HttpRequest&)
								{
									return HttpResponse::ok("get");
								});
	running.server.router().post("/denied",
								 [](const HttpRequest&)
								 {
									 return HttpResponse::serverError();
								 });
	ASSERT_TRUE(running.start(deadline));
	Client client;
	Peer peer;
	auto scenario = [](Client& client, Peer& peer, uint16_t port, Deadline deadline) -> Awaitable<void>
	{
		co_await client.negotiate(port, peer, deadline);
		std::array<int32_t, 3> rejected;
		const std::array<std::string_view, 3> paths {"/missing", "/only-get", "/denied"};
		for (size_t index = 0; index < paths.size(); ++index)
		{
			auto initial = requestHeaders("POST", paths[index]);
			std::array headers {initial[0],
								initial[1],
								initial[2],
								initial[3],
								Http2Codec::Header {"expect", "100-continue"}};
			auto submitted = peer.codec.submitRequest(headers, std::make_shared<PendingBody>());
			EXPECT_FALSE(submitted.error);
			rejected[index] = submitted.streamId;
		}
		auto good = peer.codec.submitRequest(kRequest);
		EXPECT_FALSE(good.error);
		co_await client.flush(peer, deadline);
		while (!peer.closed.contains(rejected.back()) || !peer.ended.contains(good.streamId)
			   || !peer.closed.contains(rejected.front()) || !peer.closed.contains(rejected[1]))
		{
			co_await client.read(peer, deadline);
		}
		const std::array<uint16_t, 3> expected {404, 405, 401};
		for (size_t index = 0; index < rejected.size(); ++index)
		{
			const auto id = rejected[index];
			EXPECT_EQ(peer.status[id], expected[index]);
			EXPECT_TRUE(peer.ended.contains(id));
			EXPECT_EQ(peer.closed[id], Http2Code::hNoError);
			EXPECT_TRUE(peer.informational[id].empty());
		}
		EXPECT_EQ(peer.headers[rejected[1]].find("allow"), "GET");
		EXPECT_EQ(peer.body[good.streamId], "h1");
	};
	client.run(scenario(client, peer, running.server.port(), deadline));
}

TEST_F(Http2ServerTest, JsonWriterCookiesHeadAndNoBodyStatusesUseHttp2Frames)
{
	const auto deadline = Clock::now() + kTestTimeout;
	RunningServer running;
	running.server.router().get("/writer",
								[](const HttpRequest&)
								{
									auto response = HttpResponse::ok("stale");
									response.native().preparePayload();
									response.setCookie("one", "1");
									response.setCookie("two", "2");
									{
										auto writer = response.jsonWriter();
										writer.beginObject().key("value").value(42).endObject();
									}
									return response;
								});
	running.server.router().route(HttpMethod::hHead,
								  "/head",
								  [](const HttpRequest&)
								  {
									  return HttpResponse::ok("representation");
								  });
	running.server.router().get("/empty",
								[](const HttpRequest&)
								{
									auto response = HttpResponse::ok("ignored");
									response.setStatus(HttpStatusCode::hNoContent);
									return response;
								});
	running.server.router().get("/cached",
								[](const HttpRequest&)
								{
									auto response = HttpResponse::ok("ignored");
									response.setStatus(HttpStatusCode::hNotModified);
									response.setHeader("Content-Length", "123");
									return response;
								});
	ASSERT_TRUE(running.start(deadline));
	Client client;
	Peer peer;
	auto scenario = [](Client& client, Peer& peer, uint16_t port, Deadline deadline) -> Awaitable<void>
	{
		co_await client.negotiate(port, peer, deadline);
		std::array<int32_t, 4> streams;
		const std::array<std::string_view, 4> paths {"/writer", "/head", "/empty", "/cached"};
		for (size_t index = 0; index < paths.size(); ++index)
		{
			auto submitted = peer.codec.submitRequest(requestHeaders(index == 1 ? "HEAD" : "GET", paths[index]));
			EXPECT_FALSE(submitted.error);
			streams[index] = submitted.streamId;
		}
		co_await client.flush(peer, deadline);
		while (peer.ended.size() < streams.size())
		{
			co_await client.read(peer, deadline);
		}
		EXPECT_EQ(peer.body[streams[0]], "{\"value\":42}");
		EXPECT_EQ(peer.headers[streams[0]].find("content-length"), std::to_string(peer.body[streams[0]].size()));
		EXPECT_EQ(peer.headers[streams[0]].count("set-cookie"), 2U);
		EXPECT_EQ(peer.headers[streams[1]].find("content-length"), "14");
		EXPECT_TRUE(peer.body[streams[1]].empty());
		EXPECT_EQ(peer.status[streams[2]], 204);
		EXPECT_TRUE(peer.body[streams[2]].empty());
		EXPECT_FALSE(peer.headers[streams[2]].contains("content-length"));
		EXPECT_EQ(peer.status[streams[3]], 304);
		EXPECT_TRUE(peer.body[streams[3]].empty());
		EXPECT_EQ(peer.headers[streams[3]].find("content-length"), "123");
	};
	client.run(scenario(client, peer, running.server.port(), deadline));
}

TEST_F(Http2ServerTest, ExpectContinueIsSentOnlyWhenBodyGateNeedsIt)
{
	const auto deadline = Clock::now() + kTestTimeout;
	RunningServer running;
	running.server.useBody(
		[](HttpRequest& req) -> SyncMiddlewareResult
		{
			EXPECT_EQ(req.body(), "continued");
			return std::nullopt;
		});
	running.server.router().post("/echo",
								 [](const HttpRequest& req)
								 {
									 return HttpResponse::ok(req.body());
								 });
	ASSERT_TRUE(running.start(deadline));
	Client client;
	Peer peer;
	auto scenario = [](Client& client, Peer& peer, uint16_t port, Deadline deadline) -> Awaitable<void>
	{
		co_await client.negotiate(port, peer, deadline);
		auto initial = requestHeaders("POST", "/echo");
		std::array headers {initial[0],
							initial[1],
							initial[2],
							initial[3],
							Http2Codec::Header {"expect", "100-continue"}};
		auto pending = std::make_shared<PendingBody>();
		auto submitted = peer.codec.submitRequest(headers, pending);
		EXPECT_FALSE(submitted.error);
		co_await client.flush(peer, deadline);
		while (peer.informational[submitted.streamId].empty())
		{
			co_await client.read(peer, deadline);
		}
		EXPECT_EQ(peer.informational[submitted.streamId], (std::vector<uint16_t> {100}));
		EXPECT_FALSE(peer.status.contains(submitted.streamId));
		pending->body = "continued";
		pending->finished = true;
		EXPECT_FALSE(peer.codec.resume(submitted.streamId));
		co_await client.flush(peer, deadline);
		while (!peer.ended.contains(submitted.streamId))
		{
			co_await client.read(peer, deadline);
		}
		EXPECT_EQ(peer.status[submitted.streamId], 200);
		EXPECT_EQ(peer.body[submitted.streamId], "continued");
	};
	client.run(scenario(client, peer, running.server.port(), deadline));
}

TEST_F(Http2ServerTest, HandlerExceptionUsesConfiguredErrorResponse)
{
	const auto deadline = Clock::now() + kTestTimeout;
	RunningServer running;
	running.server.router().get("/throws",
								[](const HttpRequest&) -> HttpResponse
								{
									throw std::runtime_error("broken");
								});
	running.server.setErrorHandler(
		[](const std::exception& error, const HttpRequest& req)
		{
			EXPECT_STREQ(error.what(), "broken");
			EXPECT_EQ(req.protocol(), HttpProtocol::hHttp2);
			return HttpResponse::badRequest("handled");
		});
	ASSERT_TRUE(running.start(deadline));
	Client client;
	Peer peer;
	auto scenario = [](Client& client, Peer& peer, uint16_t port, Deadline deadline) -> Awaitable<void>
	{
		co_await client.negotiate(port, peer, deadline);
		auto submitted = peer.codec.submitRequest(requestHeaders("GET", "/throws"));
		EXPECT_FALSE(submitted.error);
		co_await client.flush(peer, deadline);
		while (!peer.ended.contains(submitted.streamId))
		{
			co_await client.read(peer, deadline);
		}
		EXPECT_EQ(peer.status[submitted.streamId], 400);
		EXPECT_EQ(peer.body[submitted.streamId], "handled");
	};
	client.run(scenario(client, peer, running.server.port(), deadline));
}

TEST_F(Http2ServerTest, BodyTimeoutRespondsAndLeavesNeighborUsable)
{
	const auto deadline = Clock::now() + kTestTimeout;
	RunningServer running;
	Http2Options options;
	options.bodyTimeout = std::chrono::milliseconds {1};
	running.server.enableHttp2(options);
	running.server.router().post("/echo",
								 [](const HttpRequest& req)
								 {
									 return HttpResponse::ok(req.body());
								 });
	ASSERT_TRUE(running.start(deadline));
	Client client;
	Peer peer;
	auto scenario = [](Client& client, Peer& peer, uint16_t port, Deadline deadline) -> Awaitable<void>
	{
		co_await client.negotiate(port, peer, deadline);
		auto slow = peer.codec.submitRequest(requestHeaders("POST", "/echo"), std::make_shared<PendingBody>());
		EXPECT_FALSE(slow.error);
		co_await client.flush(peer, deadline);
		while (!peer.ended.contains(slow.streamId) || !peer.closed.contains(slow.streamId))
		{
			co_await client.read(peer, deadline);
		}
		EXPECT_EQ(peer.status[slow.streamId], 408);
		EXPECT_EQ(peer.closed[slow.streamId], Http2Code::hNoError);
		auto good = peer.codec.submitRequest(kRequest);
		EXPECT_FALSE(good.error);
		co_await client.flush(peer, deadline);
		while (!peer.ended.contains(good.streamId))
		{
			co_await client.read(peer, deadline);
		}
		EXPECT_EQ(peer.status[good.streamId], 200);
	};
	client.run(scenario(client, peer, running.server.port(), deadline));
}

TEST_F(Http2ServerTest, LateTrailersDoNotCancelWindowBlockedTimeoutResponse)
{
	const auto deadline = Clock::now() + kTestTimeout;
	RunningServer running;
	Http2Options options;
	options.bodyTimeout = std::chrono::milliseconds {1};
	running.server.enableHttp2(options);
	running.server.router().post("/echo",
								 [](const HttpRequest& req)
								 {
									 return HttpResponse::ok(req.body());
								 });
	ASSERT_TRUE(running.start(deadline));
	Client client;
	Peer peer;
	auto scenario = [](Client& client, Peer& peer, uint16_t port, Deadline deadline) -> Awaitable<void>
	{
		co_await client.negotiate(port, peer, deadline);
		const std::array<Http2Codec::Setting, 1> blocked {{{Http2Setting::hInitialWindowSize, 0}}};
		EXPECT_FALSE(peer.codec.submitSettings(blocked));
		auto slow = peer.codec.submitRequest(requestHeaders("POST", "/echo"), false);
		EXPECT_FALSE(slow.error);
		co_await client.flush(peer, deadline);
		while (!peer.status.contains(slow.streamId))
		{
			co_await client.read(peer, deadline);
		}
		EXPECT_EQ(peer.status[slow.streamId], 408);
		EXPECT_FALSE(peer.ended.contains(slow.streamId));
		EXPECT_TRUE(peer.body[slow.streamId].empty());
		const std::array<Http2Codec::Header, 1> trailers {{{"x-finish", "late"}}};
		EXPECT_FALSE(peer.codec.submitTrailers(slow.streamId, trailers));
		co_await client.flush(peer, deadline);
		const std::array<Http2Codec::Setting, 1> opened {
			{{Http2Setting::hInitialWindowSize, Http2Options::kDefaultWindowSize}}};
		EXPECT_FALSE(peer.codec.submitSettings(opened));
		co_await client.flush(peer, deadline);
		while (!peer.ended.contains(slow.streamId) && !peer.closed.contains(slow.streamId))
		{
			co_await client.read(peer, deadline);
		}
		EXPECT_TRUE(peer.ended.contains(slow.streamId));
		EXPECT_FALSE(peer.body[slow.streamId].empty());
		EXPECT_EQ(peer.body[slow.streamId].size(),
				  std::stoull(std::string(peer.headers[slow.streamId].find("content-length"))));
		auto good = peer.codec.submitRequest(kRequest);
		EXPECT_FALSE(good.error);
		co_await client.flush(peer, deadline);
		while (!peer.ended.contains(good.streamId))
		{
			co_await client.read(peer, deadline);
		}
		EXPECT_EQ(peer.status[good.streamId], 200);
		EXPECT_EQ(peer.body[good.streamId], "h1");
	};
	client.run(scenario(client, peer, running.server.port(), deadline));
}

TEST_F(Http2ServerTest, EarlyRouteAndAuthErrorsAcceptBoundedLateTrailers)
{
	const auto deadline = Clock::now() + kTestTimeout;
	RunningServer running;
	running.server.use(
		[](HttpRequest& req) -> SyncMiddlewareResult
		{
			if (req.path() == "/denied")
			{
				auto response = HttpResponse::badRequest("denied");
				response.setStatus(HttpStatusCode::hUnauthorized);
				return response;
			}
			return std::nullopt;
		});
	running.server.router().post("/denied",
								 [](const HttpRequest&)
								 {
									 return HttpResponse::serverError();
								 });
	ASSERT_TRUE(running.start(deadline));
	Client client;
	Peer peer;
	auto scenario = [](Client& client, Peer& peer, uint16_t port, Deadline deadline) -> Awaitable<void>
	{
		co_await client.negotiate(port, peer, deadline);
		const std::array<Http2Codec::Setting, 1> blocked {{{Http2Setting::hInitialWindowSize, 0}}};
		EXPECT_FALSE(peer.codec.submitSettings(blocked));
		std::array<int32_t, 2> streams;
		const std::array<std::string_view, 2> paths {"/missing", "/denied"};
		for (size_t index = 0; index < streams.size(); ++index)
		{
			auto request = peer.codec.submitRequest(requestHeaders("POST", paths[index]), false);
			EXPECT_FALSE(request.error);
			streams[index] = request.streamId;
		}
		co_await client.flush(peer, deadline);
		while (peer.status.size() < streams.size())
		{
			co_await client.read(peer, deadline);
		}
		const std::array<Http2Codec::Header, 1> trailers {{{"x-finish", "late"}}};
		for (auto id : streams)
		{
			EXPECT_FALSE(peer.ended.contains(id));
			EXPECT_FALSE(peer.codec.submitTrailers(id, trailers));
		}
		co_await client.flush(peer, deadline);
		const std::array<Http2Codec::Setting, 1> opened {
			{{Http2Setting::hInitialWindowSize, Http2Options::kDefaultWindowSize}}};
		EXPECT_FALSE(peer.codec.submitSettings(opened));
		co_await client.flush(peer, deadline);
		while (peer.closed.size() < streams.size())
		{
			co_await client.read(peer, deadline);
		}
		EXPECT_EQ(peer.status[streams[0]], 404);
		EXPECT_EQ(peer.status[streams[1]], 401);
		for (auto id : streams)
		{
			EXPECT_TRUE(peer.ended.contains(id));
			EXPECT_FALSE(peer.body[id].empty());
			EXPECT_EQ(peer.closed[id], Http2Code::hNoError);
		}
	};
	client.run(scenario(client, peer, running.server.port(), deadline));
}

TEST_F(Http2ServerTest, TrailerBlockStartedBeforeTimeoutKeepsItsCollectorAcrossContinuation)
{
	const auto deadline = Clock::now() + kTestTimeout;
	RunningServer running;
	Http2Options options;
	options.bodyTimeout = std::chrono::milliseconds {1};
	options.maxHeaderBytes = 64 * 1024;
	running.server.setMaxHeaderSize(options.maxHeaderBytes);
	running.server.enableHttp2(options);
	running.server.router().post("/echo",
								 [](const HttpRequest& req)
								 {
									 return HttpResponse::ok(req.body());
								 });
	ASSERT_TRUE(running.start(deadline));
	Client client;
	Peer peer(options);
	auto scenario = [](Client& client, Peer& peer, uint16_t port, Deadline deadline) -> Awaitable<void>
	{
		co_await client.negotiate(port, peer, deadline);
		auto collect = [&]
		{
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
					return bytes;
				}
				bytes.append(part.bytes);
			}
		};
		const std::array<Http2Codec::Setting, 1> blocked {{{Http2Setting::hInitialWindowSize, 0}}};
		EXPECT_FALSE(peer.codec.submitSettings(blocked));
		auto request = peer.codec.submitRequest(requestHeaders("POST", "/echo"), false);
		EXPECT_FALSE(request.error);
		auto requestBytes = collect();
		const std::string value(32 * 1024, 'x');
		const std::array<Http2Codec::Header, 1> trailers {{{"x-long", value}}};
		EXPECT_FALSE(peer.codec.submitTrailers(request.streamId, trailers));
		auto trailerBytes = collect();
		constexpr size_t kFrameHeaderBytes = 9;
		if (trailerBytes.size() < kFrameHeaderBytes)
		{
			throw std::runtime_error("测试尾头缺少帧首部");
		}
		const auto payload = (static_cast<size_t>(static_cast<unsigned char>(trailerBytes[0])) << 16)
							 | (static_cast<size_t>(static_cast<unsigned char>(trailerBytes[1])) << 8)
							 | static_cast<unsigned char>(trailerBytes[2]);
		const auto firstFrame = kFrameHeaderBytes + payload;
		if (firstFrame >= trailerBytes.size() || trailerBytes[3] != 1 || (trailerBytes[4] & 0x04) != 0)
		{
			throw std::runtime_error("测试尾头未拆成 HEADERS 与 CONTINUATION");
		}
		requestBytes.append(trailerBytes, 0, firstFrame);
		// 同一 TLS write 先送请求和尾头首帧，确保 Receive 已在超时前选择原收集器。
		co_await client.write(requestBytes, deadline);
		while (!peer.status.contains(request.streamId))
		{
			co_await client.read(peer, deadline);
		}
		EXPECT_EQ(peer.status[request.streamId], 408);
		EXPECT_FALSE(peer.ended.contains(request.streamId));
		co_await client.write(std::string_view(trailerBytes).substr(firstFrame), deadline);
		const std::array<Http2Codec::Setting, 1> opened {
			{{Http2Setting::hInitialWindowSize, Http2Options::kDefaultWindowSize}}};
		EXPECT_FALSE(peer.codec.submitSettings(opened));
		co_await client.flush(peer, deadline);
		while (!peer.ended.contains(request.streamId) && !peer.closed.contains(request.streamId))
		{
			co_await client.read(peer, deadline);
		}
		EXPECT_TRUE(peer.ended.contains(request.streamId));
		EXPECT_FALSE(peer.body[request.streamId].empty());
	};
	client.run(scenario(client, peer, running.server.port(), deadline));
}

TEST_F(Http2ServerTest, StopJoinsApplicationAfterTransportCloses)
{
	const auto deadline = Clock::now() + kTestTimeout;
	RunningServer running;
	using Timer = boost::asio::steady_timer;
	auto entered = std::make_shared<std::promise<std::shared_ptr<Timer>>>();
	auto enteredFuture = entered->get_future();
	auto exited = std::make_shared<std::atomic<bool>>(false);
	running.server.router().get("/hold",
								[entered, exited, deadline](const HttpRequest&) -> Awaitable<HttpResponse>
								{
									auto executor = co_await boost::asio::this_coro::executor;
									auto timer = std::make_shared<Timer>(executor, deadline);
									entered->set_value(timer);
									boost::system::error_code ignored;
									co_await timer->async_wait(
										boost::asio::redirect_error(boost::asio::use_awaitable, ignored));
									exited->store(true);
									co_return HttpResponse::ok("finished");
								});
	ASSERT_TRUE(running.start(deadline));
	Client client;
	Peer peer;
	auto scenario = [](Client& client,
					   Peer& peer,
					   RunningServer& running,
					   std::future<std::shared_ptr<Timer>>& enteredFuture,
					   Deadline deadline) -> Awaitable<void>
	{
		co_await client.negotiate(running.server.port(), peer, deadline);
		EXPECT_FALSE(peer.codec.submitRequest(requestHeaders("GET", "/hold")).error);
		co_await client.flush(peer, deadline);
		if (enteredFuture.wait_until(deadline) != std::future_status::ready)
		{
			ADD_FAILURE() << "应用未在绝对截止点前启动";
			co_return;
		}
		auto timer = enteredFuture.get();
		running.server.stop();
		co_await client.expectClosed(deadline);
		EXPECT_EQ(running.server.activeConnectionCount(), 1U);
		boost::asio::post(timer->get_executor(),
						  [timer]
						  {
							  timer->cancel();
						  });
	};
	client.run(scenario(client, peer, running, enteredFuture, deadline));
	EXPECT_TRUE(running.stopped(deadline));
	EXPECT_TRUE(exited->load());
	EXPECT_EQ(running.server.activeConnectionCount(), 0U);
}

TEST(Http2ServerSessionTest, QueuedDispatchKeepsApplicationQuotaAfterReset)
{
	boost::asio::io_context io;
	boost::asio::ssl::context tls(boost::asio::ssl::context::tls_server);
	TlsSocket socket(io, tls);
	Router router;
	MiddlewarePipeline middleware;
	middleware.build(
		[](HttpRequest& request)
		{
			return runInternalDispatch(request);
		});
	Http2Options options;
	options.maxApplicationTasks = 1;
	auto service = std::make_shared<Http2ServiceBudget>(1);
	Http2Budget budget(Http2Budget::Role::hServer, options, service.get());
	auto session = Http2ServerSession::create(socket, options, {router, middleware, {}, {}, service, {}});
	Http2Stream::Config config;
	config.streamId = 1;
	config.options = options;
	auto stream = Http2Stream::create(io.get_executor(), config, budget);
	ASSERT_TRUE(stream);
	for (const auto& field : kRequest)
	{
		ASSERT_FALSE(stream->initialHeaders().append(field.name, field.value));
	}
	ASSERT_FALSE(stream->initialHeaders().seal());
	ASSERT_FALSE(Http2ServerSession::mapRequest(*stream, {}));
	{
		auto queued = session->dispatch(stream, Clock::now() + kTestTimeout);
		stream->reset();
		EXPECT_EQ(service->used(), 1U);
		EXPECT_EQ(budget.usage().applicationTasks, 1U);
		config.streamId = 3;
		EXPECT_FALSE(Http2Stream::create(io.get_executor(), config, budget));
	}
	EXPECT_EQ(service->used(), 0U);
	EXPECT_EQ(budget.usage().applicationTasks, 0U);
	EXPECT_TRUE(Http2Stream::create(io.get_executor(), config, budget));
}
