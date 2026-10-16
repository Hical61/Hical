/**
 * @file test_async_http_client.cpp
 * @brief 协程 HTTP 客户端：接口骨架 + 真实往返（建连 / 请求 / 解析响应头）
 * 接口签名在编译期钉死（本文件顶部 static_assert）；网络用例对端起 Hical
 * 自己的 HttpServer，跑在随机端口上。读 body / 超时 / TLS 的用例在后面切片补。
 * 注意别和 test_http_client.cpp 混了——那个测的是测试用的 raw socket 客户端。
 */

#include "asio/HttpClient.h"
#include "asio/HttpClientPool.h"
#include "core/Coroutine.h"
#include "core/HttpServer.h"
#include "core/SslContext.h"
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/write.hpp>
#include <gtest/gtest.h>
#include <openssl/ssl.h>
#include <openssl/tls1.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <vector>

using namespace hical;
using boost::asio::io_context;

namespace
{

	// 接口形状在编译期钉死：签名被改坏这里直接编不过
	static_assert(std::is_constructible_v<HttpClient, io_context&, std::string, uint16_t>,
				  "HttpClient 应支持 (io_context&, host, port) 构造");
	static_assert(std::is_constructible_v<HttpClient, io_context&, std::string, uint16_t, HttpClientOptions>,
				  "HttpClient 应支持带 options 的构造");
	static_assert(!std::is_copy_constructible_v<HttpClient>, "HttpClient 不该可拷贝");
	static_assert(!std::is_copy_assignable_v<HttpClient>, "HttpClient 不该可拷贝赋值");
	static_assert(!std::is_move_constructible_v<HttpClient>, "HttpClient 不该可移动");

	static_assert(std::is_same_v<decltype(&HttpClient::request),
								 Awaitable<HttpClientResult> (
									 HttpClient::*)(HttpMethod, std::string_view, std::string_view, const HeaderMap&)>);
	static_assert(std::is_same_v<decltype(&HttpClient::get),
								 Awaitable<HttpClientResult> (HttpClient::*)(std::string_view, const HeaderMap&)>);
	static_assert(std::is_same_v<
				  decltype(&HttpClient::post),
				  Awaitable<HttpClientResult> (HttpClient::*)(std::string_view, std::string_view, std::string_view)>);

} // namespace

// ============ 结果结构默认值 ============

TEST(HttpClientResultTest, DefaultIsEmptyNotOk)
{
	HttpClientResult result;
	EXPECT_TRUE(result.error.ok());
	EXPECT_FALSE(result.isNetworkError());
	EXPECT_FALSE(result.ok()); // status 0 还没算成功
	EXPECT_EQ(result.status, 0);
	EXPECT_TRUE(result.body.empty());
	EXPECT_TRUE(result.headers.empty());
	EXPECT_FALSE(static_cast<bool>(result));
}

TEST(HttpClientResultTest, NetworkErrorIsNotOk)
{
	HttpClientResult result;
	result.error.code = ErrorCode::hTimedOut;
	result.error.message = "request timed out";

	EXPECT_TRUE(result.isNetworkError());
	EXPECT_FALSE(result.ok());
	EXPECT_EQ(result.error.code, ErrorCode::hTimedOut);
}

TEST(HttpClientResultTest, TwoHundredIsOk)
{
	HttpClientResult result;
	result.status = 200;

	EXPECT_FALSE(result.isNetworkError());
	EXPECT_TRUE(result.ok());
	EXPECT_TRUE(static_cast<bool>(result));
}

TEST(HttpClientResultTest, NotFoundIsNotNetworkError)
{
	HttpClientResult result;
	result.status = 404;

	// 404 是 HTTP 层的结果，不是网络层错误
	EXPECT_FALSE(result.isNetworkError());
	EXPECT_FALSE(result.ok());
}

// ============ 客户端接口与默认状态 ============

TEST(AsyncHttpClientTest, ConstructAndDefaults)
{
	io_context io;
	HttpClient client(io, "127.0.0.1", 8080);

	EXPECT_FALSE(client.isAlive()); // 还没连过，默认不存活
	EXPECT_EQ(std::string(client.key()), "127.0.0.1:8080");
}

TEST(AsyncHttpClientTest, KeyUsesHostAndPort)
{
	io_context io;
	HttpClient client(io, "example.com", 443);

	EXPECT_EQ(std::string(client.key()), "example.com:443");
}

// ============ 真实往返：对端用 Hical 自己的 HttpServer ============

namespace
{

	// bind 到端口 0 让内核分一个，记下端口立刻关掉——接下来要一个
	// 「确定没人监听」的端口来验连接被拒。
	uint16_t pickFreePort()
	{
		io_context io;
		boost::asio::ip::tcp::acceptor acceptor(io, boost::asio::ip::tcp::endpoint(boost::asio::ip::tcp::v4(), 0));
		uint16_t port = acceptor.local_endpoint().port();
		acceptor.close();
		return port;
	}

} // namespace

/**
 * @brief 起一个真实服务器的 fixture
 * 服务器在后台线程跑，客户端在测试自己的 io_context 上驱动，两边互不干扰。
 */
class AsyncHttpClientNetworkTest : public ::testing::Test
{
protected:
	uint16_t port_ {0};
	std::unique_ptr<HttpServer> server_;
	std::thread serverThread_;

	void SetUp() override
	{
		server_ = std::make_unique<HttpServer>(0);

		server_->router().get("/hello",
							  [](const HttpRequest&) -> HttpResponse
							  {
								  auto resp = HttpResponse::ok("hello world");
								  resp.setHeader("X-Hical-Test", "hi");
								  return resp;
							  });

		// 固定 body，钉住 Content-Length 路径把 body 读全
		server_->router().get("/json",
							  [](const HttpRequest&) -> HttpResponse
							  {
								  return HttpResponse::ok(R"({"msg":"hical","n":42})", "application/json");
							  });

		// 分三块写，逼客户端走 chunked 解码后拼装
		server_->router().get("/chunked",
							  [](const HttpRequest&) -> HttpResponse
							  {
								  auto resp = HttpResponse::chunked();
								  auto& body = resp.chunkedBody();
								  body.write("Hello, ");
								  body.write("chunked ");
								  body.write("world!");
								  body.end();
								  return resp;
							  });

		// 32KB，超过一次 read 能拿到的量，钉住分片读取
		server_->router().get("/big",
							  [](const HttpRequest&) -> HttpResponse
							  {
								  return HttpResponse::ok(std::string(32 * 1024, 'A'), "text/plain");
							  });

		// 回显服务端看到的客户端地址：两次请求地址一致就说明复用了同一条 TCP 连接
		server_->router().get("/peer",
							  [](const HttpRequest& req) -> HttpResponse
							  {
								  return HttpResponse::ok(req.peerAddr().toIpPort(), "text/plain");
							  });

		// 把客户端发来的 body 原样回显：对得上就说明 POST 的 body 通道整条是通的
		server_->router().post("/echo",
							   [](const HttpRequest& req) -> HttpResponse
							   {
								   return HttpResponse::ok(req.body(), "text/plain");
							   });

		// 回显 Content-Type：验 post 默认补的就是 application/json
		server_->router().post("/echo-ct",
							   [](const HttpRequest& req) -> HttpResponse
							   {
								   return HttpResponse::ok(std::string(req.contentType()), "text/plain");
							   });

		// 回显自定义请求头：验客户端设的 X-Test-Token 真的到了服务端
		server_->router().get("/echo-header",
							  [](const HttpRequest& req) -> HttpResponse
							  {
								  return HttpResponse::ok(std::string(req.header("X-Test-Token")), "text/plain");
							  });

		// 500：5xx 跟 404 一样是 HTTP 层结果，不是网络错误
		server_->router().get("/boom",
							  [](const HttpRequest&) -> HttpResponse
							  {
								  return HttpResponse::serverError();
							  });

		// 回显路径和两个查询参数：验带查询串的 target 被正确拆开
		server_->router().get("/echo-query",
							  [](const HttpRequest& req) -> HttpResponse
							  {
								  std::string body(req.path());
								  body += '|';
								  body += req.queryParam("x").value_or("-");
								  body += '|';
								  body += req.queryParam("y").value_or("-");
								  return HttpResponse::ok(body, "text/plain");
							  });

		// 204：按规范就没有 body，客户端别去等 EOF 把连接吊死
		server_->router().get("/nocontent",
							  [](const HttpRequest&) -> HttpResponse
							  {
								  HttpResponse resp;
								  resp.setStatus(HttpStatusCode::hNoContent);
								  return resp;
							  });
	}

	/// 后台起服务器，轮询到能连上再返回
	void startServer()
	{
		serverThread_ = std::thread(
			[this]()
			{
				server_->start();
			});

		for (int i = 0; i < 100; ++i)
		{
			port_ = server_->port();
			if (port_ == 0)
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(10));
				continue;
			}
			try
			{
				io_context io;
				boost::asio::ip::tcp::socket probe(io);
				probe.connect(boost::asio::ip::tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), port_));
				probe.close();
				return;
			}
			catch (...)
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(10));
			}
		}
		FAIL() << "服务器未能在超时内启动";
	}

	void TearDown() override
	{
		if (server_)
		{
			server_->stop();
		}
		if (serverThread_.joinable())
		{
			serverThread_.join();
		}
	}

	/// 一次 GET 的结果，外加请求跑完后连接是否仍然存活
	struct Run
	{
		HttpClientResult result;
		bool alive {false};
	};

	Run runGet(const std::string& target)
	{
		io_context io;
		HttpClient client(io, "127.0.0.1", port_);

		Run run;
		coSpawn(io,
				[&]() -> Awaitable<void>
				{
					run.result = co_await client.get(target);
				});
		io.run();

		run.alive = client.isAlive();
		return run;
	}

	/// 在同一个客户端上依次发多个 GET，每个响应后记录连接是否存活
	struct MultiRun
	{
		std::vector<HttpClientResult> results;
		std::vector<bool> aliveAfter;
	};

	MultiRun runGets(const std::vector<std::string>& targets)
	{
		io_context io;
		HttpClient client(io, "127.0.0.1", port_);

		MultiRun run;
		run.results.resize(targets.size());
		run.aliveAfter.resize(targets.size());

		coSpawn(io,
				[&]() -> Awaitable<void>
				{
					for (std::size_t i = 0; i < targets.size(); ++i)
					{
						run.results[i] = co_await client.get(targets[i]);
						run.aliveAfter[i] = client.isAlive();
					}
				});
		io.run();
		return run;
	}
};

TEST_F(AsyncHttpClientNetworkTest, GetExistingPath_ReturnsOkStatusTextAndHeader)
{
	startServer();
	auto run = runGet("/hello");

	EXPECT_TRUE(run.result.error.ok());
	EXPECT_EQ(run.result.status, 200);
	EXPECT_EQ(run.result.statusText, "OK");
	EXPECT_TRUE(run.result.ok());
	EXPECT_TRUE(run.alive);

	// 自定义响应头能被解析出来
	EXPECT_EQ(std::string(run.result.headers.find("X-Hical-Test")), "hi");
}

TEST_F(AsyncHttpClientNetworkTest, GetMissingPath_NullStatusCodeIsHttpNotNetworkError)
{
	startServer();
	auto run = runGet("/this-path-does-not-exist");

	// 404 是 HTTP 层的结果，网络层没出错——这条最容易写反
	EXPECT_TRUE(run.result.error.ok());
	EXPECT_FALSE(run.result.isNetworkError());
	EXPECT_EQ(run.result.status, 404);
	EXPECT_EQ(run.result.statusText, "Not Found");
	EXPECT_FALSE(run.result.ok());
}

TEST_F(AsyncHttpClientNetworkTest, ConnectRefused_SetsNetworkErrorAndZeroStatus)
{
	io_context io;
	HttpClient client(io, "127.0.0.1", pickFreePort());

	std::optional<HttpClientResult> got;
	coSpawn(io,
			[&]() -> Awaitable<void>
			{
				got = co_await client.get("/");
			});
	io.run();

	ASSERT_TRUE(got.has_value());
	EXPECT_TRUE(got->isNetworkError());
	EXPECT_FALSE(got->error.ok());
	EXPECT_EQ(got->status, 0);
	EXPECT_FALSE(client.isAlive()); // 出错必须把连接置成不可用，别留给池子复用
}

// ============ POST：完整往返 ============
// 前面全是 GET，这里把 POST 的 body 通道也钉住：客户端发什么、服务端就收到什么。

TEST_F(AsyncHttpClientNetworkTest, HttpPost_RoundTrip_EchoesJsonBody)
{
	startServer();

	io_context io;
	HttpClient client(io, "127.0.0.1", port_);
	const std::string payload = R"({"name":"hical","n":42})";
	HttpClientResult result;
	coSpawn(io,
			[&]() -> Awaitable<void>
			{
				result = co_await client.post("/echo", payload);
			});
	io.run();

	EXPECT_TRUE(result.error.ok());
	EXPECT_EQ(result.status, 200);
	// 服务端回显的 body 跟客户端发的逐字节一致，POST 的 body 通道就算通了
	EXPECT_EQ(result.body, payload);
	EXPECT_TRUE(client.isAlive()); // body 读干净，连接还能复用
}

TEST_F(AsyncHttpClientNetworkTest, HttpPost_DefaultContentTypeIsJson)
{
	startServer();

	io_context io;
	HttpClient client(io, "127.0.0.1", port_);
	HttpClientResult result;
	coSpawn(io,
			[&]() -> Awaitable<void>
			{
				// post 不传 contentType 时应默认 application/json
				result = co_await client.post("/echo-ct", R"({"a":1})");
			});
	io.run();

	ASSERT_TRUE(result.error.ok()) << result.error.message;
	EXPECT_EQ(result.status, 200);
	EXPECT_EQ(result.body, "application/json");
}

TEST_F(AsyncHttpClientNetworkTest, HttpPost_EmptyBody_StillRoundTrips)
{
	startServer();

	// 给一档小超时兜底：万一空 body 的 POST 被服务端当成「还有 body 要读」，
	// 这里会快速超时失败，而不是干等默认的 30s。
	HttpClientOptions options;
	options.connectTimeout = std::chrono::seconds(2);
	options.requestTimeout = std::chrono::seconds(2);

	io_context io;
	HttpClient client(io, "127.0.0.1", port_, options);
	HttpClientResult result;
	coSpawn(io,
			[&]() -> Awaitable<void>
			{
				result = co_await client.post("/echo", "");
			});
	io.run();

	EXPECT_TRUE(result.error.ok()) << result.error.message;
	EXPECT_EQ(result.status, 200);
	EXPECT_TRUE(result.body.empty());
}

// ============ 自定义请求头：客户端设的能到服务端 ============

TEST_F(AsyncHttpClientNetworkTest, GetWithCustomHeader_ServerReceivesIt)
{
	startServer();

	io_context io;
	HttpClient client(io, "127.0.0.1", port_);
	HeaderMap headers;
	headers.set("X-Test-Token", "abc123");
	HttpClientResult result;
	coSpawn(io,
			[&]() -> Awaitable<void>
			{
				result = co_await client.get("/echo-header", headers);
			});
	io.run();

	EXPECT_TRUE(result.error.ok());
	EXPECT_EQ(result.status, 200);
	// 服务端读出来的就是客户端设的值
	EXPECT_EQ(result.body, "abc123");
}

// ============ 状态码：5xx 也不是网络错误 ============

TEST_F(AsyncHttpClientNetworkTest, GetServerError_FiveHundredIsNotNetworkError)
{
	startServer();
	auto run = runGet("/boom");

	// 500 跟 404 一个道理：HTTP 层拿到了响应，网络层没出错
	EXPECT_TRUE(run.result.error.ok());
	EXPECT_FALSE(run.result.isNetworkError());
	EXPECT_EQ(run.result.status, 500);
	EXPECT_EQ(run.result.statusText, "Internal Server Error");
	EXPECT_FALSE(run.result.ok());
}

// ============ 边界：带查询串的 target ============

TEST_F(AsyncHttpClientNetworkTest, GetWithQueryString_ServerParsesPathAndParams)
{
	startServer();
	auto run = runGet("/echo-query?x=1&y=2");

	ASSERT_TRUE(run.result.error.ok()) << run.result.error.message;
	EXPECT_EQ(run.result.status, 200);
	// path 不含查询串，两个参数各自解析出来
	EXPECT_EQ(run.result.body, "/echo-query|1|2");
}

// ============ 边界：204 空响应体 ============

TEST_F(AsyncHttpClientNetworkTest, GetNoContent_EmptyBodyKeepsConnectionAlive)
{
	startServer();
	auto run = runGet("/nocontent");

	ASSERT_TRUE(run.result.error.ok()) << run.result.error.message;
	EXPECT_EQ(run.result.status, 204);
	EXPECT_TRUE(run.result.body.empty());
	// 204 没有 body，客户端不该去等 EOF；连接照旧可复用
	EXPECT_TRUE(run.alive);
}

// ============ body 读取：三种形式 ============

TEST_F(AsyncHttpClientNetworkTest, ContentLengthBody_ReadsWholeBody)
{
	startServer();
	auto run = runGet("/json");

	EXPECT_TRUE(run.result.error.ok());
	EXPECT_EQ(run.result.status, 200);
	EXPECT_EQ(run.result.body, R"({"msg":"hical","n":42})");
	EXPECT_TRUE(run.alive); // body 读干净了，连接还能复用
}

TEST_F(AsyncHttpClientNetworkTest, ChunkedBody_AssemblesChunks)
{
	startServer();
	auto run = runGet("/chunked");

	EXPECT_TRUE(run.result.error.ok());
	EXPECT_EQ(run.result.status, 200);
	EXPECT_EQ(run.result.body, "Hello, chunked world!");
	EXPECT_TRUE(run.alive); // chunked 收尾后连接仍可复用
}

TEST_F(AsyncHttpClientNetworkTest, LargeBody_ReadsAcrossMultipleReads)
{
	startServer();
	auto run = runGet("/big");

	EXPECT_TRUE(run.result.error.ok());
	ASSERT_EQ(run.result.body.size(), 32u * 1024u);
	EXPECT_TRUE(std::all_of(run.result.body.begin(),
							run.result.body.end(),
							[](char c)
							{
								return c == 'A';
							}));
}

// ============ keep-alive：第二次请求复用同一条连接 ============

TEST_F(AsyncHttpClientNetworkTest, SecondRequest_ReusesSameConnection)
{
	startServer();
	auto run = runGets({"/peer", "/peer"});

	ASSERT_EQ(run.results.size(), 2u);
	EXPECT_TRUE(run.results[0].error.ok());
	EXPECT_TRUE(run.results[1].error.ok());

	// /peer 回显服务端看到的客户端地址。两次一致 = 复用同一条 TCP 连接；
	// 中间要是重连了，客户端临时端口会变，body 就对不上了。
	EXPECT_FALSE(run.results[0].body.empty());
	EXPECT_EQ(run.results[0].body, run.results[1].body);

	EXPECT_TRUE(run.aliveAfter[0]);
	EXPECT_TRUE(run.aliveAfter[1]);
}

// ============ 无长度信息 / Connection: close：靠 EOF 给 body 收尾 ============

namespace
{

	/**
	 * @brief 极简 raw 服务端：接一条连接、读一个请求、回一段固定字节、主动关连接
	 * HttpServer 的响应总带 Content-Length 或 chunked，造不出「无长度信息、靠对端关连接
	 * 收尾」的响应。这个一次性服务端专为那条 EOF 路径准备。测试专属，不追求健壮性。
	 */
	class RawOneShotServer
	{
	public:
		RawOneShotServer()
			: acceptor_(io_, boost::asio::ip::tcp::endpoint(boost::asio::ip::tcp::v4(), 0))
			, port_(acceptor_.local_endpoint().port())
		{
		}

		[[nodiscard]] uint16_t port() const noexcept
		{
			return port_;
		}

		/// 后台线程等一条连接：读完请求头就回 response，然后关掉写端
		void serveOnce(std::string response)
		{
			thread_ = std::thread(
				[this, response = std::move(response)]()
				{
					boost::system::error_code ec;
					boost::asio::ip::tcp::socket sock(io_);
					acceptor_.accept(sock, ec);
					if (ec)
					{
						return;
					}

					std::string req;
					char buf[4096];
					for (;;)
					{
						auto n = sock.read_some(boost::asio::buffer(buf), ec);
						if (ec)
						{
							return;
						}
						req.append(buf, n);
						if (req.find("\r\n\r\n") != std::string::npos)
						{
							break;
						}
					}

					boost::asio::write(sock, boost::asio::buffer(response), ec);
					sock.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
					sock.close(ec);
				});
		}

		/// 后台线程等一条连接：只接受、不回任何东西，把连接吊在那里直到析构。
		/// 专门用来逼出请求超时——对端接了连接却永远不吭声。
		void serveAcceptOnly()
		{
			thread_ = std::thread(
				[this]()
				{
					boost::system::error_code ec;
					boost::asio::ip::tcp::socket sock(io_);
					acceptor_.accept(sock, ec);
					if (ec)
					{
						return;
					}

					// 一条响应都不发，连接也不关，等析构时收工
					std::unique_lock<std::mutex> lk(mutex_);
					cv_.wait(lk,
							 [this]()
							 {
								 return stop_.load();
							 });
					sock.close(ec);
				});
		}

		~RawOneShotServer()
		{
			{
				std::lock_guard<std::mutex> lk(mutex_);
				stop_ = true;
			}
			cv_.notify_all();
			if (thread_.joinable())
			{
				thread_.join();
			}
		}

	private:
		boost::asio::io_context io_;
		boost::asio::ip::tcp::acceptor acceptor_;
		uint16_t port_;
		std::thread thread_;
		std::atomic<bool> stop_ {false};
		std::mutex mutex_;
		std::condition_variable cv_;
	};

} // namespace

namespace
{

	/**
	 * @brief 后台线程跑一个 HttpServer，析构时停掉并 join
	 * 失败路径也不再漏清理，省得 joinable 线程析构时直接 terminate。
	 */
	class BackgroundServer
	{
	public:
		explicit BackgroundServer(std::unique_ptr<HttpServer> server) : server_(std::move(server))
		{
		}

		/// 起服务并轮询到能连上，返回端口；超时返回 0
		uint16_t startAndWait()
		{
			thread_ = std::thread(
				[this]()
				{
					server_->start();
				});

			for (int i = 0; i < 200; ++i)
			{
				uint16_t p = server_->port();
				if (p != 0)
				{
					try
					{
						io_context io;
						boost::asio::ip::tcp::socket probe(io);
						probe.connect(boost::asio::ip::tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), p));
						probe.close();
						return p;
					}
					catch (...)
					{
					}
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(10));
			}
			return 0;
		}

		HttpServer& server()
		{
			return *server_;
		}

		~BackgroundServer()
		{
			if (server_)
			{
				server_->stop();
			}
			if (thread_.joinable())
			{
				thread_.join();
			}
		}

	private:
		std::unique_ptr<HttpServer> server_;
		std::thread thread_;
	};

	/**
	 * @brief 「响应完就关连接」的 raw 服务端，能连接受多条连接
	 * 读一个请求、回一段不带 Connection: close 的 keep-alive 响应、立刻关掉，然后继续接。
	 * 客户端收到这种响应会以为连接还活着（isAlive 为真），拿它复用就会撞墙——专门用来
	 * 验证池子能不能丢掉这种「看着活、其实死了」的连接、重开一条。
	 */
	class RawAlwaysCloseServer
	{
	public:
		RawAlwaysCloseServer()
			: acceptor_(io_, boost::asio::ip::tcp::endpoint(boost::asio::ip::tcp::v4(), 0))
			, port_(acceptor_.local_endpoint().port())
		{
		}

		[[nodiscard]] uint16_t port() const noexcept
		{
			return port_;
		}

		[[nodiscard]] int acceptCount() const noexcept
		{
			return accepts_.load();
		}

		void start()
		{
			acceptNext();
			thread_ = std::thread(
				[this]()
				{
					io_.run();
				});
		}

		~RawAlwaysCloseServer()
		{
			boost::asio::post(io_,
							  [this]()
							  {
								  boost::system::error_code ec;
								  acceptor_.close(ec);
							  });
			if (thread_.joinable())
			{
				thread_.join();
			}
		}

	private:
		void acceptNext()
		{
			acceptor_.async_accept(
				[this](boost::system::error_code ec, boost::asio::ip::tcp::socket sock)
				{
					if (ec)
					{
						return;
					}
					accepts_.fetch_add(1);

					std::string req;
					char buf[4096];
					boost::system::error_code rEc;
					for (;;)
					{
						auto n = sock.read_some(boost::asio::buffer(buf), rEc);
						if (rEc)
						{
							break;
						}
						req.append(buf, n);
						if (req.find("\r\n\r\n") != std::string::npos)
						{
							break;
						}
					}

					static constexpr std::string_view kResp = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok";
					boost::asio::write(sock, boost::asio::buffer(kResp), rEc);

					boost::system::error_code cEc;
					sock.shutdown(boost::asio::ip::tcp::socket::shutdown_both, cEc);
					sock.close(cEc);

					acceptNext();
				});
		}

		boost::asio::io_context io_;
		boost::asio::ip::tcp::acceptor acceptor_;
		uint16_t port_;
		std::atomic<int> accepts_ {0};
		std::thread thread_;
	};

} // namespace

TEST(AsyncHttpClientRawTest, ConnectionCloseWithoutLength_ReadsBodyUntilEof)
{
	RawOneShotServer server;
	server.serveOnce("HTTP/1.1 200 OK\r\n"
					 "Content-Type: text/plain\r\n"
					 "Connection: close\r\n"
					 "\r\n"
					 "body till eof");

	io_context io;
	HttpClient client(io, "127.0.0.1", server.port());
	HttpClientResult result;
	coSpawn(io,
			[&]() -> Awaitable<void>
			{
				result = co_await client.get("/");
			});
	io.run();

	EXPECT_TRUE(result.error.ok());
	EXPECT_EQ(result.status, 200);
	EXPECT_EQ(result.body, "body till eof");
	// EOF 收尾说明对端已经关了写端，这条连接不能再拿来复用
	EXPECT_FALSE(client.isAlive());
}

TEST(AsyncHttpClientRawTest, ConnectionCloseWithLength_ReadsBodyThenStopsReuse)
{
	RawOneShotServer server;
	server.serveOnce("HTTP/1.1 200 OK\r\n"
					 "Content-Length: 5\r\n"
					 "Connection: close\r\n"
					 "\r\n"
					 "hello");

	io_context io;
	HttpClient client(io, "127.0.0.1", server.port());
	HttpClientResult result;
	coSpawn(io,
			[&]() -> Awaitable<void>
			{
				result = co_await client.get("/");
			});
	io.run();

	EXPECT_TRUE(result.error.ok());
	EXPECT_EQ(result.status, 200);
	EXPECT_EQ(result.body, "hello");
	// 有 Content-Length 能读全 body，但 Connection: close 决定了它不可复用
	EXPECT_FALSE(client.isAlive());
}

// ============ 超时：connectTimeout / requestTimeout ============

TEST(HttpClientOptionsTest, DefaultTimeoutsAreNonZero)
{
	// 默认不能是 0：0 等于永不超时，那正是这批改动要修的东西
	HttpClientOptions options;
	EXPECT_GT(options.connectTimeout.count(), 0);
	EXPECT_GT(options.requestTimeout.count(), 0);
}

TEST_F(AsyncHttpClientNetworkTest, NormalRequest_WithSmallTimeouts_StillSucceeds)
{
	// 超时机制不能误伤正常请求：给它压到很小也照样跑通，且连接照常可复用
	startServer();

	HttpClientOptions options;
	options.connectTimeout = std::chrono::milliseconds(500);
	options.requestTimeout = std::chrono::milliseconds(500);

	io_context io;
	HttpClient client(io, "127.0.0.1", port_, options);
	HttpClientResult result;
	auto begin = std::chrono::steady_clock::now();
	coSpawn(io,
			[&]() -> Awaitable<void>
			{
				result = co_await client.get("/hello");
			});
	io.run();
	auto elapsed = std::chrono::steady_clock::now() - begin;

	EXPECT_TRUE(result.error.ok());
	EXPECT_EQ(result.status, 200);
	EXPECT_TRUE(client.isAlive());
	// 本地回环请求是毫秒级，绝不能贴着 requestTimeout 才回来
	EXPECT_LT(elapsed, options.requestTimeout);
}

TEST(AsyncHttpClientTimeoutTest, RequestTimeout_ServerAcceptsButNeverResponds)
{
	// 服务端接受连接、收下请求，但死活不回响应——正是请求超时要救的场面
	RawOneShotServer server;
	server.serveAcceptOnly();

	HttpClientOptions options;
	options.requestTimeout = std::chrono::milliseconds(500);

	io_context io;
	HttpClient client(io, "127.0.0.1", server.port(), options);
	HttpClientResult result;
	auto begin = std::chrono::steady_clock::now();
	coSpawn(io,
			[&]() -> Awaitable<void>
			{
				result = co_await client.get("/");
			});
	io.run();
	auto elapsed = std::chrono::steady_clock::now() - begin;

	EXPECT_EQ(result.error.code, ErrorCode::hTimedOut);
	EXPECT_EQ(result.status, 0);
	// 超时后连接状态未定义，必须关掉、绝不能回池
	EXPECT_FALSE(client.isAlive());
	// 500ms 的档给足余量，CI 负载高时也别飘
	EXPECT_LT(elapsed, std::chrono::seconds(5));
}

TEST(AsyncHttpClientTimeoutTest, ConnectTimeout_BlackholeAddress)
{
	// 192.0.2.0/24 是 RFC 5737 留作文档用的网段，正常路由到不了：SYN 发出去石沉大海，
	// 正好逼出连接超时那条路。（连不监听的本机端口回的是 refused，不是 timeout，别混。）
	HttpClientOptions options;
	options.connectTimeout = std::chrono::milliseconds(300);
	options.requestTimeout = std::chrono::seconds(5);

	io_context io;
	HttpClient client(io, "192.0.2.1", 81, options);
	HttpClientResult result;
	auto begin = std::chrono::steady_clock::now();
	coSpawn(io,
			[&]() -> Awaitable<void>
			{
				result = co_await client.get("/");
			});
	io.run();
	auto elapsed = std::chrono::steady_clock::now() - begin;

	EXPECT_TRUE(result.isNetworkError());
	EXPECT_EQ(result.status, 0);
	EXPECT_FALSE(client.isAlive());
	EXPECT_LT(elapsed, std::chrono::seconds(5));

	if (result.error.code != ErrorCode::hTimedOut)
	{
		// 有的网络会立刻回不可达（host/network unreachable），那就碰不到超时路径。
		// 这种是环境问题不是代码问题，跳过而不是误报失败。
		GTEST_SKIP() << "环境未产生连接超时，实际错误码: " << static_cast<int>(result.error.code);
	}
}

// ============ 连接池：分桶复用 ============

TEST_F(AsyncHttpClientNetworkTest, HttpPool_ManyRequestsReuseFewConnections)
{
	startServer();

	io_context io;
	HttpClientPool pool(io);

	constexpr int kRequests = 100;
	std::vector<std::string> peers(kRequests);
	bool allOk = true;

	coSpawn(io,
			[&]() -> Awaitable<void>
			{
				for (int i = 0; i < kRequests; ++i)
				{
					auto r = co_await pool.get("127.0.0.1", port_, "/peer");
					if (!r.error.ok())
					{
						allOk = false;
						break;
					}
					peers[i] = r.body;
				}
				pool.shutdown(); // 取消清理 timer，好让 io.run() 收尾返回
			});
	io.run();

	EXPECT_TRUE(allOk);

	// /peer 回显服务端看到的客户端地址。100 次请求复用同一条 keep-alive 连接，
	// 客户端临时端口应该只有一个；留一点余量，允许偶发的一次重连。
	std::set<std::string> unique;
	for (const auto& p : peers)
	{
		if (!p.empty())
		{
			unique.insert(p);
		}
	}
	EXPECT_FALSE(unique.empty());
	EXPECT_LE(unique.size(), 2u);
}

TEST(AsyncHttpClientPoolTest, PeerClosesIdleConnection_PoolRecoversWithNewOne)
{
	RawAlwaysCloseServer server;
	server.start();

	io_context io;
	HttpClientPool pool(io);

	std::vector<HttpClientResult> results(3);
	coSpawn(io,
			[&]() -> Awaitable<void>
			{
				for (std::size_t i = 0; i < results.size(); ++i)
				{
					results[i] = co_await pool.get("127.0.0.1", server.port(), "/");
				}
				pool.shutdown();
			});
	io.run();

	// 每条连接响应完就被对端关掉，池子必须丢掉这个「看着活、其实死了」的连接、重开一条，
	// 三次请求才能次次成功，而不是持续失败
	for (const auto& r : results)
	{
		EXPECT_TRUE(r.error.ok());
		EXPECT_EQ(r.status, 200);
		EXPECT_EQ(r.body, "ok");
	}
	EXPECT_GE(server.acceptCount(), 2);
}

TEST(AsyncHttpClientPoolTest, DifferentTargets_UseSeparateBuckets)
{
	auto serverA = std::make_unique<HttpServer>(0);
	serverA->router().get("/id",
						  [](const HttpRequest&) -> HttpResponse
						  {
							  return HttpResponse::ok("A");
						  });
	auto serverB = std::make_unique<HttpServer>(0);
	serverB->router().get("/id",
						  [](const HttpRequest&) -> HttpResponse
						  {
							  return HttpResponse::ok("B");
						  });

	BackgroundServer bgA(std::move(serverA));
	BackgroundServer bgB(std::move(serverB));
	uint16_t portA = bgA.startAndWait();
	uint16_t portB = bgB.startAndWait();
	ASSERT_NE(portA, 0);
	ASSERT_NE(portB, 0);

	io_context io;
	HttpClientPool pool(io);

	HttpClientResult r1;
	HttpClientResult r2;
	HttpClientResult r3;
	coSpawn(io,
			[&]() -> Awaitable<void>
			{
				r1 = co_await pool.get("127.0.0.1", portA, "/id");
				r2 = co_await pool.get("127.0.0.1", portB, "/id");
				r3 = co_await pool.get("127.0.0.1", portA, "/id");
				pool.shutdown();
			});
	io.run();

	// 两个目标各走各的桶，交替请求也不能串
	EXPECT_EQ(r1.body, "A");
	EXPECT_EQ(r2.body, "B");
	EXPECT_EQ(r3.body, "A");
}

TEST(AsyncHttpClientPoolTest, ConveniencePost_ReturnsEchoedBody)
{
	auto server = std::make_unique<HttpServer>(0);
	server->router().post("/echo",
						  [](const HttpRequest& req) -> HttpResponse
						  {
							  return HttpResponse::ok(req.body(), "text/plain");
						  });
	BackgroundServer bg(std::move(server));
	uint16_t port = bg.startAndWait();
	ASSERT_NE(port, 0);

	io_context io;
	HttpClientPool pool(io);

	HttpClientResult result;
	coSpawn(io,
			[&]() -> Awaitable<void>
			{
				result = co_await pool.post("127.0.0.1", port, "/echo", R"({"n":7})");
				pool.shutdown();
			});
	io.run();

	EXPECT_TRUE(result.error.ok());
	EXPECT_EQ(result.status, 200);
	EXPECT_EQ(result.body, R"({"n":7})");
}

TEST(AsyncHttpClientPoolTest, ConvenienceRequest_RepeatsAndReusesConnection)
{
	// pool.request 是显式指定方法的那条便捷入口，别的用例只走 get/post。
	// 借 /peer 回显客户端地址，连发多次验证它内部自动 acquire / release，连接能被复用。
	auto server = std::make_unique<HttpServer>(0);
	server->router().get("/peer",
						 [](const HttpRequest& req) -> HttpResponse
						 {
							 return HttpResponse::ok(req.peerAddr().toIpPort(), "text/plain");
						 });
	BackgroundServer bg(std::move(server));
	uint16_t port = bg.startAndWait();
	ASSERT_NE(port, 0);

	io_context io;
	HttpClientPool pool(io);

	constexpr int kTimes = 10;
	std::vector<HttpClientResult> results(kTimes);
	coSpawn(io,
			[&]() -> Awaitable<void>
			{
				for (int i = 0; i < kTimes; ++i)
				{
					results[i] = co_await pool.request("127.0.0.1", port, HttpMethod::hGet, "/peer");
				}
				pool.shutdown();
			});
	io.run();

	std::set<std::string> unique;
	for (const auto& r : results)
	{
		EXPECT_TRUE(r.error.ok()) << r.error.message;
		EXPECT_EQ(r.status, 200);
		if (!r.body.empty())
		{
			unique.insert(r.body);
		}
	}
	// 便捷入口用完把连接还回池子，10 次应复用同一条；留点余量允许偶发重连
	EXPECT_FALSE(unique.empty());
	EXPECT_LE(unique.size(), 2u);
}

TEST(AsyncHttpClientPoolTest, ConcurrentRequests_OnOneIoContext_AllSucceed)
{
	auto server = std::make_unique<HttpServer>(0);
	server->router().post("/echo",
						  [](const HttpRequest& req) -> HttpResponse
						  {
							  return HttpResponse::ok(req.body(), "text/plain");
						  });
	BackgroundServer bg(std::move(server));
	uint16_t port = bg.startAndWait();
	ASSERT_NE(port, 0);

	// 池子的契约是「一个 io_context 一个池、不跨线程共享」，所以并发全挤在这一个
	// io_context 上：同时挂起多个请求协程，看池子能不能都伺候好。别开多线程——那
	// 会违反契约，测出来的是假问题。
	io_context io;
	HttpClientPool pool(io);

	constexpr int kConcurrency = 8;
	std::vector<HttpClientResult> results(kConcurrency);
	std::atomic<int> done {0};
	for (int i = 0; i < kConcurrency; ++i)
	{
		coSpawn(io,
				[&, i]() -> Awaitable<void>
				{
					results[i] = co_await pool.post("127.0.0.1", port, "/echo", "req-" + std::to_string(i));
					done.fetch_add(1);
				});
	}

	// 轮询推进事件循环，等所有请求回来；别用一次性 run()（会被清理 timer 吊住）
	auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	while (done.load() < kConcurrency && std::chrono::steady_clock::now() < deadline)
	{
		io.poll();
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
	}

	EXPECT_EQ(done.load(), kConcurrency);
	for (int i = 0; i < kConcurrency; ++i)
	{
		EXPECT_TRUE(results[i].error.ok()) << results[i].error.message;
		EXPECT_EQ(results[i].status, 200);
		EXPECT_EQ(results[i].body, "req-" + std::to_string(i));
	}

	pool.shutdown();
	io.restart();
	io.run();
}

TEST(AsyncHttpClientPoolTest, Destruction_CancelsCleanupTimer)
{
	io_context io;
	std::unique_ptr<HttpClientPool> pool;

	coSpawn(io,
			[&]() -> Awaitable<void>
			{
				// 清理周期故意设长：只有池子析构时主动 cancel，io.run() 才会立刻返回
				HttpClientPoolOptions opt;
				opt.cleanupInterval = std::chrono::seconds(30);
				pool = std::make_unique<HttpClientPool>(io, opt);

				// 一次注定连不上的请求，只为让清理协程抢先把 timer arm 起来
				HttpClientOptions copts;
				copts.connectTimeout = std::chrono::milliseconds(500);
				copts.requestTimeout = std::chrono::milliseconds(500);
				(void)co_await pool->get("127.0.0.1", 1, "/", {}, copts);

				pool.reset();
			});

	auto begin = std::chrono::steady_clock::now();
	io.run();
	auto elapsed = std::chrono::steady_clock::now() - begin;

	// 析构没把 timer cancel 掉的话，run() 得干等满 30s 清理周期，这里就超了
	EXPECT_LT(elapsed, std::chrono::seconds(2));
}

TEST(AsyncHttpClientPoolTest, IdleConnections_SweptAfterTimeout)
{
	auto server = std::make_unique<HttpServer>(0);
	server->router().get("/ping",
						 [](const HttpRequest&) -> HttpResponse
						 {
							 return HttpResponse::ok("pong");
						 });
	BackgroundServer bg(std::move(server));
	uint16_t port = bg.startAndWait();
	ASSERT_NE(port, 0);

	io_context io;
	HttpClientPoolOptions opt;
	opt.idleTimeout = std::chrono::milliseconds(200);
	opt.cleanupInterval = std::chrono::milliseconds(50);
	HttpClientPool pool(io, opt);

	coSpawn(io,
			[&]() -> Awaitable<void>
			{
				(void)co_await pool.get("127.0.0.1", port, "/ping");
			});

	// poll + 短睡推进事件循环，等请求跑完、连接落回空闲队列（别用一次性 run()，
	// 那会被清理 timer 吊住）
	auto waitUntil = [&](const std::function<bool()>& pred)
	{
		auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		while (!pred() && std::chrono::steady_clock::now() < deadline)
		{
			io.poll();
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
		return pred();
	};

	ASSERT_TRUE(waitUntil(
		[&]()
		{
			return pool.idleCount() >= 1;
		}));
	// 空闲超过 idleTimeout，清理协程该把它扫掉
	ASSERT_TRUE(waitUntil(
		[&]()
		{
			return pool.idleCount() == 0;
		}));

	pool.shutdown();
	io.restart();
	io.run();
}

// ============ HTTPS：明文那条路扩成也支持 TLS ============
// 对端起 Hical 自己的 HttpServer 并 enableSsl；证书由 scripts/gen_test_certs.py 现场签，
// 落到构建目录（ctest 的工作目录就是那儿）。证书缺失就 SKIP，CI 上置了
// HICAL_REQUIRE_SSL_CERTS 就改成硬失败——静默跳过等于这条 TLS 路没有回归保护。

namespace
{

	constexpr const char* kTestCertFile = "test_server.crt";
	constexpr const char* kTestKeyFile = "test_server.key";

	/// 自签测试证书是不是就位了
	bool testCertsExist()
	{
		std::ifstream cert(kTestCertFile);
		std::ifstream key(kTestKeyFile);
		return cert.good() && key.good();
	}

	/// 造一个 TLS 客户端配置：tlsContext 非空即 HTTPS，verify 决定校不校验证书
	HttpClientOptions makeTlsOptions(bool verify)
	{
		HttpClientOptions options;
		options.tlsContext = std::make_shared<SslContext>(boost::asio::ssl::context::tls_client);
		options.verifyPeer = verify;
		return options;
	}

	/**
	 * @brief 后台跑一个启用 TLS 的 HttpServer，析构时停掉并 join
	 * 路由由调用方通过 configure 配好；证书缺失时构造会抛，所以调用方得先查证书。
	 */
	class TlsBackgroundServer
	{
	public:
		explicit TlsBackgroundServer(std::function<void(HttpServer&)> configure)
		{
			server_ = std::make_unique<HttpServer>(0);
			server_->enableSsl(kTestCertFile, kTestKeyFile);
			configure(*server_);
		}

		/// 起服务并轮询到能 TCP 连上；超时返回 0
		uint16_t startAndWait()
		{
			thread_ = std::thread(
				[this]()
				{
					server_->start();
				});

			for (int i = 0; i < 200; ++i)
			{
				uint16_t p = server_->port();
				if (p != 0)
				{
					try
					{
						io_context io;
						boost::asio::ip::tcp::socket probe(io);
						probe.connect(boost::asio::ip::tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), p));
						probe.close();
						return p;
					}
					catch (...)
					{
					}
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(10));
			}
			return 0;
		}

		~TlsBackgroundServer()
		{
			if (server_)
			{
				server_->stop();
			}
			if (thread_.joinable())
			{
				thread_.join();
			}
		}

	private:
		std::unique_ptr<HttpServer> server_;
		std::thread thread_;
	};

} // namespace

TEST(AsyncHttpClientTlsTest, HttpsRoundTrip_SelfSignedWithVerifyOffReturnsBody)
{
	if (!testCertsExist())
	{
		if (std::getenv("HICAL_REQUIRE_SSL_CERTS") != nullptr)
		{
			FAIL() << "HICAL_REQUIRE_SSL_CERTS 已置位，但测试证书缺失（" << kTestCertFile << "）";
		}
		GTEST_SKIP() << "跳过：测试证书不存在";
	}

	TlsBackgroundServer server(
		[](HttpServer& s)
		{
			s.router().get("/hello",
						   [](const HttpRequest&) -> HttpResponse
						   {
							   auto resp = HttpResponse::ok("hello over tls");
							   resp.setHeader("X-Hical-Tls", "yes");
							   return resp;
						   });
		});
	uint16_t port = server.startAndWait();
	ASSERT_NE(port, 0);

	io_context io;
	HttpClient client(io, "127.0.0.1", port, makeTlsOptions(false));
	HttpClientResult result;
	coSpawn(io,
			[&]() -> Awaitable<void>
			{
				result = co_await client.get("/hello");
			});
	io.run();

	EXPECT_TRUE(result.error.ok()) << "err=" << static_cast<int>(result.error.code) << " " << result.error.message;
	EXPECT_EQ(result.status, 200);
	EXPECT_EQ(result.body, "hello over tls");
	EXPECT_EQ(std::string(result.headers.find("X-Hical-Tls")), "yes");
	EXPECT_TRUE(client.isAlive()); // body 读干净，TLS 连接也能复用
}

TEST(AsyncHttpClientTlsTest, VerifyPeerAgainstSelfSigned_ReturnsCertificateError)
{
	if (!testCertsExist())
	{
		if (std::getenv("HICAL_REQUIRE_SSL_CERTS") != nullptr)
		{
			FAIL() << "HICAL_REQUIRE_SSL_CERTS 已置位，但测试证书缺失（" << kTestCertFile << "）";
		}
		GTEST_SKIP() << "跳过：测试证书不存在";
	}

	TlsBackgroundServer server(
		[](HttpServer& s)
		{
			s.router().get("/hello",
						   [](const HttpRequest&) -> HttpResponse
						   {
							   return HttpResponse::ok("should not reach");
						   });
		});
	uint16_t port = server.startAndWait();
	ASSERT_NE(port, 0);

	io_context io;
	// verifyPeer 开着（默认）去连自签证书：没有可信签发链，握手必然验不过。
	// 要的是明确的证书类错误，不是崩溃、也不是含糊的 hUnknown。
	HttpClient client(io, "127.0.0.1", port, makeTlsOptions(true));
	HttpClientResult result;
	coSpawn(io,
			[&]() -> Awaitable<void>
			{
				result = co_await client.get("/hello");
			});
	io.run();

	EXPECT_FALSE(result.error.ok());
	EXPECT_EQ(result.status, 0);
	EXPECT_EQ(result.error.code, ErrorCode::hSslInvalidCertificate);
	EXPECT_FALSE(client.isAlive()); // 握手没成，这条连接不能回池
}

TEST(AsyncHttpClientTlsTest, HttpsSecondRequest_ReusesSameConnection)
{
	if (!testCertsExist())
	{
		if (std::getenv("HICAL_REQUIRE_SSL_CERTS") != nullptr)
		{
			FAIL() << "HICAL_REQUIRE_SSL_CERTS 已置位，但测试证书缺失（" << kTestCertFile << "）";
		}
		GTEST_SKIP() << "跳过：测试证书不存在";
	}

	TlsBackgroundServer server(
		[](HttpServer& s)
		{
			// /peer 回显服务端看到的客户端地址：两次一致 = 复用同一条 TLS 连接
			s.router().get("/peer",
						   [](const HttpRequest& req) -> HttpResponse
						   {
							   return HttpResponse::ok(req.peerAddr().toIpPort(), "text/plain");
						   });
		});
	uint16_t port = server.startAndWait();
	ASSERT_NE(port, 0);

	io_context io;
	HttpClient client(io, "127.0.0.1", port, makeTlsOptions(false));

	std::vector<HttpClientResult> results(2);
	std::vector<bool> aliveAfter(2);
	coSpawn(io,
			[&]() -> Awaitable<void>
			{
				for (std::size_t i = 0; i < results.size(); ++i)
				{
					results[i] = co_await client.get("/peer");
					aliveAfter[i] = client.isAlive();
				}
			});
	io.run();

	ASSERT_TRUE(results[0].error.ok()) << results[0].error.message;
	ASSERT_TRUE(results[1].error.ok()) << results[1].error.message;
	EXPECT_FALSE(results[0].body.empty());
	EXPECT_EQ(results[0].body, results[1].body); // 临时端口没变过 = 同一条连接
	EXPECT_TRUE(aliveAfter[0]);
	EXPECT_TRUE(aliveAfter[1]);
}

TEST(AsyncHttpClientTlsTest, Pool_MixesHttpsAndPlaintextWithoutCrossing)
{
	if (!testCertsExist())
	{
		if (std::getenv("HICAL_REQUIRE_SSL_CERTS") != nullptr)
		{
			FAIL() << "HICAL_REQUIRE_SSL_CERTS 已置位，但测试证书缺失（" << kTestCertFile << "）";
		}
		GTEST_SKIP() << "跳过：测试证书不存在";
	}

	auto plainServer = std::make_unique<HttpServer>(0);
	plainServer->router().get("/id",
							  [](const HttpRequest&) -> HttpResponse
							  {
								  return HttpResponse::ok("PLAIN");
							  });
	BackgroundServer plain(std::move(plainServer));
	uint16_t plainPort = plain.startAndWait();
	ASSERT_NE(plainPort, 0);

	TlsBackgroundServer tls(
		[](HttpServer& s)
		{
			s.router().get("/id",
						   [](const HttpRequest&) -> HttpResponse
						   {
							   return HttpResponse::ok("TLS");
						   });
		});
	uint16_t tlsPort = tls.startAndWait();
	ASSERT_NE(tlsPort, 0);

	io_context io;
	HttpClientPool pool(io);
	auto tlsOptions = makeTlsOptions(false);

	HttpClientResult r1;
	HttpClientResult r2;
	HttpClientResult r3;
	coSpawn(io,
			[&]() -> Awaitable<void>
			{
				r1 = co_await pool.get("127.0.0.1", tlsPort, "/id", {}, tlsOptions);
				r2 = co_await pool.get("127.0.0.1", plainPort, "/id");
				r3 = co_await pool.get("127.0.0.1", tlsPort, "/id", {}, tlsOptions);
				pool.shutdown();
			});
	io.run();

	// 加密和明文各走各的桶，来回交替也不能串
	EXPECT_EQ(r1.body, "TLS");
	EXPECT_EQ(r2.body, "PLAIN");
	EXPECT_EQ(r3.body, "TLS");
}

// ============ SNI：本机自签证书测不出漏设，得用 raw TLS 对端抓 ============

namespace
{

	/**
	 * @brief 只握手一次的 raw TLS 服务端，把客户端报上来的 SNI 原样塞进响应 body
	 * Hical 的 HttpServer 不暴露 SNI，所以这里自己搭个 TLS 对端，握手后用
	 * SSL_get_servername 把客户端给的 SNI 抓出来。客户端漏设 SNI 的话 body 会是空的。
	 */
	class RawTlsSniServer
	{
	public:
		RawTlsSniServer()
			: ctx_(boost::asio::ssl::context::tls_server)
			, acceptor_(io_, boost::asio::ip::tcp::endpoint(boost::asio::ip::tcp::v4(), 0))
			, port_(acceptor_.local_endpoint().port())
		{
			ctx_.use_certificate_chain_file(kTestCertFile);
			ctx_.use_private_key_file(kTestKeyFile, boost::asio::ssl::context::pem);
		}

		[[nodiscard]] uint16_t port() const noexcept
		{
			return port_;
		}

		/// 后台线程等一条连接：握手、抓 SNI、回一个 body 就是 SNI 的响应
		void serveOnce()
		{
			thread_ = std::thread(
				[this]()
				{
					boost::system::error_code ec;
					boost::asio::ip::tcp::socket raw(io_);
					acceptor_.accept(raw, ec);
					if (ec)
					{
						return;
					}

					boost::asio::ssl::stream<boost::asio::ip::tcp::socket> stream(std::move(raw), ctx_);
					stream.handshake(boost::asio::ssl::stream_base::server, ec);
					if (ec)
					{
						return;
					}

					const char* sni = SSL_get_servername(stream.native_handle(), TLSEXT_NAMETYPE_host_name);
					std::string body = sni != nullptr ? sni : "";

					std::string req;
					char buf[4096];
					for (;;)
					{
						auto n = stream.read_some(boost::asio::buffer(buf), ec);
						if (ec)
						{
							break;
						}
						req.append(buf, n);
						if (req.find("\r\n\r\n") != std::string::npos)
						{
							break;
						}
					}

					std::string resp = "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(body.size())
									   + "\r\nConnection: close\r\n\r\n" + body;
					boost::asio::write(stream, boost::asio::buffer(resp), ec);
					boost::system::error_code shutEc;
					stream.shutdown(shutEc);
				});
		}

		~RawTlsSniServer()
		{
			if (thread_.joinable())
			{
				thread_.join();
			}
		}

	private:
		boost::asio::io_context io_;
		boost::asio::ssl::context ctx_;
		boost::asio::ip::tcp::acceptor acceptor_;
		uint16_t port_;
		std::thread thread_;
	};

} // namespace

TEST(AsyncHttpClientTlsTest, SendsConfiguredSniHostName)
{
	if (!testCertsExist())
	{
		if (std::getenv("HICAL_REQUIRE_SSL_CERTS") != nullptr)
		{
			FAIL() << "HICAL_REQUIRE_SSL_CERTS 已置位，但测试证书缺失（" << kTestCertFile << "）";
		}
		GTEST_SKIP() << "跳过：测试证书不存在";
	}

	RawTlsSniServer server;
	server.serveOnce();

	io_context io;
	auto options = makeTlsOptions(false);
	options.sniHost = "example.test"; // 故意跟 host 不同，验的就是这个串发出去了
	HttpClient client(io, "127.0.0.1", server.port(), options);
	HttpClientResult result;
	coSpawn(io,
			[&]() -> Awaitable<void>
			{
				result = co_await client.get("/");
			});
	io.run();

	ASSERT_TRUE(result.error.ok()) << result.error.message;
	EXPECT_EQ(result.status, 200);
	// 服务端 SSL_get_servername 抓到的就是客户端设的 SNI
	EXPECT_EQ(result.body, "example.test");
}

TEST(AsyncHttpClientTlsTest, SniFallsBackToHostWhenUnset)
{
	if (!testCertsExist())
	{
		if (std::getenv("HICAL_REQUIRE_SSL_CERTS") != nullptr)
		{
			FAIL() << "HICAL_REQUIRE_SSL_CERTS 已置位，但测试证书缺失（" << kTestCertFile << "）";
		}
		GTEST_SKIP() << "跳过：测试证书不存在";
	}

	RawTlsSniServer server;
	server.serveOnce();

	io_context io;
	// sniHost 留空 → 回落 host。用 IP 字面量当 host，规避域名解析带来的耗时。
	// 客户端不会为 IP 常客跳过 SNI，回落做的就是「把 host 原样报上去」。
	HttpClient client(io, "127.0.0.1", server.port(), makeTlsOptions(false));
	HttpClientResult result;
	coSpawn(io,
			[&]() -> Awaitable<void>
			{
				result = co_await client.get("/");
			});
	io.run();

	ASSERT_TRUE(result.error.ok()) << result.error.message;
	EXPECT_EQ(result.status, 200);
	EXPECT_EQ(result.body, "127.0.0.1");
}

namespace
{

	/**
	 * @brief 每响应完就关连接的 raw TLS 服务端，能接连接受若干条
	 * 读一个请求、回一个带 Connection: close 的响应、立刻关掉，然后继续接。
	 * 客户端收到这种响应会以为还能复用，实际上连接已经被对端关了——第二次请求
	 * 必须在同一条 TLS 流上重连 + 重握手，专门验这条恢复路径。
	 */
	class RawTlsAlwaysCloseServer
	{
	public:
		explicit RawTlsAlwaysCloseServer(int serveCount)
			: ctx_(boost::asio::ssl::context::tls_server)
			, acceptor_(io_, boost::asio::ip::tcp::endpoint(boost::asio::ip::tcp::v4(), 0))
			, port_(acceptor_.local_endpoint().port())
			, serveCount_(serveCount)
		{
			ctx_.use_certificate_chain_file(kTestCertFile);
			ctx_.use_private_key_file(kTestKeyFile, boost::asio::ssl::context::pem);
		}

		[[nodiscard]] uint16_t port() const noexcept
		{
			return port_;
		}

		void start()
		{
			thread_ = std::thread(
				[this]()
				{
					boost::system::error_code ec;
					for (int i = 0; i < serveCount_; ++i)
					{
						boost::asio::ip::tcp::socket raw(io_);
						acceptor_.accept(raw, ec);
						if (ec)
						{
							return;
						}

						boost::asio::ssl::stream<boost::asio::ip::tcp::socket> stream(std::move(raw), ctx_);
						stream.handshake(boost::asio::ssl::stream_base::server, ec);
						if (ec)
						{
							return;
						}

						std::string req;
						char buf[4096];
						for (;;)
						{
							auto n = stream.read_some(boost::asio::buffer(buf), ec);
							if (ec)
							{
								break;
							}
							req.append(buf, n);
							if (req.find("\r\n\r\n") != std::string::npos)
							{
								break;
							}
						}

						constexpr std::string_view kResp =
							"HTTP/1.1 200 OK\r\nContent-Length: 5\r\nConnection: close\r\n\r\nhello";
						boost::asio::write(stream, boost::asio::buffer(kResp), ec);
						boost::system::error_code shutEc;
						stream.shutdown(shutEc);
					}
				});
		}

		~RawTlsAlwaysCloseServer()
		{
			if (thread_.joinable())
			{
				thread_.join();
			}
		}

	private:
		boost::asio::io_context io_;
		boost::asio::ssl::context ctx_;
		boost::asio::ip::tcp::acceptor acceptor_;
		uint16_t port_;
		int serveCount_ {0};
		std::thread thread_;
	};

} // namespace

TEST(AsyncHttpClientTlsTest, ReconnectsAndRehandshakesAfterPeerCloses)
{
	if (!testCertsExist())
	{
		if (std::getenv("HICAL_REQUIRE_SSL_CERTS") != nullptr)
		{
			FAIL() << "HICAL_REQUIRE_SSL_CERTS 已置位，但测试证书缺失（" << kTestCertFile << "）";
		}
		GTEST_SKIP() << "跳过：测试证书不存在";
	}

	RawTlsAlwaysCloseServer server(2);
	server.start();

	io_context io;
	HttpClient client(io, "127.0.0.1", server.port(), makeTlsOptions(false));

	std::vector<HttpClientResult> results(2);
	coSpawn(io,
			[&]() -> Awaitable<void>
			{
				for (std::size_t i = 0; i < results.size(); ++i)
				{
					results[i] = co_await client.get("/");
				}
			});
	io.run();

	// 第一次响应带 Connection: close，连接被对端关了；第二次必须重连并在同一条
	// ssl::stream 上重新握手，两次都得成
	ASSERT_TRUE(results[0].error.ok()) << results[0].error.message;
	EXPECT_EQ(results[0].body, "hello");
	ASSERT_TRUE(results[1].error.ok()) << results[1].error.message;
	EXPECT_EQ(results[1].body, "hello");
}
