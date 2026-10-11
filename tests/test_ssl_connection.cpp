#include "asio/SslConnection.h"
#include "asio/AsioEventLoop.h"
#include "core/SslContext.h"
#include "core/HttpServer.h"
#include <gtest/gtest.h>
#include <boost/asio.hpp>
#include <atomic>
#include <chrono>
#include <cstdlib>
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

// 启动 HttpServer 并等端口就绪，返回实际监听端口
static uint16_t startServerAndWaitPort(HttpServer& server, std::thread& serverThread)
{
	serverThread = std::thread(
		[&server]()
		{
			server.start();
		});

	uint16_t port = 0;
	for (int i = 0; i < 100 && port == 0; ++i)
	{
		port = server.port();
		if (port == 0)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
		}
	}
	return port;
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

// 回归：TCP 连上后一直不发 ClientHello，握手协程挂死。这条连接必须跟普通空闲
// 连接一样被 IdleScanner 在 idleTimeout_ 内回收，否则光握手就能把连接槽占满。
TEST(SslConnectionTest, StalledHandshakeReapedByIdleTimeout)
{
	if (!testCertsExist())
	{
		if (std::getenv("HICAL_REQUIRE_SSL_CERTS") != nullptr)
		{
			FAIL() << "HICAL_REQUIRE_SSL_CERTS 已置位，但测试证书不存在（" << hTestCertFile << ", " << hTestKeyFile
				   << "）";
		}
		GTEST_SKIP() << "跳过：测试证书不存在（" << hTestCertFile << ", " << hTestKeyFile << "）";
	}

	using boost::asio::ip::tcp;

	HttpServer server(0);
	// 用短的 idleTimeout_ 控制耗时；扫描间隔是 max(1000ms, timeout/4)，
	// 2 秒超时 → 最坏 ~3 秒被扫到关掉，下面的断言窗口给到 10 秒，留足余量。
	server.setIdleTimeout(2.0);
	server.enableSsl(hTestCertFile, hTestKeyFile);

	std::thread serverThread;
	uint16_t port = startServerAndWaitPort(server, serverThread);
	if (port == 0)
	{
		// 用 FAIL 而不是 ASSERT：线程已经起来了，直接 return 会让 joinable 的
		// serverThread 析构触发 std::terminate，得先把服务器停干净。
		server.stop();
		serverThread.join();
		FAIL() << "服务器没有在预期时间内分配端口";
	}

	// 客户端只做 TCP 三次握手，一个字节都不发（压根不开始 TLS 握手）
	boost::asio::io_context ioCtx;
	tcp::socket socket(ioCtx);
	boost::system::error_code connectEc;
	socket.connect(tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), port), connectEc);
	if (connectEc)
	{
		server.stop();
		serverThread.join();
		FAIL() << "TCP 连接失败：" << connectEc.message();
	}

	std::atomic<bool> deadlineFired {false};
	std::atomic<bool> serverClosed {false};

	// 兜底 deadline：10 秒还没被回收就直接关掉本地 socket 结束等待，
	// 靠 deadlineFired 把「服务端关的」和「我们自己关的」区分开，避免假绿。
	boost::asio::steady_timer deadline(ioCtx, std::chrono::seconds(10));
	deadline.async_wait(
		[&](const boost::system::error_code& dEc)
		{
			if (dEc)
			{
				return; // 被正常取消，不是真的超时
			}
			deadlineFired.store(true);
			boost::system::error_code closeEc;
			socket.close(closeEc);
		});

	char buf[1];
	socket.async_read_some(boost::asio::buffer(buf),
						   [&](const boost::system::error_code& readEc, std::size_t)
						   {
							   if (!deadlineFired.load())
							   {
								   serverClosed.store(readEc.operator bool());
							   }
							   deadline.cancel();
						   });

	ioCtx.run();

	EXPECT_TRUE(serverClosed.load()) << (deadlineFired.load() ? "10 秒上限已到，服务端仍没回收这条停滞的 SSL 握手连接"
															  : "服务端关连接时客户端没拿到 EOF/错误");

	server.stop();
	serverThread.join();
}
