/**
 * @file test_http_framing.cpp
 * @brief HTTP request framing validation end-to-end tests
 */

#include "core/HttpServer.h"
#include <boost/asio.hpp>
#include <gtest/gtest.h>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <random>
#include <sstream>
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

	struct ClientResponse
	{
		unsigned status = 0;
		std::string body;
	};

	/// 从 raw[pos..] 切一个完整响应出来（按 Content-Length 算长度）
	/// @return 吃掉的字节数，凑不齐一个完整响应就返回 0
	size_t takeResponse(std::string_view raw, size_t pos, ClientResponse& out)
	{
		auto headEndAbs = raw.find("\r\n\r\n", pos);
		if (headEndAbs == std::string_view::npos)
		{
			return 0;
		}
		// 相对 pos 的头部总长度，后面算 body 偏移和返回吃掉多少字节都用它
		const size_t headLen = headEndAbs + 4 - pos;

		constexpr std::string_view kName = "content-length:";
		size_t contentLength = 0;
		auto lineStart = raw.find("\r\n", pos);
		lineStart = (lineStart == std::string_view::npos) ? std::string_view::npos : lineStart + 2;
		while (lineStart != std::string_view::npos && lineStart < pos + headLen)
		{
			auto eol = raw.find("\r\n", lineStart);
			if (eol == std::string_view::npos)
			{
				return 0;
			}
			auto line = raw.substr(lineStart, eol - lineStart);
			bool match = line.size() > kName.size();
			for (size_t i = 0; match && i < kName.size(); ++i)
			{
				match = std::tolower(static_cast<unsigned char>(line[i])) == kName[i];
			}
			if (match)
			{
				auto value = line.substr(kName.size());
				contentLength = static_cast<size_t>(std::strtoull(std::string(value).c_str(), nullptr, 10));
			}
			lineStart = eol + 2;
		}

		if (raw.size() < pos + headLen + contentLength)
		{
			return 0;
		}
		out.status = firstStatus(raw.substr(pos));
		out.body.assign(raw.substr(pos + headLen, contentLength));
		return headLen + contentLength;
	}

	/// 流水线客户端：一次把一串请求怼出去，再按「响应个数」收回来
	class PipelineClient
	{
	public:
		explicit PipelineClient(uint16_t port) : socket_(io_)
		{
			socket_.connect(tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), port));
		}

		void send(std::string_view bytes)
		{
			boost::asio::write(socket_, boost::asio::buffer(bytes.data(), bytes.size()));
		}

		/// 读到攒够 want 个完整响应为止。超时就关连接并置 timedOut，让用例自己去断言——
		/// 这里抛异常的话现场（收到了几个、收到哪一步）全丢了
		std::vector<ClientResponse> read(size_t want, std::chrono::milliseconds timeout = std::chrono::seconds(3))
		{
			timedOut_ = false;
			std::vector<ClientResponse> got;
			drain(got);
			if (got.size() >= want)
			{
				return got;
			}

			boost::asio::steady_timer timer(io_, timeout);
			timer.async_wait(
				[&](const boost::system::error_code& ec)
				{
					if (!ec)
					{
						timedOut_ = true;
						boost::system::error_code ignored;
						socket_.close(ignored);
					}
				});

			std::function<void(const boost::system::error_code&, size_t)> onRead;
			char chunk[4096];
			onRead = [&](const boost::system::error_code& ec, size_t n)
			{
				if (ec)
				{
					// eof / 超时关闭 / 主动取消，都从这里退；定时器得一起收掉，不然 run() 要等满超时
					timer.cancel();
					return;
				}
				raw_.append(chunk, n);
				drain(got);
				if (got.size() >= want)
				{
					// 够数了，把还挂着的读取消掉，run() 才能返回
					boost::system::error_code ignored;
					socket_.cancel(ignored);
					timer.cancel();
					return;
				}
				socket_.async_read_some(boost::asio::buffer(chunk, sizeof(chunk)), onRead);
			};
			socket_.async_read_some(boost::asio::buffer(chunk, sizeof(chunk)), onRead);
			io_.restart();
			io_.run();
			return got;
		}

		/// 一直读到对端关连接（或超时），返回期间收到的全部原始字节
		std::string readToEnd(std::chrono::milliseconds timeout = std::chrono::seconds(3))
		{
			timedOut_ = false;
			boost::asio::steady_timer timer(io_, timeout);
			timer.async_wait(
				[&](const boost::system::error_code& ec)
				{
					if (!ec)
					{
						timedOut_ = true;
						boost::system::error_code ignored;
						socket_.close(ignored);
					}
				});

			std::function<void(const boost::system::error_code&, size_t)> onRead;
			char chunk[4096];
			onRead = [&](const boost::system::error_code& ec, size_t n)
			{
				if (ec)
				{
					timer.cancel();
					return;
				}
				raw_.append(chunk, n);
				socket_.async_read_some(boost::asio::buffer(chunk, sizeof(chunk)), onRead);
			};
			socket_.async_read_some(boost::asio::buffer(chunk, sizeof(chunk)), onRead);
			io_.restart();
			io_.run();
			return raw_;
		}

		bool timedOut() const
		{
			return timedOut_;
		}

	private:
		void drain(std::vector<ClientResponse>& got)
		{
			size_t pos = 0;
			for (;;)
			{
				ClientResponse res;
				auto span = takeResponse(raw_, pos, res);
				if (span == 0)
				{
					break;
				}
				pos += span;
				got.push_back(std::move(res));
			}
			if (pos > 0)
			{
				raw_.erase(0, pos);
			}
		}

		boost::asio::io_context io_;
		tcp::socket socket_;
		std::string raw_; // 收到但还没切出去的字节
		bool timedOut_ = false;
	};

	/// 给临时文件造个唯一后缀
	/// ctest 把每个用例当独立进程并行跑，文件名写死的话，隔壁用例的 TearDown
	/// 会把这边正在服务的文件删掉——/file 打不开就整条连接断，只剩第一个响应
	std::string uniqueTempSuffix()
	{
		std::mt19937_64 rng(std::random_device {}());
		std::uniform_int_distribution<std::uint64_t> dist;
		std::ostringstream oss;
		oss << std::hex << dist(rng);
		return oss.str();
	}

	class HttpFramingTest : public ::testing::Test
	{
	protected:
		void SetUp() override
		{
			filePath_ = std::filesystem::temp_directory_path()
						/ ("hical_test_framing_file_" + uniqueTempSuffix() + ".bin");
			fileContent_ = "file-body-0123456789-abcdefghijklmnopqrstuvwxyz";
			{
				std::ofstream ofs(filePath_, std::ios::binary | std::ios::trunc);
				ofs.write(fileContent_.data(), static_cast<std::streamsize>(fileContent_.size()));
			}

			server_ = std::make_unique<HttpServer>(0);
			// 写次数统计得在 start() 之前打开：连接处理协程一起来就取一次计数器指针，
			// 之后这个连接还打不开。开关只由派生 fixture 的构造函数置上，
			// 默认关着才是生产环境的真实样子
			if (writeCountEnabled_)
			{
				server_->enableResponseWriteCount();
			}
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
			server_->router().get("/chunked",
								  [](const HttpRequest&) -> HttpResponse
								  {
									  auto res = HttpResponse::chunked();
									  auto& body = res.chunkedBody();
									  body.write("hello");
									  body.end();
									  return res;
								  });
			server_->router().get("/file",
								  [this](const HttpRequest&) -> HttpResponse
								  {
									  HttpResponse res;
									  res.setFileBody(filePath_,
													  0,
													  static_cast<int64_t>(fileContent_.size()),
													  "application/octet-stream");
									  return res;
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
			std::error_code ec;
			std::filesystem::remove(filePath_, ec);
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
		std::filesystem::path filePath_;
		std::string fileContent_;

		// 由派生 fixture 的构造函数打开，SetUp 起来 server 之前读它
		bool writeCountEnabled_ {false};
	};

	/// 要断言「回了几趟」的用例走这个 fixture。开关必须在 start() 之前打开，
	/// 而用例体是 SetUp 跑完之后才执行的，所以只能靠构造函数先把它置上。
	class HttpFramingWriteCountTest : public HttpFramingTest
	{
	public:
		HttpFramingWriteCountTest()
		{
			writeCountEnabled_ = true;
		}
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

	// ============ pipelined 响应合批 ============
	// 一批请求要能全部按序回对（上面那些用例已经盯着），这里额外盯「回了几趟」：
	// 16 个请求攒成一条写，服务端只该走 1~2 次 socket 写。

	TEST_F(HttpFramingWriteCountTest, PipelinedBatchSharesOneWriteAndKeepsOrder)
	{
		// 每条 31 字节，16 条 496 字节，服务端 256B 投机读最多切两刀，所以最多 2 趟写；
		// 一个请求一趟写的老实现是 16 趟
		static constexpr std::string_view kOne = "GET /next HTTP/1.1\r\nHost: a\r\n\r\n";
		std::string batch;
		for (int i = 0; i < 16; ++i)
		{
			batch.append(kOne);
		}

		PipelineClient client(port());
		client.send(batch);
		auto responses = client.read(16);

		ASSERT_EQ(responses.size(), 16u);
		EXPECT_FALSE(client.timedOut());
		for (const auto& r : responses)
		{
			EXPECT_EQ(r.status, 200u);
			EXPECT_EQ(r.body, "next");
		}
		EXPECT_EQ(nextCalls_.load(std::memory_order_relaxed), 16u);
		EXPECT_LE(server_->responseWriteCount(), 2u);
	}

	TEST_F(HttpFramingWriteCountTest, SingleRequestIsWrittenWithoutWaitingForMore)
	{
		PipelineClient client(port());

		// 串行请求（不是流水线）：每条响应都得立刻发出去，不能为了攒批拖到下一次读。
		// 写次数严格一请求一趟——合批不能给单请求路径多加任何一趟
		for (size_t i = 0; i < 2; ++i)
		{
			client.send("GET /next HTTP/1.1\r\nHost: a\r\n\r\n");
			auto responses = client.read(1);
			ASSERT_EQ(responses.size(), 1u);
			EXPECT_FALSE(client.timedOut());
			EXPECT_EQ(responses[0].status, 200u);
			EXPECT_EQ(responses[0].body, "next");
			EXPECT_EQ(server_->responseWriteCount(), i + 1);
		}

		client.send("GET /next HTTP/1.1\r\nHost: a\r\nConnection: close\r\n\r\n");
		auto last = client.read(1);
		ASSERT_EQ(last.size(), 1u);
		EXPECT_EQ(last[0].body, "next");
		EXPECT_EQ(server_->responseWriteCount(), 3u);
	}

	TEST_F(HttpFramingWriteCountTest, PipelineKeepsOrderAcrossErrorResponse)
	{
		std::string batch;
		batch += "GET /next HTTP/1.1\r\nHost: a\r\n\r\n";
		batch += "GET /next HTTP/1.1\r\nHost: a\r\n\r\n";
		batch += "GET /missing HTTP/1.1\r\nHost: a\r\n\r\n";
		batch += "GET /next HTTP/1.1\r\nHost: a\r\n\r\n";
		batch += "GET /next HTTP/1.1\r\nHost: a\r\n\r\n";

		PipelineClient client(port());
		client.send(batch);
		auto responses = client.read(5);

		ASSERT_EQ(responses.size(), 5u);
		EXPECT_FALSE(client.timedOut());
		EXPECT_EQ(responses[0].status, 200u);
		EXPECT_EQ(responses[1].status, 200u);
		EXPECT_EQ(responses[2].status, 404u);
		EXPECT_EQ(responses[3].status, 200u);
		EXPECT_EQ(responses[4].status, 200u);
		EXPECT_EQ(responses[4].body, "next");
		EXPECT_LE(server_->responseWriteCount(), 2u);
	}

	TEST_F(HttpFramingTest, PipelineFlushesBeforeWaitingForRequestBody)
	{
		// 第二个请求的 body 分开发：服务端如果等 body 之前不把第一个响应放出去，
		// 客户端就一直等第一个响应才发 body——两边死等，到超时为止。
		PipelineClient client(port());
		client.send(std::string("GET /next HTTP/1.1\r\nHost: a\r\n\r\n")
					+ "POST /echo HTTP/1.1\r\nHost: a\r\nContent-Length: 5\r\n\r\n");

		auto first = client.read(1);
		ASSERT_EQ(first.size(), 1u);
		EXPECT_FALSE(client.timedOut());
		EXPECT_EQ(first[0].body, "next");

		client.send(std::string("hello") + "GET /next HTTP/1.1\r\nHost: a\r\n\r\n");
		auto rest = client.read(2);
		ASSERT_EQ(rest.size(), 2u);
		EXPECT_EQ(rest[0].body, "hello");
		EXPECT_EQ(rest[1].body, "next");
	}

	TEST_F(HttpFramingWriteCountTest, PipelineWithConnectionCloseKeepsLastResponse)
	{
		PipelineClient client(port());
		client.send("GET /next HTTP/1.1\r\nHost: a\r\n\r\n"
					"GET /next HTTP/1.1\r\nHost: a\r\n\r\n"
					"GET /next HTTP/1.1\r\nHost: a\r\nConnection: close\r\n\r\n");

		auto responses = client.read(3);
		ASSERT_EQ(responses.size(), 3u);
		EXPECT_FALSE(client.timedOut());
		for (const auto& r : responses)
		{
			EXPECT_EQ(r.status, 200u);
			EXPECT_EQ(r.body, "next");
		}
		// 最后一个响应发完服务端主动关连接：再读一次得是 EOF，不是超时
		client.read(4);
		EXPECT_FALSE(client.timedOut());
		EXPECT_LE(server_->responseWriteCount(), 2u);
	}

	// 开关没开时，生产路径上不该有人去记这笔账——计数必须一直是 0
	TEST_F(HttpFramingTest, WriteCountStaysZeroWhenNotEnabled)
	{
		PipelineClient client(port());
		std::string batch;
		for (int i = 0; i < 4; ++i)
		{
			batch.append("GET /next HTTP/1.1\r\nHost: a\r\n\r\n");
		}
		client.send(batch);

		auto responses = client.read(4);
		ASSERT_EQ(responses.size(), 4u);
		EXPECT_FALSE(client.timedOut());
		EXPECT_EQ(nextCalls_.load(std::memory_order_relaxed), 4u);
		EXPECT_EQ(server_->responseWriteCount(), 0u);
	}

	TEST_F(HttpFramingTest, ChunkedResponseInPipelineKeepsOrder)
	{
		PipelineClient client(port());
		client.send("GET /next HTTP/1.1\r\nHost: a\r\n\r\n"
					"GET /chunked HTTP/1.1\r\nHost: a\r\n\r\n"
					"GET /next HTTP/1.1\r\nHost: a\r\nConnection: close\r\n\r\n");

		auto raw = client.readToEnd();
		EXPECT_FALSE(client.timedOut());
		EXPECT_EQ(countStatus(raw, "HTTP/1.1 200"), 3u);

		auto firstBody = raw.find("\r\n\r\n");
		ASSERT_NE(firstBody, std::string::npos);
		EXPECT_EQ(raw.substr(firstBody + 4, 4), "next");

		auto chunkFrame = raw.find("5\r\nhello\r\n0\r\n\r\n");
		EXPECT_NE(chunkFrame, std::string::npos);
		auto lastStatus = raw.rfind("HTTP/1.1 200");
		EXPECT_GT(lastStatus, chunkFrame);
	}

	TEST_F(HttpFramingTest, FileResponseInPipelineKeepsOrder)
	{
		PipelineClient client(port());
		client.send("GET /next HTTP/1.1\r\nHost: a\r\n\r\n"
					"GET /file HTTP/1.1\r\nHost: a\r\n\r\n"
					"GET /next HTTP/1.1\r\nHost: a\r\nConnection: close\r\n\r\n");

		auto raw = client.readToEnd();
		EXPECT_FALSE(client.timedOut());
		EXPECT_EQ(countStatus(raw, "HTTP/1.1 200"), 3u);

		auto firstBody = raw.find("\r\n\r\n");
		ASSERT_NE(firstBody, std::string::npos);
		EXPECT_EQ(raw.substr(firstBody + 4, 4), "next");

		auto fileBytes = raw.find(fileContent_);
		EXPECT_NE(fileBytes, std::string::npos);
		auto lastStatus = raw.rfind("HTTP/1.1 200");
		// 最后一个响应紧跟着文件字节后面开始，等于也算对
		EXPECT_GE(lastStatus, fileBytes + fileContent_.size());
	}

} // namespace
