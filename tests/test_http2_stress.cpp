/**
 * @file test_http2_stress.cpp
 * @brief HTTP/2 复位、跨连接配额、慢上传和文件取消压力测试
 */

#include "core/http2/Http2ServerSession.h"
#include "core/Middleware.h"
#include "core/RequestDispatch.h"
#include "core/Router.h"
#include <boost/asio/cancel_at.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/asio/write.hpp>
#include <gtest/gtest.h>
#ifdef _WIN32
	#include <Windows.h>
#endif
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <map>
#include <set>
#include <stdexcept>
#include <vector>

using namespace hical;
using namespace hical::detail;
using namespace boost::asio::experimental::awaitable_operators;

namespace
{
	using Codec = Http2Codec;
	using Clock = std::chrono::steady_clock;
	using TimePoint = Clock::time_point;
	using Timer = boost::asio::steady_timer;
	constexpr auto kTestTimeout = std::chrono::seconds {15};
	constexpr size_t kResetRequests = 32;
	constexpr size_t kFileResetRounds = 16;
	constexpr size_t kFileRounds = kFileResetRounds + 8;
	constexpr size_t kFileBytes = 256 * 1024;
	constexpr size_t kMaximumObservedBody = 64 * 1024;
	constexpr std::string_view kHeldBody = "retained-request-payload";
	constexpr std::array<Codec::Header, 4> kRequest {
		{{":method", "GET"}, {":scheme", "https"}, {":authority", "localhost"}, {":path", "/neighbor"}}};

	struct Gate
	{
		Gate(boost::asio::any_io_executor executor, TimePoint deadline) : timer(executor, deadline)
		{
		}

		void release()
		{
			open = true;
			timer.cancel();
		}

		Awaitable<void> wait()
		{
			if (!open)
			{
				boost::system::error_code ignored;
				co_await timer.async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, ignored));
			}
			EXPECT_TRUE(open) << "显式应用门到期前没有自然放行";
		}

		Timer timer;
		bool open {false};
	};

	struct Counts
	{
		size_t entered {0};
		size_t exited {0};
		size_t active {0};
		size_t maximum {0};
	};

	struct FrameProbe
	{
		explicit FrameProbe(Counts& owner) : counts(owner)
		{
			++counts.entered;
			++counts.active;
			counts.maximum = std::max(counts.maximum, counts.active);
		}

		~FrameProbe()
		{
			--counts.active;
			++counts.exited;
		}

		Counts& counts;
	};

	struct FixedBody : Codec::DataSource
	{
		Codec::DataRead read(std::span<char> output) override
		{
			const auto count = std::min(output.size(), kHeldBody.size() - offset);
			std::memcpy(output.data(), kHeldBody.data() + offset, count);
			offset += count;
			return {count, offset == kHeldBody.size(), false, {}};
		}

		size_t offset {0};
	};

	struct TrickleBody : Codec::DataSource
	{
		Codec::DataRead read(std::span<char> output) override
		{
			if (!ready || output.empty())
			{
				return {0, false, true, {}};
			}
			ready = false;
			output[0] = 'u';
			return {1, false, false, {}};
		}

		void onClose(Http2Code) override
		{
			closed = true;
		}

		bool ready {false};
		bool closed {false};
	};

	struct Peer : Codec::Listener
	{
		static Http2Options smallWindow()
		{
			Http2Options options;
			options.initialStreamWindow = 1024;
			return options;
		}

		Codec codec {Codec::Role::hClient, smallWindow(), this};
		bool settings {false};
		bool consume {true};
		bool goaway {false};
		size_t pingAcks {0};
		size_t observedBytes {0};
		std::map<int32_t, uint16_t> status;
		std::map<int32_t, std::string> body;
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
			if (bytes.size() > kMaximumObservedBody - observedBytes)
			{
				ADD_FAILURE() << "对端响应超过压力夹具的有限容量";
				return Http2Code::hEnhanceYourCalm;
			}
			observedBytes += bytes.size();
			body[id].append(bytes);
			if (consume)
			{
				EXPECT_FALSE(codec.consume(id, bytes.size()));
			}
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

		void onGoaway(int32_t, Http2Code, std::string_view) override
		{
			goaway = true;
		}
	};

	struct Link
	{
		Link(boost::asio::io_context& io,
			 boost::asio::ssl::context& serverContext,
			 boost::asio::ssl::context& clientContext)
			: server(io, serverContext), client(io, clientContext)
		{
			client.set_verify_mode(boost::asio::ssl::verify_none);
		}

		void close()
		{
			boost::system::error_code ignored;
			server.next_layer().close(ignored);
			client.next_layer().close(ignored);
		}

		Http2Connection::TlsSocket server;
		Http2Connection::TlsSocket client;
		Peer peer;
		std::shared_ptr<Http2ServerSession> session;
		std::future<NetworkError> finished;
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

		explicit Harness(size_t maximumTasks = 32) : service(std::make_shared<Http2ServiceBudget>(maximumTasks))
		{
			options.maxConcurrentStreams = 2;
			options.maxApplicationTasks = 32;
			options.maxServerApplicationTasks = maximumTasks;
			options.maxConnectionSendBytes = 32 * 1024;
			options.maxOutputBatchBytes = 4096;
			options.maxFilePrefetchBytes = 4096;
			options.maxBodyBytes = 4096;
			options.maxConnectionReceiveBytes = 64 * 1024;
			router.get("/neighbor",
					   [](const HttpRequest&)
					   {
						   return HttpResponse::ok("neighbor");
					   });
		}

		void close()
		{
			held.release();
			neighbor.release();
			for (const auto& link : links)
			{
				link->close();
			}
		}

		Awaitable<void> write(Link& link, std::string_view bytes)
		{
			co_await boost::asio::async_write(link.client,
											  boost::asio::buffer(bytes),
											  boost::asio::cancel_at(deadline, boost::asio::use_awaitable));
		}

		Awaitable<void> flush(Link& link)
		{
			size_t total = 0;
			for (size_t attempt = 0; attempt < 128; ++attempt)
			{
				auto batch = link.peer.codec.memSend();
				if (batch.error)
				{
					throw std::runtime_error(batch.error.message);
				}
				if (batch.bytes.empty())
				{
					co_return;
				}
				if (batch.bytes.size() > kMaximumObservedBody - total)
				{
					throw std::runtime_error("单轮出站协议数据超过压力夹具容量");
				}
				total += batch.bytes.size();
				co_await write(link, batch.bytes);
			}
			throw std::runtime_error("出站协议输出没有在有限轮次内收敛");
		}

		Awaitable<void> read(Link& link, TimePoint limit = TimePoint::max())
		{
			std::array<char, 16384> buffer;
			const auto count = co_await link.client.async_read_some(
				boost::asio::buffer(buffer),
				boost::asio::cancel_at(std::min(deadline, limit), boost::asio::use_awaitable));
			const auto result = link.peer.codec.memRecv({buffer.data(), count});
			if (result.error || result.consumed != count || count == 0)
			{
				throw std::runtime_error("压力测试对端协议解析没有完整推进");
			}
		}

		Awaitable<Link*> connect()
		{
			if (!built)
			{
				middleware.build(
					[](HttpRequest& request)
					{
						return runInternalDispatch(request);
					});
				built = true;
			}
			auto link = std::make_unique<Link>(io, serverContext, clientContext);
			auto* current = link.get();
			links.push_back(std::move(link));
			co_await (
				acceptor.async_accept(current->server.next_layer(), boost::asio::use_awaitable)
				&& current->client.next_layer().async_connect(acceptor.local_endpoint(), boost::asio::use_awaitable));
			co_await (
				current->server.async_handshake(boost::asio::ssl::stream_base::server, boost::asio::use_awaitable)
				&& current->client.async_handshake(boost::asio::ssl::stream_base::client, boost::asio::use_awaitable));
			current->session =
				Http2ServerSession::create(current->server, options, {router, middleware, {}, {}, service, {}});
			current->finished = boost::asio::co_spawn(io, current->session->run(), boost::asio::use_future);
			co_await flush(*current);
			for (size_t attempt = 0; attempt < 8 && !current->peer.settings; ++attempt)
			{
				co_await read(*current);
			}
			EXPECT_TRUE(current->peer.settings);
			co_await flush(*current);
			co_return current;
		}

		Awaitable<int32_t> request(Link& link, std::string_view path, bool withBody = false)
		{
			auto fields = kRequest;
			fields[0].value = withBody ? "POST" : "GET";
			fields[3].value = path;
			const auto result = withBody ? link.peer.codec.submitRequest(fields, std::make_shared<FixedBody>())
										 : link.peer.codec.submitRequest(fields);
			if (result.error)
			{
				throw std::runtime_error(result.error.message);
			}
			co_await flush(link);
			co_return result.streamId;
		}

		Awaitable<void> ping(Link& link)
		{
			constexpr size_t kFrameHeaderBytes = 9;
			constexpr size_t kPingBytes = 8;
			std::string bytes(kFrameHeaderBytes, '\0');
			bytes[2] = static_cast<char>(kPingBytes);
			bytes[3] = static_cast<char>(Codec::FrameType::hPing);
			bytes += "stress!!";
			co_await write(link, bytes);
		}

		Awaitable<void> barrier(Link& link)
		{
			const auto expected = link.peer.pingAcks + 1;
			co_await ping(link);
			while (link.peer.pingAcks < expected)
			{
				co_await read(link);
				co_await flush(link);
			}
		}

		Awaitable<void> complete(Link& link, int32_t id)
		{
			while (!link.peer.ended.contains(id) && !link.peer.closed.contains(id))
			{
				co_await read(link);
				co_await flush(link);
			}
		}

		template <class Predicate>
		Awaitable<void> until(Predicate predicate)
		{
			Timer poll(io);
			while (!predicate())
			{
				if (Clock::now() >= deadline)
				{
					throw std::runtime_error("资源状态没有在绝对截止点前收敛");
				}
				poll.expires_at(std::min(deadline, Clock::now() + std::chrono::milliseconds {1}));
				co_await poll.async_wait(boost::asio::use_awaitable);
			}
		}

		Awaitable<void> retire(Link& link)
		{
			link.close();
			co_await until(
				[&]
				{
					return link.finished.wait_for(std::chrono::seconds {0}) == std::future_status::ready;
				});
			EXPECT_NO_THROW(static_cast<void>(link.finished.get()));
			const auto weak = std::weak_ptr(link.session);
			link.session.reset();
			EXPECT_TRUE(weak.expired()) << "run 收拢后角色会话仍被后台任务持有";
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
			watchdog.expires_at(deadline);
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
			EXPECT_FALSE(timedOut);
			EXPECT_NO_THROW(result.get());
			for (const auto& link : links)
			{
				if (link->finished.valid())
				{
					const auto ready = link->finished.wait_for(std::chrono::seconds {0});
					EXPECT_EQ(ready, std::future_status::ready);
					if (ready == std::future_status::ready)
					{
						EXPECT_NO_THROW(static_cast<void>(link->finished.get()));
					}
				}
				const auto weak = std::weak_ptr(link->session);
				link->session.reset();
				EXPECT_TRUE(weak.expired());
			}
			io.restart();
			io.run();
			EXPECT_EQ(service->used(), 0U);
		}

		boost::asio::io_context io;
		TimePoint deadline {Clock::now() + kTestTimeout};
		boost::asio::ssl::context serverContext {makeServerContext()};
		boost::asio::ssl::context clientContext {boost::asio::ssl::context::tls_client};
		boost::asio::ip::tcp::acceptor acceptor {io, {boost::asio::ip::address_v4::loopback(), 0}};
		Timer watchdog {io};
		Gate held {io.get_executor(), deadline};
		Gate neighbor {io.get_executor(), deadline};
		Router router;
		MiddlewarePipeline middleware;
		Http2Options options;
		std::shared_ptr<Http2ServiceBudget> service;
		std::vector<std::unique_ptr<Link>> links;
		bool built {false};
		bool timedOut {false};
	};

	struct TemporaryFile
	{
		TemporaryFile()
		{
			path = std::filesystem::temp_directory_path()
				   / ("hical-h2-stress-" + std::to_string(Clock::now().time_since_epoch().count()) + ".bin");
			const std::string bytes(kFileBytes, 'f');
			std::ofstream output(path, std::ios::binary);
			output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
			if (!output)
			{
				throw std::runtime_error("无法创建压力测试临时文件");
			}
		}

		~TemporaryFile()
		{
			std::error_code ignored;
			std::filesystem::remove(path, ignored);
		}

		void expectClosed() const
		{
#ifdef _WIN32
			const auto handle =
				CreateFileW(path.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
			EXPECT_NE(handle, INVALID_HANDLE_VALUE) << "取消后的文件句柄仍持有共享打开状态";
			if (handle != INVALID_HANDLE_VALUE)
			{
				CloseHandle(handle);
			}
#elif defined(__linux__)
			size_t matching = 0;
			for (const auto& entry : std::filesystem::directory_iterator("/proc/self/fd"))
			{
				std::error_code ignored;
				matching += std::filesystem::read_symlink(entry.path(), ignored) == path;
			}
			EXPECT_EQ(matching, 0U);
#else
			EXPECT_TRUE(std::filesystem::exists(path));
#endif
		}

		std::filesystem::path path;
	};

	class Http2StressTlsTest : public testing::Test
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

TEST_F(Http2StressTlsTest, ResetStreamsReuseProtocolSlotsButRetainSharedRunningTaskQuota)
{
	Harness harness;
	Counts held;
	Counts neighbor;
	harness.router.post("/hold",
						[&](const HttpRequest& request) -> Awaitable<HttpResponse>
						{
							FrameProbe frame(held);
							EXPECT_EQ(request.body(), kHeldBody);
							co_await harness.held.wait();
							EXPECT_EQ(request.path(), "/hold");
							EXPECT_EQ(request.body(), kHeldBody);
							co_return HttpResponse::ok("retired");
						});
	harness.router.get("/waiting-neighbor",
					   [&](const HttpRequest&) -> Awaitable<HttpResponse>
					   {
						   FrameProbe frame(neighbor);
						   co_await harness.neighbor.wait();
						   co_return HttpResponse::ok("survived");
					   });
	auto scenario = [&]() -> Awaitable<void>
	{
		auto* first = co_await harness.connect();
		auto* second = co_await harness.connect();
		const auto survivor = co_await harness.request(*first, "/waiting-neighbor");
		co_await harness.until(
			[&]
			{
				return neighbor.entered == 1;
			});
		for (size_t index = 0; index + 1 < kResetRequests; ++index)
		{
			auto& link = index % 2 == 0 ? *first : *second;
			const auto id = co_await harness.request(link, "/hold", true);
			co_await harness.until(
				[&]
				{
					return held.entered == index + 1;
				});
			EXPECT_FALSE(link.peer.codec.reset(id));
			co_await harness.flush(link);
			co_await harness.barrier(link);
			EXPECT_EQ(held.exited, 0U);
			EXPECT_EQ(harness.service->used(), index + 2);
		}
		EXPECT_EQ(held.active, kResetRequests - 1);
		EXPECT_EQ(harness.service->used(), 32U);
		auto* third = co_await harness.connect();
		for (auto* link : {first, second, third})
		{
			const auto rejected = co_await harness.request(*link, "/hold", true);
			co_await harness.complete(*link, rejected);
			EXPECT_EQ(link->peer.closed.at(rejected), Http2Code::hRefusedStream);
			EXPECT_FALSE(link->peer.goaway);
		}
		EXPECT_EQ(held.entered, kResetRequests - 1);
		EXPECT_EQ(held.exited, 0U);
		harness.neighbor.release();
		co_await harness.complete(*first, survivor);
		EXPECT_EQ(first->peer.status.at(survivor), 200);
		EXPECT_EQ(first->peer.body.at(survivor), "survived");
		co_await harness.until(
			[&]
			{
				return harness.service->used() == kResetRequests - 1;
			});
		const auto healthy = co_await harness.request(*third, "/neighbor");
		co_await harness.complete(*third, healthy);
		EXPECT_EQ(third->peer.body.at(healthy), "neighbor");
		co_await harness.until(
			[&]
			{
				return harness.service->used() == kResetRequests - 1;
			});
		const auto lastReset = co_await harness.request(*second, "/hold", true);
		co_await harness.until(
			[&]
			{
				return held.entered == kResetRequests;
			});
		EXPECT_FALSE(second->peer.codec.reset(lastReset));
		co_await harness.flush(*second);
		co_await harness.barrier(*second);
		EXPECT_EQ(harness.service->used(), kResetRequests);
		EXPECT_EQ(held.exited, 0U);
		harness.held.release();
		co_await harness.until(
			[&]
			{
				return held.exited == kResetRequests && harness.service->used() == 0;
			});
		EXPECT_EQ(held.active, 0U);
		EXPECT_EQ(held.maximum, kResetRequests);
		EXPECT_EQ(neighbor.exited, 1U);
		for (auto* link : {first, second, third})
		{
			const auto recovered = co_await harness.request(*link, "/neighbor");
			co_await harness.complete(*link, recovered);
			EXPECT_EQ(link->peer.body.at(recovered), "neighbor");
			EXPECT_FALSE(link->peer.goaway);
		}
	};
	harness.run(scenario());
	EXPECT_EQ(held.entered, kResetRequests);
	EXPECT_EQ(held.exited, kResetRequests);
	EXPECT_EQ(held.active, 0U);
}

TEST_F(Http2StressTlsTest, ConcurrentSlowUploadsAndPingsDoNotExtendBodyDeadlinesOrHarmANeighbor)
{
	Harness harness(16);
	harness.options.maxConcurrentStreams = 16;
	harness.options.bodyTimeout = std::chrono::milliseconds {120};
	size_t uploadsDispatched = 0;
	harness.router.post("/upload",
						[&](const HttpRequest&)
						{
							++uploadsDispatched;
							return HttpResponse::ok("unexpected complete body");
						});
	auto scenario = [&]() -> Awaitable<void>
	{
		auto& link = *co_await harness.connect();
		std::array<std::shared_ptr<TrickleBody>, 8> bodies;
		std::array<int32_t, 8> ids {};
		auto fields = kRequest;
		fields[0].value = "POST";
		fields[3].value = "/upload";
		for (size_t index = 0; index < bodies.size(); ++index)
		{
			bodies[index] = std::make_shared<TrickleBody>();
			const auto submitted = link.peer.codec.submitRequest(fields, bodies[index]);
			EXPECT_FALSE(submitted.error);
			ids[index] = submitted.streamId;
		}
		const auto neighbor = co_await harness.request(link, "/neighbor");
		bool finished = false;
		Timer cadence(harness.io);
		const auto responseDeadline = Clock::now() + std::chrono::seconds {1};
		auto produce = [&]() -> Awaitable<void>
		{
			auto nextByte = Clock::now();
			for (size_t attempt = 0; attempt < 256 && !finished; ++attempt)
			{
				for (size_t index = 0; index < bodies.size(); ++index)
				{
					if (!bodies[index]->closed)
					{
						bodies[index]->ready = true;
						EXPECT_FALSE(link.peer.codec.resume(ids[index]));
					}
				}
				co_await harness.flush(link);
				co_await harness.ping(link);
				nextByte += std::chrono::milliseconds {10};
				cadence.expires_at(nextByte);
				boost::system::error_code ignored;
				co_await cadence.async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, ignored));
			}
			if (!finished)
			{
				throw std::runtime_error("慢上传没有在有限输入轮次内结束");
			}
		};
		auto consume = [&]() -> Awaitable<void>
		{
			const auto allEnded = [&]
			{
				return link.peer.ended.contains(neighbor)
					   && std::ranges::all_of(ids,
											  [&](int32_t id)
											  {
												  return link.peer.ended.contains(id);
											  });
			};
			while (!allEnded())
			{
				co_await harness.read(link, responseDeadline);
			}
			finished = true;
			cadence.cancel();
		};
		// 一读一写分别运行；consume 不另写 ACK，避免测试夹具自身并发写 TLS。
		co_await (produce() && consume());
		co_await harness.barrier(link);
		while (!std::ranges::all_of(bodies,
									[](const auto& body)
									{
										return body->closed;
									}))
		{
			co_await harness.read(link, responseDeadline);
		}
		EXPECT_GT(link.peer.pingAcks, 0U);
		EXPECT_EQ(link.peer.body.at(neighbor), "neighbor");
		for (const auto id : ids)
		{
			EXPECT_EQ(link.peer.status.at(id), 408);
			EXPECT_TRUE(link.peer.ended.contains(id));
		}
		EXPECT_EQ(uploadsDispatched, 0U);
		EXPECT_FALSE(link.peer.goaway);
		co_await harness.until(
			[&]
			{
				return harness.service->used() == 0;
			});
	};
	harness.run(scenario());
}

TEST_F(Http2StressTlsTest, RepeatedWindowBlockedFilesResetAndForceCloseWithoutRetainedTasksOrHandles)
{
	TemporaryFile file;
	Harness harness(4);
	harness.router.get(
		"/file",
		[&](const HttpRequest&)
		{
			HttpResponse response;
			response.setFileBody(file.path, 0, static_cast<int64_t>(kFileBytes), "application/octet-stream");
			return response;
		});
	auto scenario = [&]() -> Awaitable<void>
	{
		Link* link = nullptr;
		for (size_t round = 0; round < kFileRounds; ++round)
		{
			SCOPED_TRACE(round);
			if (!link)
			{
				link = co_await harness.connect();
			}
			link->peer.consume = false;
			const auto id = co_await harness.request(*link, "/file");
			while (link->peer.body[id].empty())
			{
				co_await harness.read(*link);
			}
			EXPECT_EQ(link->peer.status.at(id), 200);
			EXPECT_EQ(harness.service->used(), 1U);
			EXPECT_FALSE(link->peer.ended.contains(id));
			// 前十六次复用同一连接，累计预取量超过连接预算，检查取消后是否漏还配额。
			if (round < kFileResetRounds)
			{
				EXPECT_FALSE(link->peer.codec.reset(id));
				co_await harness.flush(*link);
				co_await harness.barrier(*link);
				co_await harness.until(
					[&]
					{
						return harness.service->used() == 0;
					});
				link->peer.consume = true;
				const auto neighbor = co_await harness.request(*link, "/neighbor");
				co_await harness.complete(*link, neighbor);
				EXPECT_EQ(link->peer.body.at(neighbor), "neighbor");
				EXPECT_FALSE(link->peer.goaway);
				co_await harness.until(
					[&]
					{
						return harness.service->used() == 0;
					});
			}
			else
			{
				co_await harness.retire(*link);
				EXPECT_EQ(harness.service->used(), 0U);
				link = nullptr;
			}
			file.expectClosed();
		}
	};
	harness.run(scenario());
	file.expectClosed();
}
