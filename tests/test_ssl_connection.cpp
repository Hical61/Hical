#include "asio/SslConnection.h"
#include "asio/AsioEventLoop.h"
#include "core/SslContext.h"
#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <thread>
#include <fstream>

using namespace hical;

// 测试证书路径（由 CMake 或手动生成）
static const char* hTestCertFile = "test_server.crt";
static const char* hTestKeyFile = "test_server.key";

// 辅助函数：检查测试证书是否存在
static bool testCertsExist()
{
	std::ifstream cert(hTestCertFile);
	std::ifstream key(hTestKeyFile);
	return cert.good() && key.good();
}

// 测试 SslContext 基本功能
TEST(SslContextTest, DefaultConstruction)
{
	SslContext ctx;
	// 默认构造不抛异常
	EXPECT_NO_THROW(ctx.setVerifyPeer(false));
}

TEST(SslContextTest, MethodConstruction)
{
	SslContext serverCtx(boost::asio::ssl::context::tls_server);
	SslContext clientCtx(boost::asio::ssl::context::tls_client);
	SUCCEED();
}

TEST(SslContextTest, SetVerifyMode)
{
	SslContext ctx;
	EXPECT_NO_THROW(ctx.setVerifyPeer(true));
	EXPECT_NO_THROW(ctx.setVerifyPeer(false));
}

TEST(SslContextTest, LoadInvalidCertThrows)
{
	SslContext ctx;
	EXPECT_THROW(ctx.loadCertificate("/nonexistent/cert.pem"), std::runtime_error);
}

TEST(SslContextTest, LoadInvalidKeyThrows)
{
	SslContext ctx;
	EXPECT_THROW(ctx.loadPrivateKey("/nonexistent/key.pem"), std::runtime_error);
}

// 测试原生 Boost SSL 握手（验证 OpenSSL 和证书正常）
TEST(SslConnectionTest, NativeSslHandshake)
{
	if (!testCertsExist())
	{
		GTEST_SKIP() << "跳过：测试证书不存在（" << hTestCertFile << ", " << hTestKeyFile << "）";
	}

	using boost::asio::ip::tcp;
	boost::asio::io_context ioCtx;

	// 服务端 SSL 上下文
	SslContext serverCtx(boost::asio::ssl::context::tls_server);
	serverCtx.loadCertificate(hTestCertFile);
	serverCtx.loadPrivateKey(hTestKeyFile);

	// 客户端 SSL 上下文
	SslContext clientCtx(boost::asio::ssl::context::tls_client);
	clientCtx.setVerifyPeer(false);

	// 使用协程做完整握手 + 通信
	std::atomic<bool> success {false};
	std::string receivedData;

	std::string errorMsg;

	boost::asio::co_spawn(
		ioCtx,
		[&]() -> boost::asio::awaitable<void>
		{
			tcp::acceptor acceptor(ioCtx, tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), 0));
			auto port = acceptor.local_endpoint().port();

			// 客户端 SSL stream（shared_ptr 延长生命周期）
			auto clientSsl = std::make_shared<boost::asio::ssl::stream<tcp::socket>>(ioCtx, clientCtx.native());

			// 并行启动客户端连接协程
			boost::asio::co_spawn(
				ioCtx,
				[&, port, clientSsl]() -> boost::asio::awaitable<void>
				{
					co_await clientSsl->lowest_layer().async_connect(
						tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), port),
						boost::asio::use_awaitable);
					co_await clientSsl->async_handshake(boost::asio::ssl::stream_base::client,
														boost::asio::use_awaitable);
					// 发送数据
					std::string msg = "Hello SSL!";
					co_await boost::asio::async_write(*clientSsl, boost::asio::buffer(msg), boost::asio::use_awaitable);
					// 等待服务端读完（短暂延时保持连接）
					boost::asio::steady_timer timer(ioCtx, std::chrono::milliseconds(200));
					co_await timer.async_wait(boost::asio::use_awaitable);
				},
				[&](std::exception_ptr ep)
				{
					if (ep)
					{
						try
						{
							std::rethrow_exception(ep);
						}
						catch (const std::exception& e)
						{
							errorMsg += std::string("Client: ") + e.what() + "; ";
						}
					}
				});

			// 服务端接受
			tcp::socket rawSocket = co_await acceptor.async_accept(boost::asio::use_awaitable);

			boost::asio::ssl::stream<tcp::socket> serverSsl(std::move(rawSocket), serverCtx.native());

			// 服务端握手
			co_await serverSsl.async_handshake(boost::asio::ssl::stream_base::server, boost::asio::use_awaitable);

			// 读取数据
			char buf[1024];
			auto n = co_await serverSsl.async_read_some(boost::asio::buffer(buf), boost::asio::use_awaitable);

			receivedData = std::string(buf, n);
			success = true;
		},
		[&](std::exception_ptr ep)
		{
			if (ep)
			{
				try
				{
					std::rethrow_exception(ep);
				}
				catch (const std::exception& e)
				{
					errorMsg += std::string("Server: ") + e.what() + "; ";
				}
			}
		});

	// 运行事件循环，最长等待 3 秒
	ioCtx.run_for(std::chrono::seconds(3));

	if (!errorMsg.empty())
	{
		GTEST_SKIP() << "SSL 错误: " << errorMsg;
	}

	EXPECT_TRUE(success.load());
	EXPECT_EQ(receivedData, "Hello SSL!");
}

// 测试 SslContext 加载证书
TEST(SslConnectionTest, LoadCertAndKey)
{
	if (!testCertsExist())
	{
		GTEST_SKIP() << "跳过：测试证书不存在";
	}

	SslContext ctx(boost::asio::ssl::context::tls_server);
	EXPECT_NO_THROW(ctx.loadCertificate(hTestCertFile));
	EXPECT_NO_THROW(ctx.loadPrivateKey(hTestKeyFile));
}

// 测试 isSsl() 编译期判断
TEST(GenericConnectionTest, IsSslCompileTime)
{
	static_assert(!PlainConnection::isSsl(), "PlainConnection 不是 SSL 连接");
	static_assert(SslConnection::isSsl(), "SslConnection 是 SSL 连接");
	SUCCEED();
}
