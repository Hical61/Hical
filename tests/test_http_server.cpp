#include "TestHttpClient.h"
#include "core/HttpServer.h"
#include <boost/asio.hpp>
#include <boost/json.hpp>
#include <gtest/gtest.h>
#include <atomic>
#include <cctype>
#include <charconv>
#include <chrono>
#include <string>
#include <thread>

using namespace hical;
using boost::asio::ip::tcp;
using hical::test::httpGet;
using hical::test::httpPost;

// 辅助：启动服务器并等待就绪，返回实际端口
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

// 测试 HttpServer 基本启动
TEST(HttpServerTest, StartAndStop)
{
	HttpServer server(0); // 端口 0 = 系统分配
	server.router().get("/",
						[](const HttpRequest&) -> HttpResponse
						{
							return HttpResponse::ok("hello");
						});

	std::thread serverThread;
	startServerAndWait(server, serverThread);
	EXPECT_TRUE(server.isRunning());

	server.stop();
	serverThread.join();
}

// 测试 HttpServer GET 请求
TEST(HttpServerTest, GetRequest)
{
	HttpServer server(0);

	server.router().get("/api/hello",
						[](const HttpRequest&) -> HttpResponse
						{
							return HttpResponse::ok("Hello from hical!");
						});

	std::thread serverThread;
	uint16_t port = startServerAndWait(server, serverThread);

	auto [status, body] = httpGet("127.0.0.1", port, "/api/hello");
	EXPECT_EQ(status, 200u);
	EXPECT_EQ(body, "Hello from hical!");

	server.stop();
	serverThread.join();
}

// 测试 HttpServer POST 请求
TEST(HttpServerTest, PostRequest)
{
	HttpServer server(0);

	server.router().post("/api/echo",
						 [](const HttpRequest& req) -> HttpResponse
						 {
							 return HttpResponse::ok(req.body());
						 });

	std::thread serverThread;
	uint16_t port = startServerAndWait(server, serverThread);

	auto [status, body] = httpPost("127.0.0.1", port, "/api/echo", "Echo this!");
	EXPECT_EQ(status, 200u);
	EXPECT_EQ(body, "Echo this!");

	server.stop();
	serverThread.join();
}

// 测试 404
TEST(HttpServerTest, NotFound)
{
	HttpServer server(0);

	std::thread serverThread;
	uint16_t port = startServerAndWait(server, serverThread);

	auto [status, body] = httpGet("127.0.0.1", port, "/nonexistent");
	EXPECT_EQ(status, 404u);

	server.stop();
	serverThread.join();
}

// 测试路径参数
TEST(HttpServerTest, PathParam)
{
	HttpServer server(0);

	server.router().get("/users/{id}",
						[](const HttpRequest& req) -> HttpResponse
						{
							return HttpResponse::ok("User " + req.param("id"));
						});

	std::thread serverThread;
	uint16_t port = startServerAndWait(server, serverThread);

	auto [status, body] = httpGet("127.0.0.1", port, "/users/42");
	EXPECT_EQ(status, 200u);
	EXPECT_EQ(body, "User 42");

	server.stop();
	serverThread.join();
}

// 测试中间件
TEST(HttpServerTest, Middleware)
{
	HttpServer server(0);

	server.use(
		[](HttpRequest& req, MiddlewareNext next) -> Awaitable<HttpResponse>
		{
			auto res = co_await next(req);
			res.setHeader("X-Powered-By", "hical");
			co_return res;
		});

	server.router().get("/api/test",
						[](const HttpRequest&) -> HttpResponse
						{
							return HttpResponse::ok("test");
						});

	std::thread serverThread;
	uint16_t port = startServerAndWait(server, serverThread);

	auto result = hical::test::httpGetFull("127.0.0.1", port, "/api/test");
	EXPECT_EQ(result.status, 200u);
	EXPECT_EQ(result.findHeader("X-Powered-By"), "hical");

	server.stop();
	serverThread.join();
}

// 测试 handler 能拿到对端地址（连接层注入到 HttpRequest）
TEST(HttpServerTest, PeerAddr_InjectedIntoRequest_ReturnsClientAddress)
{
	HttpServer server(0);

	server.router().get("/whoami",
						[](const HttpRequest& req) -> HttpResponse
						{
							// 未注入时返回 "invalid"，注入后应返回 "127.0.0.1:<客户端临时端口>"
							if (!req.peerAddr().isValid())
							{
								return HttpResponse::ok("invalid");
							}
							return HttpResponse::ok(req.peerAddr().toIpPort());
						});

	std::thread serverThread;
	uint16_t port = startServerAndWait(server, serverThread);

	auto [status, body] = httpGet("127.0.0.1", port, "/whoami");
	EXPECT_EQ(status, 200u);
	ASSERT_NE(body, "invalid");

	// 客户端走回环地址连接，对端 IP 固定是 127.0.0.1；端口是客户端临时端口，只校验格式
	const std::string prefix = "127.0.0.1:";
	ASSERT_EQ(body.compare(0, prefix.size(), prefix), 0);

	auto colon = body.find(':');
	ASSERT_NE(colon, std::string::npos);
	ASSERT_GT(body.size(), colon + 1);
	for (size_t i = colon + 1; i < body.size(); ++i)
	{
		EXPECT_TRUE(std::isdigit(static_cast<unsigned char>(body[i])));
	}

	server.stop();
	serverThread.join();
}

// 测试 JSON 响应
TEST(HttpServerTest, JsonResponse)
{
	HttpServer server(0);

	server.router().get("/api/status",
						[](const HttpRequest&) -> HttpResponse
						{
							return HttpResponse::json({{"status", "ok"}, {"version", "0.2.0"}});
						});

	std::thread serverThread;
	uint16_t port = startServerAndWait(server, serverThread);

	auto result = hical::test::httpGetFull("127.0.0.1", port, "/api/status");
	EXPECT_EQ(result.status, 200u);
	EXPECT_EQ(result.findHeader("content-type"), "application/json");

	auto json = boost::json::parse(result.body);
	EXPECT_EQ(json.at("status").as_string(), "ok");

	server.stop();
	serverThread.join();
}

// 辅助：只发请求头部（声明 Content-Length 但 body 不发），
// 验证服务端在读过 header 之后是否立刻返回响应而不等待 body。
// 带 2 秒读超时：服务端若消费 body 会一直阻塞等 body，此处 read 超时返回。
struct SkipBodyProbe
{
	unsigned status = 0;
	bool respondedPromptly = false;
};

static SkipBodyProbe postHeaderOnly(uint16_t port, const std::string& target, size_t declaredBodyLen)
{
	using boost::asio::ip::tcp;
	boost::asio::io_context io;
	tcp::socket sock(io);
	sock.connect(tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), port));

	std::string req = "POST " + target
					  + " HTTP/1.1\r\n"
						"Host: 127.0.0.1\r\n"
						"Content-Length: "
					  + std::to_string(declaredBodyLen)
					  + "\r\n"
						"Connection: close\r\n"
						"\r\n";
	boost::asio::write(sock, boost::asio::buffer(req));

	SkipBodyProbe probe;
	std::string buf;
	char tmp[4096];
	boost::system::error_code ec;
	std::atomic<bool> done {false};

	// 非阻塞读：收到任意字节即停；2 秒超时兜底
	boost::asio::steady_timer timer(io);
	timer.expires_after(std::chrono::seconds(2));
	timer.async_wait(
		[&](boost::system::error_code) -> void
		{
			sock.cancel();
			done.store(true);
		});

	sock.async_read_some(boost::asio::buffer(tmp),
						 [&](boost::system::error_code rdEc, std::size_t n) -> void
						 {
							 ec = rdEc;
							 if (n > 0)
							 {
								 buf.append(tmp, n);
							 }
							 done.store(true);
						 });

	while (!done.load())
	{
		io.poll();
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}

	probe.respondedPromptly = (buf.find("\r\n\r\n") != std::string::npos);
	if (probe.respondedPromptly && buf.size() >= 12)
	{
		std::from_chars(buf.data() + 9, buf.data() + 12, probe.status);
	}
	sock.close();
	return probe;
}

// 前置路由匹配：请求不存在的 uri 且带 body，服务端不读 body 直接回 404
TEST(HttpServerTest, MissingRouteSkipsBodyRead)
{
	HttpServer server(0);

	std::thread serverThread;
	uint16_t port = startServerAndWait(server, serverThread);

	// 声明 1MB body 但实际不发，服务端应立刻回 404 而非等待 body 到达
	auto probe = postHeaderOnly(port, "/nonexistent", 1024 * 1024);
	EXPECT_TRUE(probe.respondedPromptly);
	EXPECT_EQ(probe.status, 404u);

	server.stop();
	serverThread.join();
}

// 前置路由匹配：wildcard 路由命中正常，不被误判为 404
TEST(HttpServerTest, WildcardRouteWithBodyNot404)
{
	HttpServer server(0);

	server.router().post("/files/*rest",
						 [](const HttpRequest& req) -> HttpResponse
						 {
							 return HttpResponse::ok("matched:" + std::string(req.body()));
						 });

	std::thread serverThread;
	uint16_t port = startServerAndWait(server, serverThread);

	auto [status, body] = httpPost("127.0.0.1", port, "/files/abc/def.txt", "payload");
	EXPECT_EQ(status, 200u);
	EXPECT_EQ(body, "matched:payload");

	server.stop();
	serverThread.join();
}

// 前置路由匹配：超深路径在读 body 前直接 400
TEST(HttpServerTest, TooDeepPathRejects400)
{
	HttpServer server(0);

	std::thread serverThread;
	uint16_t port = startServerAndWait(server, serverThread);

	// 构造 40 段路径，超过 kMaxPathSegments(32)
	std::string deepPath;
	for (int i = 0; i < 40; ++i)
	{
		deepPath += "/seg";
	}
	auto probe = postHeaderOnly(port, deepPath, 1024);
	EXPECT_TRUE(probe.respondedPromptly);
	EXPECT_EQ(probe.status, 400u);

	server.stop();
	serverThread.join();
}

// 前置路由匹配：405 方法不匹配带 body，读 body 前返回并带 Allow 头
TEST(HttpServerTest, MethodNotAllowedWithBodyReturnsAllow)
{
	HttpServer server(0);

	server.router().get("/only-get",
						[](const HttpRequest&) -> HttpResponse
						{
							return HttpResponse::ok("get only");
						});

	std::thread serverThread;
	uint16_t port = startServerAndWait(server, serverThread);

	// POST 到仅 GET 的路径，声明带 body 但不发 body，服务端应前置 405 而非等 body。
	// 用带超时的非阻塞读，避免服务端若错误消费 body 导致死锁。
	using boost::asio::ip::tcp;
	boost::asio::io_context io;
	tcp::socket sock(io);
	sock.connect(tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), port));

	std::string req = "POST /only-get HTTP/1.1\r\n"
					  "Host: 127.0.0.1\r\n"
					  "Content-Length: 8\r\n"
					  "Connection: close\r\n"
					  "\r\n";
	boost::asio::write(sock, boost::asio::buffer(req));

	std::string raw;
	std::atomic<bool> done {false};
	boost::system::error_code rdEc;
	boost::asio::steady_timer timer(io);
	timer.expires_after(std::chrono::seconds(2));
	timer.async_wait(
		[&](boost::system::error_code) -> void
		{
			sock.cancel();
			done.store(true);
		});
	char tmp[4096];
	sock.async_read_some(boost::asio::buffer(tmp),
						 [&](boost::system::error_code ec, std::size_t n) -> void
						 {
							 rdEc = ec;
							 if (n > 0)
							 {
								 raw.append(tmp, n);
							 }
							 done.store(true);
						 });
	while (!done.load())
	{
		io.poll();
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}

	EXPECT_NE(raw.find(" 405 "), std::string::npos);
	EXPECT_NE(raw.find("Allow:"), std::string::npos);
	EXPECT_NE(raw.find("GET"), std::string::npos);

	sock.close();
	server.stop();
	serverThread.join();
}

// ============ 中间件前置 ============

// 认证中间件在读 body 前短路：拒绝请求不消费 body，handler 也不执行
TEST(HttpServerTest, MiddlewareRejectsBeforeBodyRead)
{
	HttpServer server(0);

	server.use(
		[](HttpRequest&, MiddlewareNext) -> Awaitable<HttpResponse>
		{
			HttpResponse res;
			res.setStatus(HttpStatusCode::hUnauthorized);
			res.setBody("Unauthorized", "text/plain");
			co_return res;
		});
	server.router().post("/protected",
						 [](const HttpRequest& req) -> HttpResponse
						 {
							 return HttpResponse::ok(req.body());
						 });

	std::thread serverThread;
	uint16_t port = startServerAndWait(server, serverThread);

	// 声明 1MB body 但实际不发，服务端应在中间件 401 后立刻响应，不等待 body
	auto probe = postHeaderOnly(port, "/protected", 1024 * 1024);
	EXPECT_TRUE(probe.respondedPromptly);
	EXPECT_EQ(probe.status, 401u);

	server.stop();
	serverThread.join();
}

// 中间件 setAttribute 的上下文在 handler 里可读（同一实例贯穿，不丢）
TEST(HttpServerTest, MiddlewareAttributePassesToHandler)
{
	HttpServer server(0);

	server.use(
		[](HttpRequest& req, MiddlewareNext next) -> Awaitable<HttpResponse>
		{
			req.setAttribute("uid", std::string("42"));
			co_return co_await next(req);
		});
	server.router().get("/me",
						[](const HttpRequest& req) -> HttpResponse
						{
							auto uid = req.getAttribute<std::string>("uid");
							return HttpResponse::ok(uid.value_or("missing"));
						});

	std::thread serverThread;
	uint16_t port = startServerAndWait(server, serverThread);

	auto [status, body] = httpGet("127.0.0.1", port, "/me");
	EXPECT_EQ(status, 200u);
	EXPECT_EQ(body, "42");

	server.stop();
	serverThread.join();
}

// 中间件执行时 body 为空，handler 执行时 body 完整
TEST(HttpServerTest, MiddlewareSeesEmptyBodyHandlerSeesFullBody)
{
	HttpServer server(0);

	server.use(
		[](HttpRequest& req, MiddlewareNext next) -> Awaitable<HttpResponse>
		{
			bool empty = req.body().empty();
			req.setAttribute("mw.body.empty", empty);
			co_return co_await next(req);
		});
	server.router().post("/echo",
						 [](const HttpRequest& req) -> HttpResponse
						 {
							 bool mwSawEmpty = req.getAttribute<bool>("mw.body.empty").value_or(false);
							 return HttpResponse::ok(std::string("mw_empty=") + (mwSawEmpty ? "1" : "0")
													 + " body=" + std::string(req.body()));
						 });

	std::thread serverThread;
	uint16_t port = startServerAndWait(server, serverThread);

	auto [status, body] = httpPost("127.0.0.1", port, "/echo", "payload");
	EXPECT_EQ(status, 200u);
	EXPECT_EQ(body, "mw_empty=1 body=payload");

	server.stop();
	serverThread.join();
}

// 有中间件 + keep-alive：同一连接连发多个不同 path 的请求，走预构建链（cachedChain_ + 请求槽）。
// 每个响应的 body 必须对应各自的 path，不能串（cascade 成上一请求的 resolveResult）。
TEST(HttpServerTest, MiddlewareKeepAliveNoStaleDispatch)
{
	HttpServer server(0);

	// 一个后置中间件，标签化响应，确保链确实被执行到 after
	server.use(
		[](HttpRequest& req, MiddlewareNext next) -> Awaitable<HttpResponse>
		{
			auto res = co_await next(req);
			res.setHeader("X-Mw", "hit");
			co_return res;
		});

	server.router().get("/mw/a",
						[](const HttpRequest&) -> HttpResponse
						{
							return HttpResponse::ok("route-a");
						});
	server.router().get("/mw/b",
						[](const HttpRequest&) -> HttpResponse
						{
							return HttpResponse::ok("route-b");
						});
	server.router().get("/mw/c",
						[](const HttpRequest&) -> HttpResponse
						{
							return HttpResponse::ok("route-c");
						});

	std::thread serverThread;
	uint16_t port = startServerAndWait(server, serverThread);

	auto results =
		hical::test::httpKeepAliveRequests("127.0.0.1",
										   port,
										   {{"GET", "/mw/a"}, {"GET", "/mw/b"}, {"GET", "/mw/c"}, {"GET", "/mw/a"}});

	ASSERT_EQ(results.size(), 4u);
	EXPECT_EQ(results[0].status, 200u);
	EXPECT_EQ(results[1].status, 200u);
	EXPECT_EQ(results[2].status, 200u);
	EXPECT_EQ(results[3].status, 200u);

	EXPECT_EQ(results[0].body, "route-a");
	EXPECT_EQ(results[1].body, "route-b");
	EXPECT_EQ(results[2].body, "route-c");
	EXPECT_EQ(results[3].body, "route-a");

	// 每个响应都经过中间件，证明链（含终端骨架）每个请求都正确执行
	for (const auto& r : results)
	{
		EXPECT_EQ(r.findHeader("X-Mw"), "hit");
	}

	server.stop();
	serverThread.join();
}

// 有中间件 + keep-alive + 带 body：同一连接连发多个 POST 且 body 各不相同，
// 验证 tailHandler 里的 readRequestBody 每请求独立，body 不相互串（stale/UAF 回归）。
TEST(HttpServerTest, MiddlewareKeepAliveNoStaleBody)
{
	HttpServer server(0);

	server.use(
		[](HttpRequest& req, MiddlewareNext next) -> Awaitable<HttpResponse>
		{
			auto res = co_await next(req);
			res.setHeader("X-Mw", "hit");
			co_return res;
		});

	// 回显 body 的自有路由，不同 path 以保证 resolveResult 每请求独立
	for (const char* p : {"/mw/echo1", "/mw/echo2", "/mw/echo3"})
	{
		server.router().post(p,
							 [](const HttpRequest& req) -> HttpResponse
							 {
								 return HttpResponse::ok(std::string(req.path()) + "=" + std::string(req.body()));
							 });
	}

	std::thread serverThread;
	uint16_t port = startServerAndWait(server, serverThread);

	// 手写 keep-alive POST 连发：同一条 socket 依次发 3 个请求，逐个读响应
	using hical::test::detail::readHttpResponse;
	boost::asio::io_context io;
	tcp::socket sock(io);
	sock.connect(tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), port));

	const std::vector<std::pair<std::string, std::string>> requests = {
		{"/mw/echo1", "PING-1"},
		{"/mw/echo2", "PING-2"},
		{"/mw/echo3", "PING-3"},
	};

	std::vector<hical::test::detail::ParsedResponse> responses;
	std::string buf;
	for (size_t i = 0; i < requests.size(); ++i)
	{
		bool isLast = (i == requests.size() - 1);
		const auto& [path, body] = requests[i];
		std::string reqStr = "POST " + path
							 + " HTTP/1.1\r\n"
							   "Host: 127.0.0.1\r\n"
							   "Content-Length: "
							 + std::to_string(body.size())
							 + "\r\n"
							   "Connection: "
							 + (isLast ? "close" : "keep-alive")
							 + "\r\n"
							   "\r\n"
							 + body;
		boost::asio::write(sock, boost::asio::buffer(reqStr));
		responses.push_back(readHttpResponse(sock, buf));
	}

	ASSERT_EQ(responses.size(), 3u);
	EXPECT_EQ(responses[0].body, "/mw/echo1=PING-1");
	EXPECT_EQ(responses[1].body, "/mw/echo2=PING-2");
	EXPECT_EQ(responses[2].body, "/mw/echo3=PING-3");
	for (const auto& r : responses)
	{
		EXPECT_EQ(r.findHeader("X-Mw"), "hit");
	}

	boost::system::error_code ec;
	sock.close(ec);

	server.stop();
	serverThread.join();
}
