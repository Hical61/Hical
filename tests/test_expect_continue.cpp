/**
 * @file test_expect_continue.cpp
 * @brief Expect: 100-continue 集成测试
 */

#include "TestHttpClient.h"
#include "core/HttpServer.h"
#include "core/Router.h"
#include <boost/asio.hpp>
#include <gtest/gtest.h>
#include <chrono>
#include <string>
#include <thread>

using namespace hical;
using boost::asio::ip::tcp;

namespace
{

	class ExpectContinueTest : public ::testing::Test
	{
	protected:
		uint16_t port_ {0};
		std::unique_ptr<HttpServer> server_;
		std::thread serverThread_;

		void SetUp() override
		{
			server_ = std::make_unique<HttpServer>(0);
			server_->setMaxBodySize(512); // 小上限，方便测 413

			server_->router().post("/upload",
								   [](const HttpRequest& req) -> HttpResponse
								   {
									   return HttpResponse::ok("received:" + std::to_string(req.body().size()));
								   });

			server_->router().post("/items/{id}",
								   [](const HttpRequest& req) -> HttpResponse
								   {
									   return HttpResponse::ok("item:" + req.param("id"));
								   });
		}

		void startServer()
		{
			serverThread_ = std::thread(
				[this]()
				{
					server_->start();
				});

			for (int i = 0; i < 50; ++i)
			{
				port_ = server_->port();
				if (port_ == 0)
				{
					std::this_thread::sleep_for(std::chrono::milliseconds(20));
					continue;
				}
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
		}

		void TearDown() override
		{
			server_->stop();
			if (serverThread_.joinable())
			{
				serverThread_.join();
			}
		}

		/// 发送带 Expect: 100-continue 的 POST 请求，分两步：先发头部，收到 100/错误 后再决定是否发 body
		/// 返回 {interim_status, final_status, final_body}
		/// interim_status: 100 = 收到 100 Continue；其他 = 收到错误响应（body 未发）
		struct ExpectResult
		{
			unsigned interimStatus = 0; // 100 = 继续；其他 = 直接拒绝
			unsigned finalStatus = 0;   // 最终响应码（只有 interimStatus==100 时有意义）
			std::string finalBody;
		};

		ExpectResult sendWithExpect(const std::string& path, const std::string& body)
		{
			boost::asio::io_context io;
			tcp::socket sock(io);
			sock.connect(tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), port_));

			// 第一步：只发头部，带 Expect: 100-continue
			std::string req = "POST " + path
							  + " HTTP/1.1\r\n"
								"Host: localhost\r\n"
								"Content-Length: "
							  + std::to_string(body.size())
							  + "\r\n"
								"Expect: 100-continue\r\n"
								"Connection: close\r\n"
								"\r\n";
			boost::asio::write(sock, boost::asio::buffer(req));

			// 读取中间响应，直到收到完整头部（\r\n\r\n）
			char tmp[4096];
			std::string recvBuf;

			for (;;)
			{
				boost::system::error_code ec;
				auto n = sock.read_some(boost::asio::buffer(tmp), ec);
				if (n > 0)
				{
					recvBuf.append(tmp, n);
				}
				if (recvBuf.find("\r\n\r\n") != std::string::npos)
				{
					break;
				}
				if (ec)
				{
					break;
				}
			}

			// 解析状态码
			unsigned status = 0;
			if (recvBuf.size() >= 12)
			{
				// "HTTP/1.1 XXX"
				std::from_chars(recvBuf.data() + 9, recvBuf.data() + 12, status);
			}

			ExpectResult result;
			result.interimStatus = status;

			if (status != 100)
			{
				// 拒绝，body 不发
				sock.close();
				return result;
			}

			// 第二步：收到 100，发送 body
			boost::asio::write(sock, boost::asio::buffer(body));

			// 读取最终响应
			std::string residual = recvBuf.substr(
				recvBuf.find("\r\n\r\n") != std::string::npos ? recvBuf.find("\r\n\r\n") + 4 : recvBuf.size());
			auto finalRes = hical::test::detail::readHttpResponse(sock, residual);
			result.finalStatus = finalRes.status;
			result.finalBody = finalRes.body;

			sock.close();
			return result;
		}
	};

} // namespace

// 路由存在 → 收到 100 Continue → 发 body → 正常响应 200
TEST_F(ExpectContinueTest, RouteExistsReceives100ThenOk)
{
	startServer();

	std::string body(128, 'A');
	auto result = sendWithExpect("/upload", body);

	EXPECT_EQ(result.interimStatus, 100u);
	EXPECT_EQ(result.finalStatus, 200u);
	EXPECT_EQ(result.finalBody, "received:128");
}

// 路由不存在 → 直接收到 404，body 未发送
TEST_F(ExpectContinueTest, RouteNotFoundRejects404)
{
	startServer();

	std::string body(1024, 'B');
	auto result = sendWithExpect("/nonexistent", body);

	EXPECT_EQ(result.interimStatus, 404u);
	// 拒绝后连接关闭，body 未被服务端读取
	EXPECT_EQ(result.finalStatus, 0u);
}

// 不带 Expect 头的普通请求，行为与现在完全一致
TEST_F(ExpectContinueTest, NoExpectHeaderWorksNormally)
{
	startServer();

	boost::asio::io_context io;
	tcp::socket sock(io);
	sock.connect(tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), port_));

	std::string body = "hello";
	std::string req = "POST /upload HTTP/1.1\r\n"
					  "Host: localhost\r\n"
					  "Content-Length: 5\r\n"
					  "Connection: close\r\n"
					  "\r\n"
					  + body;

	boost::asio::write(sock, boost::asio::buffer(req));

	std::string residual;
	auto res = hical::test::detail::readHttpResponse(sock, residual);
	EXPECT_EQ(res.status, 200u);
	EXPECT_EQ(res.body, "received:5");

	sock.close();
}

// Content-Length 超限 → 在发 100 之前直接 413，body 未上传
TEST_F(ExpectContinueTest, OversizedBodyRejects413BeforeSending100)
{
	startServer();

	// maxBodySize_ 设为 512，body 声明 1024 字节
	std::string body(1024, 'X');
	auto result = sendWithExpect("/upload", body);

	// 应在 100 之前收到 413，body 未发送
	EXPECT_EQ(result.interimStatus, 413u);
	EXPECT_EQ(result.finalStatus, 0u);
}

// 参数路由 + Expect → exists() 走参数分支，正常发 100
TEST_F(ExpectContinueTest, ParamRouteReceives100ThenOk)
{
	startServer();

	std::string body = "data";
	auto result = sendWithExpect("/items/42", body);

	EXPECT_EQ(result.interimStatus, 100u);
	EXPECT_EQ(result.finalStatus, 200u);
	EXPECT_EQ(result.finalBody, "item:42");
}

// HTTP/1.0 带 Expect 头 → 忽略（不发 100），直接读 body，正常响应
TEST_F(ExpectContinueTest, Http10IgnoresExpect)
{
	startServer();

	boost::asio::io_context io;
	tcp::socket sock(io);
	sock.connect(tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), port_));

	std::string body = "hi";
	std::string req = "POST /upload HTTP/1.0\r\n"
					  "Host: localhost\r\n"
					  "Content-Length: 2\r\n"
					  "Expect: 100-continue\r\n"
					  "\r\n"
					  + body;

	boost::asio::write(sock, boost::asio::buffer(req));

	std::string residual;
	auto res = hical::test::detail::readHttpResponse(sock, residual);
	// 没有发 100，直接读完 body 后正常响应
	EXPECT_EQ(res.status, 200u);
	EXPECT_EQ(res.body, "received:2");

	sock.close();
}
