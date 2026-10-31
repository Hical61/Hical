/**
 * @file test_http2_file.cpp
 * @brief TLS 下的 HTTP/2 文件发送、Range 和取消隔离测试
 */

#include "core/http2/Http2ServerSession.h"
#include "core/Middleware.h"
#include "core/RequestDispatch.h"
#include "core/Router.h"
#include "core/StaticFiles.h"
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/use_future.hpp>
#include <gtest/gtest.h>
#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <map>
#include <set>
#include <stdexcept>
#include <string>

using namespace hical;
using namespace hical::detail;
using namespace boost::asio::experimental::awaitable_operators;

namespace
{
	using Codec = Http2Codec;
	using Clock = std::chrono::steady_clock;
	using TimePoint = Clock::time_point;
	constexpr auto kTestTimeout = std::chrono::seconds {10};
	constexpr size_t kFileBytes = 256 * 1024;
	constexpr std::array<Codec::Header, 4> kRequest {
		{{":method", "GET"}, {":scheme", "https"}, {":authority", "localhost"}, {":path", "/file"}}};

	struct TemporaryFile
	{
		TemporaryFile()
		{
			path = std::filesystem::temp_directory_path()
				   / ("hical-h2-file-" + std::to_string(Clock::now().time_since_epoch().count()) + ".bin");
			body.reserve(kFileBytes);
			for (size_t i = 0; i < kFileBytes; ++i)
			{
				body.push_back(static_cast<char>('a' + i % 23));
			}
			std::ofstream stream(path, std::ios::binary);
			stream.write(body.data(), static_cast<std::streamsize>(body.size()));
			if (!stream)
			{
				throw std::runtime_error("无法创建文件发送测试数据");
			}
		}

		~TemporaryFile()
		{
			std::error_code ignored;
			std::filesystem::remove(path, ignored);
		}

		std::filesystem::path path;
		std::string body;
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
		std::map<int32_t, uint16_t> status;
		std::map<int32_t, HeaderMap> headers;
		std::map<int32_t, std::string> body;
		std::map<int32_t, Http2Code> closed;
		std::set<int32_t> ended;
		bool goaway {false};

		void onSettings(bool ack) override
		{
			settings = settings || !ack;
		}

		Http2Code onHeader(int32_t id, Codec::Header field) override
		{
			if (field.name != ":status")
			{
				headers[id].insert(field.name, field.value);
			}
			return Http2Code::hNoError;
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

		void onGoaway(int32_t, Http2Code, std::string_view) override
		{
			goaway = true;
		}
	};

	struct Harness
	{
		static boost::asio::ssl::context serverContext()
		{
			boost::asio::ssl::context result(boost::asio::ssl::context::tls_server);
			result.use_certificate_chain_file("test_server.crt");
			result.use_private_key_file("test_server.key", boost::asio::ssl::context::pem);
			return result;
		}

		boost::asio::io_context io;
		boost::asio::ssl::context serverTls {serverContext()};
		boost::asio::ssl::context clientTls {boost::asio::ssl::context::tls_client};
		boost::asio::ip::tcp::acceptor acceptor {io, {boost::asio::ip::address_v4::loopback(), 0}};
		Http2Connection::TlsSocket server {io, serverTls};
		Http2Connection::TlsSocket client {io, clientTls};
		boost::asio::steady_timer watchdog {io};
		Router router;
		MiddlewarePipeline middleware;
		Http2Options options;
		std::shared_ptr<Http2ServiceBudget> service = std::make_shared<Http2ServiceBudget>(64);
		std::shared_ptr<Http2ServerSession> session;
		std::future<NetworkError> finished;
		bool timedOut {false};

		Harness()
		{
			options.maxFilePrefetchBytes = 4096;
			options.maxOutputBatchBytes = 4096;
			options.maxConnectionSendBytes = 32 * 1024;
			client.set_verify_mode(boost::asio::ssl::verify_none);
			router.get("/neighbor",
					   [](const HttpRequest&)
					   {
						   return HttpResponse::ok("neighbor");
					   });
		}

		void close()
		{
			boost::system::error_code ignored;
			server.next_layer().close(ignored);
			client.next_layer().close(ignored);
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
				co_await boost::asio::async_write(client, boost::asio::buffer(batch.bytes), boost::asio::use_awaitable);
			}
		}

		Awaitable<void> read(Peer& peer)
		{
			std::array<char, 16384> buffer;
			const auto size = co_await client.async_read_some(boost::asio::buffer(buffer), boost::asio::use_awaitable);
			auto result = peer.codec.memRecv({buffer.data(), size});
			EXPECT_FALSE(result.error);
			EXPECT_EQ(result.consumed, size);
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
				co_await read(peer);
			}
			co_await flush(peer);
		}

		Awaitable<int32_t> request(Peer& peer, std::string_view path = "/file", HttpMethod method = HttpMethod::hGet)
		{
			auto fields = kRequest;
			fields[0].value = httpMethodToString(method);
			fields[3].value = path;
			auto submitted = peer.codec.submitRequest(fields);
			EXPECT_FALSE(submitted.error);
			co_await flush(peer);
			co_return submitted.streamId;
		}

		Awaitable<void> waitReleased()
		{
			const auto deadline = Clock::now() + kTestTimeout;
			boost::asio::steady_timer timer(io);
			while (service->used() != 0 && Clock::now() < deadline)
			{
				timer.expires_after(std::chrono::milliseconds {1});
				co_await timer.async_wait(boost::asio::use_awaitable);
			}
			EXPECT_EQ(service->used(), 0U);
		}

		Awaitable<void> complete(Peer& peer, int32_t id)
		{
			while (!peer.ended.contains(id) && !peer.closed.contains(id))
			{
				co_await read(peer);
				co_await flush(peer);
			}
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
			EXPECT_FALSE(timedOut);
			EXPECT_NO_THROW(result.get());
			if (finished.valid())
			{
				EXPECT_NO_THROW(static_cast<void>(finished.get()));
			}
			EXPECT_EQ(service->used(), 0U);
		}
	};

	class Http2FileTlsTest : public testing::Test
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

TEST_F(Http2FileTlsTest, LargeFileCrossesSmallWindowAndConnectionBudgetWhileNeighborCompletes)
{
	TemporaryFile file;
	Harness harness;
	harness.router.get(
		"/file",
		[&](const HttpRequest&)
		{
			HttpResponse response;
			response.setFileBody(file.path, 0, static_cast<int64_t>(file.body.size()), "application/octet-stream");
			return response;
		});
	Peer peer;
	auto scenario = [](Harness& harness, Peer& peer, const TemporaryFile& file) -> Awaitable<void>
	{
		co_await harness.start(peer);
		peer.consume = false;
		const auto id = co_await harness.request(peer);
		while (!peer.status.contains(id) || (peer.status[id] == 200 && peer.body[id].empty()))
		{
			co_await harness.read(peer);
		}
		EXPECT_EQ(peer.status[id], 200);
		EXPECT_LE(peer.body[id].size(), 1024U);
		const auto neighbor = co_await harness.request(peer, "/neighbor");
		co_await harness.complete(peer, neighbor);
		EXPECT_EQ(peer.body[neighbor], "neighbor");
		EXPECT_FALSE(peer.ended.contains(id));
		EXPECT_FALSE(peer.codec.consume(id, peer.body[id].size()));
		peer.consume = true;
		co_await harness.flush(peer);
		co_await harness.complete(peer, id);
		EXPECT_EQ(peer.body[id], file.body);
		EXPECT_EQ(peer.headers[id].find("content-length"), std::to_string(file.body.size()));
		EXPECT_TRUE(peer.ended.contains(id));
	};
	harness.run(scenario(harness, peer, file));
}

TEST_F(Http2FileTlsTest, PartialFilePreservesOffsetRangeAndEtag)
{
	TemporaryFile file;
	Harness harness;
	constexpr int64_t kOffset = 717;
	constexpr int64_t kLength = 87001;
	harness.router.get("/file",
					   [&](const HttpRequest&)
					   {
						   HttpResponse response;
						   response.setStatus(HttpStatusCode::hPartialContent);
						   response.setFileBody(file.path, kOffset, kLength, "application/octet-stream");
						   response.setHeader("Content-Range", "bytes 717-87717/262144");
						   response.setHeader("ETag", "\"range-tag\"");
						   return response;
					   });
	Peer peer;
	auto scenario = [](Harness& harness, Peer& peer, const TemporaryFile& file) -> Awaitable<void>
	{
		co_await harness.start(peer);
		const auto id = co_await harness.request(peer);
		co_await harness.complete(peer, id);
		EXPECT_EQ(peer.status[id], 206);
		EXPECT_EQ(peer.body[id], file.body.substr(kOffset, kLength));
		EXPECT_EQ(peer.headers[id].find("content-length"), std::to_string(kLength));
		EXPECT_EQ(peer.headers[id].find("content-range"), "bytes 717-87717/262144");
		EXPECT_EQ(peer.headers[id].find("etag"), "\"range-tag\"");
	};
	harness.run(scenario(harness, peer, file));
}

TEST_F(Http2FileTlsTest, MissingOrTruncatedFileResetsOnlyItsStream)
{
	for (const bool missing : {false, true})
	{
		TemporaryFile file;
		if (missing)
		{
			ASSERT_TRUE(std::filesystem::remove(file.path));
		}
		Harness harness;
		harness.router.get("/file",
						   [&](const HttpRequest&)
						   {
							   HttpResponse response;
							   response.setFileBody(file.path,
													0,
													static_cast<int64_t>(file.body.size() + 1),
													"application/octet-stream");
							   return response;
						   });
		Peer peer;
		auto scenario = [](Harness& harness, Peer& peer) -> Awaitable<void>
		{
			co_await harness.start(peer);
			const auto id = co_await harness.request(peer);
			co_await harness.complete(peer, id);
			EXPECT_TRUE(peer.closed.contains(id));
			EXPECT_EQ(peer.closed[id], Http2Code::hInternalError);
			EXPECT_FALSE(peer.ended.contains(id));
			const auto neighbor = co_await harness.request(peer, "/neighbor");
			co_await harness.complete(peer, neighbor);
			EXPECT_EQ(peer.status[neighbor], 200);
			EXPECT_EQ(peer.body[neighbor], "neighbor");
		};
		harness.run(scenario(harness, peer));
	}
}

TEST_F(Http2FileTlsTest, HeadNoContentNotModifiedAndZeroLengthDoNotOpenMissingFile)
{
	TemporaryFile file;
	ASSERT_TRUE(std::filesystem::remove(file.path));
	Harness harness;
	harness.router.get(
		"/file",
		[&](const HttpRequest& req)
		{
			HttpResponse response;
			response.setFileBody(file.path, 0, req.path() == "/zero" ? 0 : 1024, "application/octet-stream");
			return response;
		});
	harness.router.route(HttpMethod::hHead,
						 "/file",
						 [&](const HttpRequest&)
						 {
							 HttpResponse response;
							 response.setFileBody(file.path, 0, 1024, "application/octet-stream");
							 return response;
						 });
	for (auto [path, status] : {std::pair {"/no-content", HttpStatusCode::hNoContent},
								std::pair {"/not-modified", HttpStatusCode::hNotModified}})
	{
		harness.router.get(path,
						   [&, status](const HttpRequest&)
						   {
							   HttpResponse response;
							   response.setFileBody(file.path, 0, 1024, "application/octet-stream");
							   response.setStatus(status);
							   response.setHeader("Content-Length", "123");
							   return response;
						   });
	}
	harness.router.get("/zero",
					   [&](const HttpRequest&)
					   {
						   HttpResponse response;
						   response.setFileBody(file.path, 0, 0, "application/octet-stream");
						   return response;
					   });
	Peer peer;
	auto scenario = [](Harness& harness, Peer& peer) -> Awaitable<void>
	{
		co_await harness.start(peer);
		const auto head = co_await harness.request(peer, "/file", HttpMethod::hHead);
		co_await harness.complete(peer, head);
		EXPECT_EQ(peer.status[head], 200);
		EXPECT_TRUE(peer.body[head].empty());
		EXPECT_EQ(peer.headers[head].find("content-length"), "1024");
		for (auto [path, status] :
			 {std::pair {"/no-content", 204}, std::pair {"/not-modified", 304}, std::pair {"/zero", 200}})
		{
			const auto id = co_await harness.request(peer, path);
			co_await harness.complete(peer, id);
			EXPECT_EQ(peer.status[id], status);
			EXPECT_TRUE(peer.body[id].empty());
			EXPECT_EQ(peer.headers[id].find("content-length"), status == 304 ? "123" : status == 200 ? "0" : "");
			EXPECT_TRUE(peer.ended.contains(id));
		}
	};
	harness.run(scenario(harness, peer));
}

TEST_F(Http2FileTlsTest, ResetPausedFileReleasesTaskAndHandleWithoutBlockingNeighbor)
{
	for (const auto code : {Http2Code::hCancel, Http2Code::hNoError})
	{
		TemporaryFile file;
		Harness harness;
		harness.router.get(
			"/file",
			[&](const HttpRequest&)
			{
				HttpResponse response;
				response.setFileBody(file.path, 0, static_cast<int64_t>(file.body.size()), "application/octet-stream");
				return response;
			});
		Peer peer;
		auto scenario = [](Harness& harness, Peer& peer, const TemporaryFile& file, Http2Code code) -> Awaitable<void>
		{
			co_await harness.start(peer);
			peer.consume = false;
			const auto id = co_await harness.request(peer);
			while (peer.body[id].empty())
			{
				co_await harness.read(peer);
			}
			EXPECT_EQ(harness.service->used(), 1U);
			EXPECT_FALSE(peer.codec.reset(id, code));
			co_await harness.flush(peer);
			peer.consume = true;
			const auto neighbor = co_await harness.request(peer, "/neighbor");
			co_await harness.complete(peer, neighbor);
			EXPECT_EQ(peer.body[neighbor], "neighbor");
			co_await harness.waitReleased();
			EXPECT_FALSE(peer.ended.contains(id));
#ifdef _WIN32
			// 共享模式为零的新句柄只有在生产者的原生文件句柄已关闭后才能打开。
			const auto handle =
				CreateFileW(file.path.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
			EXPECT_NE(handle, INVALID_HANDLE_VALUE);
			if (handle != INVALID_HANDLE_VALUE)
			{
				CloseHandle(handle);
			}
#else
			EXPECT_TRUE(std::filesystem::exists(file.path));
#endif
		};
		harness.run(scenario(harness, peer, file, code));
	}
}

TEST_F(Http2FileTlsTest, ZeroWindowUsesOriginalResponseDeadlineAndOnlyResetsFileStream)
{
	TemporaryFile file;
	Harness harness;
	harness.options.writeTimeout = std::chrono::milliseconds {100};
	harness.router.get(
		"/file",
		[&](const HttpRequest&)
		{
			HttpResponse response;
			response.setFileBody(file.path, 0, static_cast<int64_t>(file.body.size()), "application/octet-stream");
			return response;
		});
	Peer peer;
	auto scenario = [](Harness& harness, Peer& peer) -> Awaitable<void>
	{
		co_await harness.start(peer);
		const std::array<Codec::Setting, 1> blocked {{{Http2Setting::hInitialWindowSize, 0}}};
		EXPECT_FALSE(peer.codec.submitSettings(blocked));
		const auto id = co_await harness.request(peer);
		while (!peer.closed.contains(id))
		{
			co_await harness.read(peer);
		}
		EXPECT_EQ(peer.closed[id], Http2Code::hCancel);
		EXPECT_TRUE(peer.body[id].empty());
		EXPECT_FALSE(peer.ended.contains(id));
		co_await harness.waitReleased();
		const std::array<Codec::Setting, 1> unblocked {{{Http2Setting::hInitialWindowSize, 1024}}};
		EXPECT_FALSE(peer.codec.submitSettings(unblocked));
		const auto neighbor = co_await harness.request(peer, "/neighbor");
		co_await harness.complete(peer, neighbor);
		EXPECT_EQ(peer.body[neighbor], "neighbor");
	};
	harness.run(scenario(harness, peer));
}

TEST_F(Http2FileTlsTest, GracefulDrainFinishesAdmittedFileWhileForcedDisconnectJoinsTask)
{
	for (const bool force : {false, true})
	{
		TemporaryFile file;
		Harness harness;
		harness.router.get(
			"/file",
			[&](const HttpRequest&)
			{
				HttpResponse response;
				response.setFileBody(file.path, 0, static_cast<int64_t>(file.body.size()), "application/octet-stream");
				return response;
			});
		Peer peer;
		auto scenario = [](Harness& harness, Peer& peer, const TemporaryFile& file, bool force) -> Awaitable<void>
		{
			co_await harness.start(peer);
			peer.consume = false;
			const auto id = co_await harness.request(peer);
			while (peer.body[id].empty())
			{
				co_await harness.read(peer);
			}
			EXPECT_EQ(harness.service->used(), 1U);
			harness.session->drain();
			if (force)
			{
				boost::system::error_code ignored;
				// 关闭传输使读泵退出，文件生产协程收到 Output 的取消信号后完成清理。
				harness.client.next_layer().close(ignored);
				co_await harness.waitReleased();
			}
			else
			{
				EXPECT_FALSE(peer.codec.consume(id, peer.body[id].size()));
				peer.consume = true;
				co_await harness.flush(peer);
				co_await harness.complete(peer, id);
				EXPECT_EQ(peer.body[id], file.body);
				EXPECT_TRUE(peer.ended.contains(id));
				EXPECT_TRUE(peer.goaway);
			}
		};
		harness.run(scenario(harness, peer, file, force));
	}
}
