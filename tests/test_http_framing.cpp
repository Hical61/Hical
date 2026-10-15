/**
 * @file test_http_framing.cpp
 * @brief HTTP request framing validation end-to-end tests
 */

#include "core/HttpServer.h"
#include <boost/asio.hpp>
#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace hical;
using boost::asio::ip::tcp;

namespace
{

	struct RawResult
	{
		std::string response;
		bool closed = false;
		bool timedOut = false;
	};

	RawResult exchange(uint16_t port, std::string_view request)
	{
		boost::asio::io_context io;
		tcp::socket socket(io);
		socket.connect(tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), port));
		boost::asio::write(socket, boost::asio::buffer(request.data(), request.size()));

		RawResult result;
		boost::asio::steady_timer timer(io, std::chrono::seconds(2));
		timer.async_wait(
			[&](const boost::system::error_code& ec)
			{
				if (!ec)
				{
					result.timedOut = true;
					boost::system::error_code ignored;
					socket.cancel(ignored);
				}
			});

		boost::asio::async_read(socket,
								boost::asio::dynamic_buffer(result.response, 64 * 1024),
								[&](const boost::system::error_code& ec, std::size_t)
								{
									result.closed =
										ec == boost::asio::error::eof || ec == boost::asio::error::connection_reset;
									timer.cancel();
								});
		io.run();
		return result;
	}

	unsigned firstStatus(std::string_view response)
	{
		if (response.size() < 12 || response.substr(0, 9) != "HTTP/1.1 ")
		{
			return 0;
		}
		return static_cast<unsigned>((response[9] - '0') * 100 + (response[10] - '0') * 10 + response[11] - '0');
	}

	size_t countStatus(std::string_view response, std::string_view status)
	{
		size_t count = 0;
		for (size_t pos = 0; (pos = response.find(status, pos)) != std::string_view::npos; pos += status.size())
		{
			++count;
		}
		return count;
	}

	class HttpFramingTest : public ::testing::Test
	{
	protected:
		void SetUp() override
		{
			server_ = std::make_unique<HttpServer>(0);
			server_->use(
				[this](HttpRequest&) -> SyncMiddlewareResult
				{
					middlewareCalls_.fetch_add(1, std::memory_order_relaxed);
					return std::nullopt;
				});
			server_->router().post("/echo",
								   [this](const HttpRequest& request) -> HttpResponse
								   {
									   echoCalls_.fetch_add(1, std::memory_order_relaxed);
									   return HttpResponse::ok(request.body());
								   });
			server_->router().get("/next",
								  [this](const HttpRequest&) -> HttpResponse
								  {
									  nextCalls_.fetch_add(1, std::memory_order_relaxed);
									  return HttpResponse::ok("next");
								  });

			serverThread_ = std::thread(
				[this]
				{
					server_->start();
				});
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
			while (server_->port() == 0 && std::chrono::steady_clock::now() < deadline)
			{
				std::this_thread::yield();
			}
			ASSERT_NE(server_->port(), 0);
		}

		void TearDown() override
		{
			server_->stop();
			serverThread_.join();
		}

		uint16_t port() const
		{
			return server_->port();
		}

		std::unique_ptr<HttpServer> server_;
		std::thread serverThread_;
		std::atomic<size_t> middlewareCalls_ {0};
		std::atomic<size_t> echoCalls_ {0};
		std::atomic<size_t> nextCalls_ {0};
	};

	TEST_F(HttpFramingTest, RejectsAmbiguousOrInvalidFramingBeforePipeline)
	{
		struct Case
		{
			const char* name;
			std::string request;
			unsigned status;
		};

		const std::string prefix = "POST /echo HTTP/1.1\r\nHost: localhost\r\n";
		const std::string expect = "Expect: 100-continue\r\n";
		const std::vector<Case> cases {
			{"empty CL", prefix + "Content-Length:\r\n\r\n", 400},
			{"negative CL", prefix + "Content-Length: -1\r\n\r\n", 400},
			{"signed CL", prefix + "Content-Length: +1\r\n\r\n", 400},
			{"trailing junk CL", prefix + "Content-Length: 1x\r\n\r\n", 400},
			{"internal whitespace CL", prefix + "Content-Length: 1 0\r\n\r\n", 400},
			{"overflow CL", prefix + "Content-Length: 184467440737095516160\r\n\r\n", 400},
			{"comma list CL", prefix + "Content-Length: 1, 1\r\n\r\n", 400},
			{"conflicting CL", prefix + "Content-Length: 1\r\ncontent-length: 2\r\n\r\n", 400},
			{"folded CL", prefix + "Content-Length: 1\r\n 0\r\n\r\n", 400},
			{"CL then TE", prefix + "Content-Length: 3\r\nTransfer-Encoding: chunked\r\n" + expect + "\r\n", 400},
			{"TE then CL", prefix + "Transfer-Encoding: chunked\r\nContent-Length: 3\r\n" + expect + "\r\n", 400},
			{"CL zero and TE", prefix + "Content-Length: 0\r\nTransfer-Encoding: chunked\r\n" + expect + "\r\n", 400},
			{"duplicate TE", prefix + "Transfer-Encoding: chunked\r\ntransfer-encoding: chunked\r\n\r\n", 400},
			{"empty TE", prefix + "Transfer-Encoding:\r\n\r\n", 400},
			{"non-chunked TE", prefix + "Transfer-Encoding: gzip\r\n\r\n", 400},
			{"empty TE token", prefix + "Transfer-Encoding: gzip,,chunked\r\n\r\n", 400},
			{"chunked not final", prefix + "Transfer-Encoding: chunked, gzip\r\n\r\n", 400},
			{"repeated chunked", prefix + "Transfer-Encoding: chunked, chunked\r\n\r\n", 400},
			{"chunked parameter", prefix + "Transfer-Encoding: chunked;foo=bar\r\n\r\n", 400},
			{"HTTP 1.0 TE", "POST /echo HTTP/1.0\r\nHost: localhost\r\nTransfer-Encoding: chunked\r\n\r\n", 400},
			{"unsupported coding", prefix + "Transfer-Encoding: gzip, chunked\r\n\r\n", 501},
		};
		const std::string next = "GET /next HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";

		for (const auto& testCase : cases)
		{
			SCOPED_TRACE(testCase.name);
			auto result = exchange(port(), testCase.request + next);
			EXPECT_FALSE(result.timedOut);
			EXPECT_TRUE(result.closed);
			EXPECT_EQ(firstStatus(result.response), testCase.status);
			EXPECT_EQ(middlewareCalls_.load(std::memory_order_relaxed), 0u);
			EXPECT_EQ(echoCalls_.load(std::memory_order_relaxed), 0u);
			EXPECT_EQ(nextCalls_.load(std::memory_order_relaxed), 0u);
		}

		auto expectResult =
			exchange(port(), prefix + "Content-Length: 3\r\nTransfer-Encoding: chunked\r\n" + expect + "\r\n");
		EXPECT_FALSE(expectResult.timedOut);
		EXPECT_TRUE(expectResult.closed);
		EXPECT_EQ(firstStatus(expectResult.response), 400u);
		EXPECT_EQ(expectResult.response.find(" 100 "), std::string::npos);
		EXPECT_EQ(middlewareCalls_.load(std::memory_order_relaxed), 0u);
		EXPECT_EQ(echoCalls_.load(std::memory_order_relaxed), 0u);
		EXPECT_EQ(nextCalls_.load(std::memory_order_relaxed), 0u);
	}

	TEST_F(HttpFramingTest, AcceptsCanonicalFramingAndPreservesPipelineBoundaries)
	{
		struct Case
		{
			const char* name;
			std::string firstRequest;
			std::string expectedBody;
		};

		const std::string prefix = "POST /echo HTTP/1.1\r\nHost: localhost\r\n";
		const std::vector<Case> cases {
			{"single CL", prefix + "Content-Length: 3\r\n\r\nabc", "abc"},
			{"same duplicate CL", prefix + "Content-Length: 3\r\ncontent-length: 3\r\n\r\nabc", "abc"},
			{"zero CL", prefix + "Content-Length: 0\r\n\r\n", ""},
			{"no body", prefix + "\r\n", ""},
			{"chunked casing and tab OWS", prefix + "tRaNsFeR-EnCoDiNg:\tChUnKeD\r\n\r\n3\r\nabc\r\n0\r\n\r\n", "abc"},
		};
		const std::string next = "GET /next HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";

		for (size_t i = 0; i < cases.size(); ++i)
		{
			SCOPED_TRACE(cases[i].name);
			auto result = exchange(port(), cases[i].firstRequest + next);
			EXPECT_FALSE(result.timedOut);
			EXPECT_TRUE(result.closed);
			EXPECT_EQ(countStatus(result.response, "HTTP/1.1 200"), 2u);
			const auto firstHeaderEnd = result.response.find("\r\n\r\n");
			ASSERT_NE(firstHeaderEnd, std::string::npos);
			const auto firstBody = firstHeaderEnd + 4;
			EXPECT_EQ(result.response.substr(firstBody, cases[i].expectedBody.size()), cases[i].expectedBody);
			const auto secondResponse = firstBody + cases[i].expectedBody.size();
			EXPECT_EQ(result.response.substr(secondResponse, 12), "HTTP/1.1 200");
			EXPECT_NE(result.response.find("next", secondResponse), std::string::npos);
			EXPECT_EQ(middlewareCalls_.load(std::memory_order_relaxed), (i + 1) * 2);
			EXPECT_EQ(echoCalls_.load(std::memory_order_relaxed), i + 1);
			EXPECT_EQ(nextCalls_.load(std::memory_order_relaxed), i + 1);
		}
	}

} // namespace
