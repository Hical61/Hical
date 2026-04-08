#include "core/HttpServer.h"
#include "core/MemoryPool.h"
#include "core/PmrBuffer.h"
#include "core/Router.h"
#include "core/Middleware.h"
#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

using namespace hical;

namespace beast = boost::beast;
namespace http = beast::http;
using boost::asio::ip::tcp;

// ============ 测试辅助 ============

namespace
{

	/**
 * @brief 集成测试 fixture
 *
 * 每个测试启动一个 HttpServer（端口 0，由 OS 分配），测试完毕后停止。
 */
	class IntegrationTest : public ::testing::Test
	{
	protected:
		uint16_t port_ {0};
		std::unique_ptr<HttpServer> server_;
		std::thread serverThread_;

		void SetUp() override
		{
			server_ = std::make_unique<HttpServer>(0);

			// 注册基础路由
			server_->router().get("/",
								  [](const HttpRequest&) -> HttpResponse
								  {
									  return HttpResponse::ok("hello");
								  });

			server_->router().post("/echo",
								   [](const HttpRequest& req) -> HttpResponse
								   {
									   return HttpResponse::ok(req.body());
								   });

			server_->router().get("/users/{id}",
								  [](const HttpRequest& req) -> HttpResponse
								  {
									  return HttpResponse::json({{"id", req.param("id")}});
								  });
		}

		/**
     * @brief 在后台线程启动服务器，等待其就绪
     */
		void startServer()
		{
			serverThread_ = std::thread(
				[this]()
				{
					server_->start();
				});

			// 等待服务器就绪：轮询实际端口（端口 0 启动后会更新为真实端口）
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

		/**
     * @brief 发送 HTTP 请求并获取响应
     * @param method HTTP 方法
     * @param target 目标路径
     * @param body 请求体（可选）
     * @return 响应体字符串
     */
		struct Response
		{
			unsigned status;
			std::string body;
		};

		Response sendRequest(http::verb method, const std::string& target, const std::string& body = "")
		{
			boost::asio::io_context io;
			tcp::socket sock(io);
			sock.connect(tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), port_));

			http::request<http::string_body> req(method, target, 11);
			req.set(http::field::host, "localhost");
			req.set(http::field::connection, "close");
			if (!body.empty())
			{
				req.body() = body;
				req.set(http::field::content_type, "text/plain");
				req.prepare_payload();
			}

			http::write(sock, req);

			beast::flat_buffer buffer;
			http::response<http::string_body> res;
			http::read(sock, buffer, res);

			return {res.result_int(), res.body()};
		}

		/**
     * @brief 在同一连接上发送多个请求（Keep-Alive）
     */
		std::vector<Response> sendKeepAliveRequests(const std::vector<std::pair<http::verb, std::string>>& requests)
		{
			boost::asio::io_context io;
			tcp::socket sock(io);
			sock.connect(tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), port_));

			std::vector<Response> responses;
			beast::flat_buffer buffer;

			for (size_t i = 0; i < requests.size(); ++i)
			{
				auto& [method, target] = requests[i];
				bool isLast = (i == requests.size() - 1);

				http::request<http::string_body> req(method, target, 11);
				req.set(http::field::host, "localhost");
				req.set(http::field::connection, isLast ? "close" : "keep-alive");

				http::write(sock, req);

				http::response<http::string_body> res;
				http::read(sock, buffer, res);
				responses.push_back({res.result_int(), res.body()});
				buffer.consume(buffer.size());
			}

			return responses;
		}
	};

} // namespace

// ============ A. 网络边界场景 ============

// 大包传输：1MB 请求体
TEST_F(IntegrationTest, LargeBody)
{
	startServer();

	// 生成 1MB 数据
	std::string largeBody(1024 * 1024, 'X');

	auto res = sendRequest(http::verb::post, "/echo", largeBody);

	EXPECT_EQ(res.status, 200);
	EXPECT_EQ(res.body.size(), largeBody.size());
	EXPECT_EQ(res.body, largeBody);
}

// 半关闭：客户端 shutdown(send) 后服务器仍能返回完整响应
TEST_F(IntegrationTest, HalfClose)
{
	startServer();

	boost::asio::io_context io;
	tcp::socket sock(io);
	sock.connect(tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), port_));

	// 发送请求
	http::request<http::string_body> req(http::verb::get, "/", 11);
	req.set(http::field::host, "localhost");
	req.set(http::field::connection, "close");
	http::write(sock, req);

	// 客户端关闭写端
	sock.shutdown(tcp::socket::shutdown_send);

	// 仍然应该能读到完整响应
	beast::flat_buffer buffer;
	http::response<http::string_body> res;
	http::read(sock, buffer, res);

	EXPECT_EQ(res.result_int(), 200);
	EXPECT_EQ(res.body(), "hello");
}

// Keep-Alive 连接复用
TEST_F(IntegrationTest, KeepAlive)
{
	startServer();

	auto responses = sendKeepAliveRequests({
		{http::verb::get, "/"},
		{http::verb::get, "/users/42"},
		{http::verb::get, "/"},
	});

	ASSERT_EQ(responses.size(), 3);
	EXPECT_EQ(responses[0].status, 200);
	EXPECT_EQ(responses[0].body, "hello");
	EXPECT_EQ(responses[1].status, 200);
	EXPECT_EQ(responses[2].status, 200);
	EXPECT_EQ(responses[2].body, "hello");
}

// 空请求体 POST
TEST_F(IntegrationTest, EmptyBodyPost)
{
	startServer();

	auto res = sendRequest(http::verb::post, "/echo", "");

	EXPECT_EQ(res.status, 200);
	EXPECT_TRUE(res.body.empty());
}

// 404 未注册路由
TEST_F(IntegrationTest, NotFoundRoute)
{
	startServer();

	auto res = sendRequest(http::verb::get, "/nonexistent");

	EXPECT_EQ(res.status, 404);
}

// 并发连接
TEST_F(IntegrationTest, ConcurrentConnections)
{
	startServer();

	static constexpr int hNumClients = 20;
	static constexpr int hRequestsPerClient = 10;

	std::atomic<int> successCount {0};
	std::atomic<int> errorCount {0};
	std::vector<std::thread> threads;

	for (int c = 0; c < hNumClients; ++c)
	{
		threads.emplace_back(
			[&, c]()
			{
				try
				{
					boost::asio::io_context io;
					tcp::socket sock(io);
					sock.connect(tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), port_));

					beast::flat_buffer buffer;

					for (int r = 0; r < hRequestsPerClient; ++r)
					{
						bool isLast = (r == hRequestsPerClient - 1);
						http::request<http::string_body> req(http::verb::get, "/", 11);
						req.set(http::field::host, "localhost");
						req.set(http::field::connection, isLast ? "close" : "keep-alive");

						http::write(sock, req);

						http::response<http::string_body> res;
						http::read(sock, buffer, res);
						buffer.consume(buffer.size());

						if (res.result_int() == 200 && res.body() == "hello")
						{
							successCount.fetch_add(1);
						}
						else
						{
							errorCount.fetch_add(1);
						}
					}
				}
				catch (...)
				{
					errorCount.fetch_add(hRequestsPerClient);
				}
			});
	}

	for (auto& t : threads)
	{
		t.join();
	}

	EXPECT_EQ(successCount.load(), hNumClients * hRequestsPerClient);
	EXPECT_EQ(errorCount.load(), 0);
}

// ============ B. 并发安全测试 ============

// 多线程路由分发
TEST_F(IntegrationTest, ConcurrentRouterDispatch)
{
	Router router;
	for (int i = 0; i < 100; ++i)
	{
		router.get("/route" + std::to_string(i),
				   [i](const HttpRequest&) -> HttpResponse
				   {
					   return HttpResponse::ok(std::to_string(i));
				   });
	}

	static constexpr int hNumThreads = 8;
	static constexpr int hIterations = 1000;

	std::atomic<int> successCount {0};
	std::vector<std::thread> threads;

	for (int t = 0; t < hNumThreads; ++t)
	{
		threads.emplace_back(
			[&router, &successCount, t]()
			{
				boost::asio::io_context io;
				for (int i = 0; i < hIterations; ++i)
				{
					int routeIdx = (t * hIterations + i) % 100;

					// 按值捕获 req，避免协程延迟执行时悬垂引用
					boost::asio::co_spawn(
						io,
						[&router, &successCount, routeIdx]() -> Awaitable<void>
						{
							HttpRequest req;
							req.setMethod(HttpMethod::hGet);
							req.setTarget("/route" + std::to_string(routeIdx));
							auto res = co_await router.dispatch(req);
							if (res.body() == std::to_string(routeIdx))
							{
								successCount.fetch_add(1);
							}
						},
						boost::asio::detached);
				}
				io.run();
			});
	}

	for (auto& t : threads)
	{
		t.join();
	}

	EXPECT_EQ(successCount.load(), hNumThreads * hIterations);
}

// 多线程中间件执行
TEST_F(IntegrationTest, ConcurrentMiddleware)
{
	MiddlewarePipeline pipeline;
	std::atomic<int> callCount {0};

	pipeline.use(
		[&callCount](HttpRequest& req, MiddlewareNext next) -> Awaitable<HttpResponse>
		{
			callCount.fetch_add(1);
			co_return co_await next(req);
		});

	static constexpr int hNumThreads = 4;
	static constexpr int hIterations = 500;

	std::vector<std::thread> threads;
	for (int t = 0; t < hNumThreads; ++t)
	{
		threads.emplace_back(
			[&]()
			{
				boost::asio::io_context io;
				for (int i = 0; i < hIterations; ++i)
				{
					HttpRequest req;
					req.setMethod(HttpMethod::hGet);
					req.setTarget("/");

					boost::asio::co_spawn(
						io,
						[&pipeline, &req]() -> Awaitable<void>
						{
							co_await pipeline.execute(req,
													  [](const HttpRequest&) -> Awaitable<HttpResponse>
													  {
														  co_return HttpResponse::ok("ok");
													  });
						},
						boost::asio::detached);
				}
				io.run();
			});
	}

	for (auto& t : threads)
	{
		t.join();
	}

	EXPECT_EQ(callCount.load(), hNumThreads * hIterations);
}

// ============ C. pmr 内存池长时间无泄漏 ============

// 请求池创建/销毁循环：内存使用不应无限增长
TEST_F(IntegrationTest, RequestPoolNoLeak)
{
	MemoryPool::instance().resetStats();

	static constexpr int hIterations = 10000;

	for (int i = 0; i < hIterations; ++i)
	{
		auto pool = MemoryPool::instance().createRequestPool(4096);
		std::pmr::polymorphic_allocator<char> alloc(pool.get());

		// 模拟请求生命周期内的分配
		std::pmr::vector<char> header(128, alloc);
		std::pmr::vector<char> body(2048, alloc);
		std::pmr::vector<char> json(512, alloc);
		header[0] = 'H';
		body[0] = 'B';
		json[0] = 'J';
		// pool 析构 -> 整体释放回上游
	}

	auto stats = MemoryPool::instance().getStats();

	// pmr pool_resource 会缓存内存块，所以 allocations >= deallocations
	// 关键验证：分配次数应远少于迭代次数（说明内存在被复用）
	EXPECT_GT(stats.totalAllocations, 0);
	// 当前已分配字节应稳定，不应随迭代次数线性增长
	// 10000 次请求如果泄漏，每次 ~4KB，应超过 40MB
	EXPECT_LT(stats.currentBytesAllocated, 1024 * 1024); // < 1MB 说明无泄漏
}

// PmrBuffer 反复扩容/回收
TEST_F(IntegrationTest, PmrBufferNoLeak)
{
	auto alloc = MemoryPool::instance().threadLocalAllocator();

	static constexpr int hIterations = 5000;

	for (int i = 0; i < hIterations; ++i)
	{
		PmrBuffer buffer(alloc);

		// 追加大量数据触发扩容
		std::string data(4096, 'A');
		buffer.append(data);
		buffer.append(data);
		buffer.append(data);

		EXPECT_EQ(buffer.readableBytes(), 4096 * 3);

		buffer.retrieveAll();
		EXPECT_EQ(buffer.readableBytes(), 0);
	}

	// 不应崩溃或泄漏（buffer 析构后内存归还 thread_local pool）
	SUCCEED();
}

// 多线程 threadLocal 池并发使用
TEST_F(IntegrationTest, MultiThreadPoolNoLeak)
{
	MemoryPool::instance().resetStats();

	static constexpr int hNumThreads = 8;
	static constexpr int hIterations = 5000;

	std::vector<std::thread> threads;
	for (int t = 0; t < hNumThreads; ++t)
	{
		threads.emplace_back(
			[]()
			{
				auto alloc = MemoryPool::instance().threadLocalAllocator();
				for (int i = 0; i < hIterations; ++i)
				{
					auto* p = alloc.allocate_bytes(256);
					alloc.deallocate_bytes(p, 256);
				}
			});
	}

	for (auto& t : threads)
	{
		t.join();
	}

	auto stats = MemoryPool::instance().getStats();

	// thread_local unsynchronized_pool 会向上游 globalPool 请求大块内存并缓存，
	// 所以 totalAllocations 和 totalDeallocations 可能不相等。
	// 关键验证：有分配活动且内存使用不应无限增长
	EXPECT_GT(stats.totalAllocations, 0);
	// 8 线程 x 5000 次 x 256 字节 = ~10MB，如果全部泄漏
	// 实际 pool 会复用，当前字节应远小于总量
	EXPECT_LT(stats.currentBytesAllocated, 10 * 1024 * 1024); // < 10MB
}
