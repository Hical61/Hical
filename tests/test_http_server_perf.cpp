/**
 * @file test_http_server_perf.cpp
 * @brief HTTP Server 吞量基准测试
 * 报谷 P0 建议：建立可比较的全链路性能基线
 * 为后续 epoll_ctl 优化（concurrency_hint / EPOLLET / io_uring）提供对比基准
 */

#include "TestHttpClient.h"
#include "core/HttpServer.h"
#include <boost/asio.hpp>
#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

using namespace hical;
using boost::asio::ip::tcp;

namespace
{

	/// 辅助：启动服务器并等待就绪，返回实际端口（同 test_http_server.cpp）
	uint16_t startServerAndWait(HttpServer& server, std::thread& serverThread)
	{
		serverThread = std::thread(
			[&server]()
			{
				server.start();
			});

		// 等待端口分配
		uint16_t port = 0;
		for (int i = 0; i < 50; ++i)
		{
			port = server.port();
			if (port != 0)
			{
				break;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
		}

		// 等待可连接
		for (int i = 0; i < 50; ++i)
		{
			try
			{
				boost::asio::io_context io;
				tcp::socket sock(io);
				sock.connect(tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), port));
				sock.close();
				return port;
			}
			catch (...)
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(20));
			}
		}
		return port;
	}

	/// 通过单条 keep-alive 连接连续发送 HTTP GET 请求，返回 req/s
	double runSingleConnectionBench(uint16_t port, const std::string& target, int requestCount)
	{
		boost::asio::io_context ioCtx;
		tcp::socket sock(ioCtx);
		sock.connect(tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), port));

		// 构造请求（带 Connection: keep-alive）
		std::string req = "GET " + target
						  + " HTTP/1.1\r\n"
							"Host: 127.0.0.1\r\n"
							"Connection: keep-alive\r\n"
							"\r\n";

		std::string residual;

		// 预热：先发 10 个请求，每个读完响应
		for (int i = 0; i < 10; ++i)
		{
			sock.send(boost::asio::buffer(req));
			(void)hical::test::detail::readHttpResponse(sock, residual);
		}

		// 计时发送
		auto start = std::chrono::high_resolution_clock::now();
		for (int i = 0; i < requestCount; ++i)
		{
			sock.send(boost::asio::buffer(req));
			(void)hical::test::detail::readHttpResponse(sock, residual);
		}
		auto end = std::chrono::high_resolution_clock::now();

		double totalSec = std::chrono::duration<double>(end - start).count();
		double rps = requestCount / totalSec;

		boost::system::error_code ec;
		sock.shutdown(tcp::socket::shutdown_both, ec);
		return rps;
	}

	/// 并发连接测试：多个线程各自建立一条连接，各自发送 N 个请求
	/// 返回总吞吐量（所有连接 req/s 之和）
	double runMultiConnectionBench(uint16_t port, const std::string& target, int numConnections, int requestsPerConn)
	{
		std::atomic<int> completedRequests {0};
		std::atomic<bool> failed {false};

		std::vector<std::thread> threads;
		threads.reserve(static_cast<size_t>(numConnections));

		auto worker = [&](int localCount)
		{
			try
			{
				boost::asio::io_context ioCtx;
				tcp::socket sock(ioCtx);
				sock.connect(tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), port));

				std::string req = "GET " + target
								  + " HTTP/1.1\r\n"
									"Host: 127.0.0.1\r\n"
									"Connection: keep-alive\r\n"
									"\r\n";

				std::string residual;

				// 预热
				for (int i = 0; i < 5; ++i)
				{
					sock.send(boost::asio::buffer(req));
					(void)hical::test::detail::readHttpResponse(sock, residual);
				}

				for (int i = 0; i < localCount; ++i)
				{
					sock.send(boost::asio::buffer(req));
					(void)hical::test::detail::readHttpResponse(sock, residual);
					completedRequests.fetch_add(1, std::memory_order_relaxed);
				}

				boost::system::error_code ec;
				sock.shutdown(tcp::socket::shutdown_both, ec);
			}
			catch (...)
			{
				failed.store(true, std::memory_order_relaxed);
			}
		};

		auto start = std::chrono::high_resolution_clock::now();

		for (int i = 0; i < numConnections; ++i)
		{
			threads.emplace_back(worker, requestsPerConn);
		}
		for (auto& t : threads)
		{
			t.join();
		}

		auto end = std::chrono::high_resolution_clock::now();
		double totalSec = std::chrono::duration<double>(end - start).count();
		int totalReqs = completedRequests.load(std::memory_order_relaxed);

		EXPECT_FALSE(failed.load()) << "有连接发生异常";
		return totalReqs / totalSec;
	}

	constexpr int kPerfBench = 5000;

} // anonymous namespace

/**
 * @brief 单连接连续请求（sync handler）
 * 使用 keep-alive 连接连续发送 5000 次请求
 * 测量端到端吞吐量（含 TCP send/recv 开销）
 */
TEST(HttpServerPerfTest, SyncHandler_SingleConn_5kReq)
{
	HttpServer server(0);
	server.router().get("/hello",
						[](const HttpRequest&) -> HttpResponse
						{
							return HttpResponse::ok("ok");
						});

	std::thread serverThread;
	uint16_t port = startServerAndWait(server, serverThread);

	double rps = runSingleConnectionBench(port, "/hello", kPerfBench);

	std::cout << "[SyncHandler-SingleConn] " << kPerfBench << " req, 吞吐量: " << static_cast<int>(rps) << " req/s\n";

	// 性能断言（宽松）：CI 环境（MSYS2/MinGW）可能较慢
	EXPECT_GT(rps, 500.0);

	server.stop();
	serverThread.join();
}

/**
 * @brief 多连接并发请求（sync handler）
 * 10 条连接各发 1000 次请求，测量总吞吐
 */
TEST(HttpServerPerfTest, SyncHandler_10Conn_1kEach)
{
	HttpServer server(0);
	server.router().get("/hello",
						[](const HttpRequest&) -> HttpResponse
						{
							return HttpResponse::ok("ok");
						});

	std::thread serverThread;
	uint16_t port = startServerAndWait(server, serverThread);

	double rps = runMultiConnectionBench(port, "/hello", 10, 1000);

	std::cout << "[SyncHandler-10Conn] 10 连接 x 1000 req, 吞吐量: " << static_cast<int>(rps) << " req/s\n";

	// 多连接吞吐应 > 单连接
	EXPECT_GT(rps, 1000.0);

	server.stop();
	serverThread.join();
}
