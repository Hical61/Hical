/**
 * @file test_http_client.cpp
 * @brief HttpTestClient 链式测试客户端验证
 */

#include "TestHttpClient.h"
#include "core/HttpServer.h"
#include <gtest/gtest.h>
#include <chrono>
#include <string>
#include <thread>

using namespace hical;
using boost::asio::ip::tcp;

namespace
{

	/**
	 * @brief 集成测试 fixture，所有测试共享一个 HttpServer
	 * 避免反复启停服务器带来的端口竞争
	 */
	class HttpClientTest : public ::testing::Test
	{
	protected:
		static uint16_t port_;
		static std::unique_ptr<HttpServer> server_;
		static std::thread serverThread_;

		static void SetUpTestSuite()
		{
			server_ = std::make_unique<HttpServer>(0);

			// 注册基础路由
			server_->router().get("/hello",
								  [](const HttpRequest&) -> HttpResponse
								  {
									  return HttpResponse::ok("world");
								  });

			server_->router().get("/json",
								  [](const HttpRequest&) -> HttpResponse
								  {
									  HttpResponse resp = HttpResponse::ok("{}");
									  resp.setHeader("Content-Type", "application/json");
									  return resp;
								  });

			server_->router().post("/echo",
								   [](const HttpRequest& req) -> HttpResponse
								   {
									   return HttpResponse::ok(req.body());
								   });

			server_->router().put("/update",
								  [](const HttpRequest& req) -> HttpResponse
								  {
									  return HttpResponse::ok("updated:" + req.body());
								  });

			server_->router().del("/resource/42",
								  [](const HttpRequest&) -> HttpResponse
								  {
									  return HttpResponse::ok("deleted");
								  });

			server_->router().route(HttpMethod::hPatch,
									"/patch",
									[](const HttpRequest& req) -> HttpResponse
									{
										return HttpResponse::ok("patched:" + req.body());
									});

			// 路由响应含附加头
			server_->router().get("/chain",
								  [](const HttpRequest&) -> HttpResponse
								  {
									  HttpResponse resp = HttpResponse::ok("hello world");
									  resp.setHeader("X-Custom", "value123");
									  return resp;
								  });

			// JSON 响应用于 bodyContains
			server_->router().get("/data",
								  [](const HttpRequest&) -> HttpResponse
								  {
									  return HttpResponse::ok("{\"key\": \"value\", \"num\": 42}");
								  });

			// Cookie 测试
			server_->router().get("/set-cookie",
								  [](const HttpRequest&) -> HttpResponse
								  {
									  HttpResponse resp = HttpResponse::ok("cookie-set");
									  hical::CookieOptions opts;
									  resp.setCookie("session_id", "abc123", opts);
									  return resp;
								  });

			server_->router().get("/check-cookie",
								  [](const HttpRequest& req) -> HttpResponse
								  {
									  auto val = req.header("Cookie");
									  return HttpResponse::ok(val.empty() ? "none" : std::string(val));
								  });

			// Chunked 响应（用于 HttpClientTest.ChunkedResponse）
			server_->router().get("/chunked",
								  [](const HttpRequest&) -> Awaitable<HttpResponse>
								  {
									  auto res = HttpResponse::chunked();
									  auto& body = res.chunkedBody();
									  body.write("hello");
									  body.write(" ");
									  body.write("world");
									  body.end();
									  co_return res;
								  });

			// 大响应体
			server_->router().get("/big",
								  [](const HttpRequest&) -> HttpResponse
								  {
									  std::string bigBody(65536, 'M');
									  return HttpResponse::ok(bigBody);
								  });

			// 启动
			serverThread_ = std::thread(
				[]()
				{
					server_->start();
				});

			// 等待就绪
			for (int i = 0; i < 50; ++i)
			{
				port_ = server_->port();
				if (port_ != 0)
				{
					break;
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(20));
			}
			for (int i = 0; i < 50; ++i)
			{
				try
				{
					boost::asio::io_context io;
					tcp::socket sock(io);
					sock.connect(tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), port_));
					sock.close();
					return;
				}
				catch (...)
				{
					std::this_thread::sleep_for(std::chrono::milliseconds(20));
				}
			}
			FAIL() << "服务器未能在超时内启动";
		}

		static void TearDownTestSuite()
		{
			if (server_)
			{
				server_->stop();
			}
			if (serverThread_.joinable())
			{
				serverThread_.join();
			}
			server_.reset();
		}
	};

	uint16_t HttpClientTest::port_ = 0;
	std::unique_ptr<HttpServer> HttpClientTest::server_;
	std::thread HttpClientTest::serverThread_;

} // namespace

// ============ GET 请求与链式断言 ============

TEST_F(HttpClientTest, GetRequest)
{
	hical::test::HttpTestClient client(port_);
	client.get("/hello").expectStatus(200).expectBody("world");
}

TEST_F(HttpClientTest, GetWithHeaderAssertion)
{
	hical::test::HttpTestClient client(port_);
	auto res = client.get("/json").expectStatus(200).expectHeader("Content-Type", "application/json");
}

// ============ POST 请求 ============

TEST_F(HttpClientTest, PostRequest)
{
	hical::test::HttpTestClient client(port_);
	client.post("/echo", "test-data", "text/plain").expectStatus(200).expectBody("test-data");

	client.post("/echo", "").expectStatus(200).expectBody("");
}

// ============ PUT / DELETE 请求 ============

TEST_F(HttpClientTest, PutRequest)
{
	hical::test::HttpTestClient client(port_);
	client.put("/update", "data123", "text/plain").expectStatus(200).expectBody("updated:data123");
}

TEST_F(HttpClientTest, DeleteRequest)
{
	hical::test::HttpTestClient client(port_);
	client.del("/resource/42").expectStatus(200).expectBody("deleted");
}

// ============ 自定义方法 ============

TEST_F(HttpClientTest, CustomMethod)
{
	hical::test::HttpTestClient client(port_);
	client.request("PATCH", "/patch", "fix", "application/json").expectStatus(200).expectBody("patched:fix");
}

// ============ Base URL 构造 ============

TEST_F(HttpClientTest, BaseUrlConstructor)
{
	std::string baseUrl = "http://127.0.0.1:" + std::to_string(port_);
	hical::test::HttpTestClient client(baseUrl);
	client.get("/hello").expectStatus(200).expectBody("world");
}

// ============ 链式断言顺序 ============

TEST_F(HttpClientTest, ChainedAssertions)
{
	hical::test::HttpTestClient client(port_);
	client.get("/chain")
		.expectStatus(200)
		.expectHeader("X-Custom", "value123")
		.expectBodyContains("hello")
		.expectBody("hello world");
}

// ============ 404 路由 ============

TEST_F(HttpClientTest, NotFound)
{
	hical::test::HttpTestClient client(port_);
	client.get("/nonexistent").expectStatus(404);
}

// ============ Body Contains 断言 ============

TEST_F(HttpClientTest, BodyContains)
{
	hical::test::HttpTestClient client(port_);
	client.get("/data").expectStatus(200).expectBodyContains("key").expectBodyContains("42");
}

// ============ 超时设置 ============

TEST_F(HttpClientTest, CustomTimeout)
{
	// 只验证超时设置不报错
	hical::test::HttpTestClient client(port_);
	client.setTimeout(std::chrono::milliseconds(5000));
	SUCCEED();
}

// ============ Cookie 自动追踪 ============

TEST_F(HttpClientTest, CookieTracking)
{
	hical::test::HttpTestClient client(port_);

	// 第一次请求设置 Cookie
	client.get("/set-cookie").expectStatus(200);

	// 第二次请求应该自动携带 Cookie
	auto res = client.get("/check-cookie");
	EXPECT_EQ(res.status(), 200u);
	EXPECT_TRUE(res.body().find("session_id=abc123") != std::string::npos
				|| res.body().find("abc123") != std::string::npos)
		<< "Cookie should be auto-tracked, body: " << res.body();

	// 清空 Cookie
	client.clearCookies();
	res = client.get("/check-cookie");
	EXPECT_EQ(res.status(), 200u);
}

// ============ 大响应体 ============

TEST_F(HttpClientTest, LargeResponseBody)
{
	hical::test::HttpTestClient client(port_);
	client.get("/big").expectStatus(200);
}

// ============ 连续请求（每次新连接） ============

TEST_F(HttpClientTest, MultipleRequests)
{
	hical::test::HttpTestClient client(port_);

	for (int i = 0; i < 5; ++i)
	{
		client.get("/hello").expectStatus(200).expectBody("world");
	}
}

// ============ Chunked 响应 ============

TEST_F(HttpClientTest, ChunkedResponse)
{
	hical::test::HttpTestClient client(port_);
	client.get("/chunked").expectStatus(200).expectBody("hello world");
}
