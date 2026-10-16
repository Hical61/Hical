/**
 * @file test_middleware_body.cpp
 * @brief 中间件 body 声明接口与 body 门插入的行为断言测试
 * 前半是任务 1 的接口层：useBody 能注册、服务器能起停、没声明 needsBody 时行为与改动前一致。
 * 后半是任务 2 的链构建：声明了 needsBody 就在第一个这类条目前插一道 body 门，
 * 用一个假的 readBody 记录自己被调的时机，就能断言门插在哪、多个声明只插一道、
 * 读失败时门短路但外层后置仍跑。真实读 body 由 handleSession 挂上去（任务 3），
 * 这里手工构造 ctx，不依赖 handleSession。
 */

#include "TestHttpClient.h"
#include "core/Middleware.h"
#include "core/HttpServer.h"
#include "core/RouteGroup.h"
#include "core/RequestDispatch.h"
#include "core/WebSocket.h"
#include "core/SseSession.h"
#include "core/Coroutine.h"
#include "asio/AsioEventLoop.h"
#include <boost/asio.hpp>
#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace hical;
using boost::asio::ip::tcp;
using hical::test::httpGet;

namespace
{

	// 在事件循环里跑一个协程拿回结果（同 test_middleware.cpp 的驱动方式）
	template <typename F>
	auto runCoroutine(F&& f)
	{
		using ReturnType = typename std::invoke_result_t<F>::value_type;

		boost::asio::io_context ioCtx;
		std::optional<ReturnType> result;

		coSpawn(ioCtx,
				[&]() -> Awaitable<void>
				{
					result = co_await f();
				});

		ioCtx.run();
		return result;
	}

	// 启动服务器并等它就绪，返回实际端口
	uint16_t startServerAndWait(HttpServer& server, std::thread& serverThread)
	{
		serverThread = std::thread(
			[&server]()
			{
				server.start();
			});

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

} // namespace

// ============ 注册接口 ============

TEST(MiddlewareBodyTest, UseBodyAsync_RegistersEntry)
{
	MiddlewarePipeline pipeline;
	pipeline.useBody(
		[](HttpRequest& req, MiddlewareNext next) -> Awaitable<HttpResponse>
		{
			co_return co_await next(req);
		});
	EXPECT_EQ(pipeline.size(), 1);
}

TEST(MiddlewareBodyTest, UseBodyNamedAsync_RegistersEntry)
{
	MiddlewarePipeline pipeline;
	pipeline.useBody("sign",
					 [](HttpRequest& req, MiddlewareNext next) -> Awaitable<HttpResponse>
					 {
						 co_return co_await next(req);
					 });
	EXPECT_EQ(pipeline.size(), 1);
}

TEST(MiddlewareBodyTest, UseBodySync_RegistersEntry)
{
	MiddlewarePipeline pipeline;
	pipeline.useBody(
		[](HttpRequest&) -> SyncMiddlewareResult
		{
			return std::nullopt;
		});
	EXPECT_EQ(pipeline.size(), 1);
}

TEST(MiddlewareBodyTest, UseBodyNamedSync_RegistersEntry)
{
	MiddlewarePipeline pipeline;
	pipeline.useBody("sign",
					 [](HttpRequest&) -> SyncMiddlewareResult
					 {
						 return std::nullopt;
					 });
	EXPECT_EQ(pipeline.size(), 1);
}

// build() 之后不许再加中间件这条线，useBody 也得守住
TEST(MiddlewareBodyTest, UseBodyAfterBuild_Throws)
{
	MiddlewarePipeline pipeline;
	pipeline.build(
		[](HttpRequest&) -> Awaitable<HttpResponse>
		{
			co_return HttpResponse::ok("final");
		});

	EXPECT_THROW(pipeline.useBody(
					 [](HttpRequest& req, MiddlewareNext next) -> Awaitable<HttpResponse>
					 {
						 co_return co_await next(req);
					 }),
				 std::logic_error);
}

// ============ 默认行为不变 ============

// 没声明 needsBody 的普通中间件链，执行结果与改动前一致
TEST(MiddlewareBodyTest, PlainChainWithoutUseBody_BehaviorUnchanged)
{
	MiddlewarePipeline pipeline;
	pipeline.use(
		[](HttpRequest& req, MiddlewareNext next) -> Awaitable<HttpResponse>
		{
			auto res = co_await next(req);
			res.setHeader("X-Plain", "1");
			co_return res;
		});

	HttpRequest req;
	req.setMethod(HttpMethod::hGet);
	req.setTarget("/plain");

	auto result = runCoroutine(
		[&]()
		{
			return pipeline.execute(req,
									[](HttpRequest&) -> Awaitable<HttpResponse>
									{
										co_return HttpResponse::ok("plain");
									});
		});

	ASSERT_TRUE(result.has_value());
	EXPECT_EQ(result->body(), "plain");
	EXPECT_EQ(result->header("X-Plain"), "1");
}

// ============ 服务器起停 ============

// useBody 注册后服务器照常起停、普通 GET 请求正常返回
TEST(MiddlewareBodyTest, ServerWithUseBody_StartsStopsAndServesPlainGet)
{
	HttpServer server(0);
	server.useBody(
		[](HttpRequest& req, MiddlewareNext next) -> Awaitable<HttpResponse>
		{
			co_return co_await next(req);
		});
	server.router().get("/ping",
						[](const HttpRequest&) -> Awaitable<HttpResponse>
						{
							co_return HttpResponse::ok("pong");
						});

	std::thread serverThread;
	uint16_t port = startServerAndWait(server, serverThread);
	ASSERT_NE(port, 0);

	auto [status, body] = httpGet("127.0.0.1", port, "/ping");
	EXPECT_EQ(status, 200u);
	EXPECT_EQ(body, "pong");

	server.stop();
	serverThread.join();
}

// 组级 useBody 同样能注册，组内路由照常起停并正常返回
TEST(MiddlewareBodyTest, RouteGroupUseBody_StartsStopsAndServesPlainGet)
{
	HttpServer server(0);
	auto api = server.router().group("/api");
	api.useBody(
		[](HttpRequest& req, MiddlewareNext next) -> Awaitable<HttpResponse>
		{
			co_return co_await next(req);
		});
	api.get("/hello",
			[](const HttpRequest&) -> Awaitable<HttpResponse>
			{
				co_return HttpResponse::ok("hi");
			});

	std::thread serverThread;
	uint16_t port = startServerAndWait(server, serverThread);
	ASSERT_NE(port, 0);

	auto [status, body] = httpGet("127.0.0.1", port, "/api/hello");
	EXPECT_EQ(status, 200u);
	EXPECT_EQ(body, "hi");

	server.stop();
	serverThread.join();
}

// ============ body 门插入位置（任务 2） ============
//
// entries_ / cachedChain_ 都是私有的，链是 std::function 没法直接断言结构，
// 所以用行为断言：塞一个记名字的 readBody，看它被调的时机对应到链里的哪个位置。

// 声明 needsBody 时，门插在第一个 needsBody 条目之前——先跑完前面的普通中间件，
// 再读 body，最后才进声明者。慢 body 早读早亏，所以门尽量靠后。
TEST(MiddlewareBodyTest, BodyGate_InsertedBeforeFirstNeedsBodyEntry)
{
	std::vector<std::string> order;

	MiddlewarePipeline pipeline;
	pipeline.use(
		[&order](HttpRequest& req, MiddlewareNext next) -> Awaitable<HttpResponse>
		{
			order.push_back("A");
			co_return co_await next(req);
		});
	pipeline.useBody(
		[&order](HttpRequest& req, MiddlewareNext next) -> Awaitable<HttpResponse>
		{
			order.push_back("B");
			co_return co_await next(req);
		});

	pipeline.build(
		[&order](HttpRequest&) -> Awaitable<HttpResponse>
		{
			order.push_back("HANDLER");
			co_return HttpResponse::ok("");
		});

	HttpRequest req;
	detail::RequestDispatchContext ctx;
	ctx.bodyObj = &order;
	ctx.readBody = [](void* obj) -> Awaitable<std::optional<HttpResponse>>
	{
		static_cast<std::vector<std::string>*>(obj)->push_back("BODY");
		co_return std::nullopt; // 读成功
	};
	detail::RequestDispatchContext::attach(req, &ctx);

	auto result = runCoroutine(
		[&]()
		{
			return pipeline.execute(req);
		});

	ASSERT_TRUE(result.has_value());
	EXPECT_EQ(order, (std::vector<std::string> {"A", "BODY", "B", "HANDLER"}));
}

// 多个 needsBody 只插一道门，body 只读一次
TEST(MiddlewareBodyTest, MultipleBodyEntries_OnlyOneGate)
{
	std::vector<std::string> order;
	int bodyReadCount = 0;

	MiddlewarePipeline pipeline;
	pipeline.useBody(
		[&order](HttpRequest& req, MiddlewareNext next) -> Awaitable<HttpResponse>
		{
			order.push_back("B1");
			co_return co_await next(req);
		});
	pipeline.useBody(
		[&order](HttpRequest& req, MiddlewareNext next) -> Awaitable<HttpResponse>
		{
			order.push_back("B2");
			co_return co_await next(req);
		});

	pipeline.build(
		[&order](HttpRequest&) -> Awaitable<HttpResponse>
		{
			order.push_back("HANDLER");
			co_return HttpResponse::ok("");
		});

	HttpRequest req;
	detail::RequestDispatchContext ctx;
	ctx.bodyObj = &bodyReadCount;
	ctx.readBody = [](void* obj) -> Awaitable<std::optional<HttpResponse>>
	{
		++*static_cast<int*>(obj);
		co_return std::nullopt;
	};
	detail::RequestDispatchContext::attach(req, &ctx);

	auto result = runCoroutine(
		[&]()
		{
			return pipeline.execute(req);
		});

	ASSERT_TRUE(result.has_value());
	EXPECT_EQ(order, (std::vector<std::string> {"B1", "B2", "HANDLER"}));
	EXPECT_EQ(bodyReadCount, 1); // 门只插一道，body 不重复读
}

// 读 body 失败：门短路返回错误响应，声明者和 handler 都不执行，
// 但外层普通中间件的后置逻辑仍要跑（洋葱模型）
TEST(MiddlewareBodyTest, BodyGate_ReadFailure_ShortCircuitsButOuterAfterStillRuns)
{
	std::vector<std::string> order;

	MiddlewarePipeline pipeline;
	pipeline.use(
		[&order](HttpRequest& req, MiddlewareNext next) -> Awaitable<HttpResponse>
		{
			order.push_back("A-pre");
			auto res = co_await next(req);
			order.push_back("A-post");
			co_return res;
		});
	pipeline.useBody(
		[&order](HttpRequest& req, MiddlewareNext next) -> Awaitable<HttpResponse>
		{
			order.push_back("B");
			co_return co_await next(req);
		});

	pipeline.build(
		[&order](HttpRequest&) -> Awaitable<HttpResponse>
		{
			order.push_back("HANDLER");
			co_return HttpResponse::ok("");
		});

	HttpRequest req;
	detail::RequestDispatchContext ctx;
	ctx.bodyObj = &order;
	ctx.readBody = [](void* obj) -> Awaitable<std::optional<HttpResponse>>
	{
		static_cast<std::vector<std::string>*>(obj)->push_back("BODY");
		co_return HttpResponse::badRequest("body too large"); // 有值 = 读失败
	};
	detail::RequestDispatchContext::attach(req, &ctx);

	auto result = runCoroutine(
		[&]()
		{
			return pipeline.execute(req);
		});

	ASSERT_TRUE(result.has_value());
	EXPECT_EQ(result->statusCode(), HttpStatusCode::hBadRequest);
	EXPECT_EQ(order, (std::vector<std::string> {"A-pre", "BODY", "A-post"}));
}

// 完全不用 useBody：一个字节都不插，readBody 一次都不该被调到
TEST(MiddlewareBodyTest, NoBodyDeclaration_NoGateInserted)
{
	std::vector<std::string> order;
	bool bodyRead = false;

	MiddlewarePipeline pipeline;
	pipeline.use(
		[&order](HttpRequest& req, MiddlewareNext next) -> Awaitable<HttpResponse>
		{
			order.push_back("A");
			co_return co_await next(req);
		});

	pipeline.build(
		[&order](HttpRequest&) -> Awaitable<HttpResponse>
		{
			order.push_back("HANDLER");
			co_return HttpResponse::ok("");
		});

	HttpRequest req;
	detail::RequestDispatchContext ctx;
	ctx.bodyObj = &bodyRead;
	ctx.readBody = [](void* obj) -> Awaitable<std::optional<HttpResponse>>
	{
		*static_cast<bool*>(obj) = true;
		co_return std::nullopt;
	};
	detail::RequestDispatchContext::attach(req, &ctx);

	auto result = runCoroutine(
		[&]()
		{
			return pipeline.execute(req);
		});

	ASSERT_TRUE(result.has_value());
	EXPECT_EQ(order, (std::vector<std::string> {"A", "HANDLER"}));
	EXPECT_FALSE(bodyRead);
}

// buildFor() 走的是另一条链构建路径（WS/SSE 用），门同样要插
TEST(MiddlewareBodyTest, BuildFor_AlsoInsertsGate)
{
	std::vector<std::string> order;

	MiddlewarePipeline pipeline;
	pipeline.use(
		[&order](HttpRequest& req, MiddlewareNext next) -> Awaitable<HttpResponse>
		{
			order.push_back("A");
			co_return co_await next(req);
		});
	pipeline.useBody(
		[&order](HttpRequest& req, MiddlewareNext next) -> Awaitable<HttpResponse>
		{
			order.push_back("B");
			co_return co_await next(req);
		});

	auto chain = pipeline.buildFor(
		[&order](HttpRequest&) -> Awaitable<HttpResponse>
		{
			order.push_back("HANDLER");
			co_return HttpResponse::ok("");
		});

	HttpRequest req;
	detail::RequestDispatchContext ctx;
	ctx.bodyObj = &order;
	ctx.readBody = [](void* obj) -> Awaitable<std::optional<HttpResponse>>
	{
		static_cast<std::vector<std::string>*>(obj)->push_back("BODY");
		co_return std::nullopt;
	};
	detail::RequestDispatchContext::attach(req, &ctx);

	auto result = runCoroutine(
		[&]()
		{
			return chain(req);
		});

	ASSERT_TRUE(result.has_value());
	EXPECT_EQ(order, (std::vector<std::string> {"A", "BODY", "B", "HANDLER"}));
}

// WS/SSE 那条链的请求不塞槽，门取不到 ctx 时直接放行 next，不读 body 也不崩
TEST(MiddlewareBodyTest, BodyGate_NullContext_PassesThrough)
{
	std::vector<std::string> order;

	MiddlewarePipeline pipeline;
	pipeline.useBody(
		[&order](HttpRequest& req, MiddlewareNext next) -> Awaitable<HttpResponse>
		{
			order.push_back("B");
			co_return co_await next(req);
		});

	pipeline.build(
		[&order](HttpRequest&) -> Awaitable<HttpResponse>
		{
			order.push_back("HANDLER");
			co_return HttpResponse::ok("");
		});

	HttpRequest req; // 没 attach，内部槽是 nullptr

	auto result = runCoroutine(
		[&]()
		{
			return pipeline.execute(req);
		});

	ASSERT_TRUE(result.has_value());
	EXPECT_EQ(order, (std::vector<std::string> {"B", "HANDLER"}));
}

// ============ handleSession 接管读 body（任务 3） ============
//
// 上面那批用假的 readBody 断链结构，这批干脆走真实 HTTP：handleSession 把真正的读 body
// 入口挂进 ctx，门一调就真把 body 读进 req。断言点全落在「中间件/handler 拿到的 body 对不对」。

// 声明了 needsBody 的中间件能读到完整 body，而它前面的普通中间件此时 body 还是空的。
// 中间件把各自看到的 body 存进 req attribute，handler 再原样吐进响应体，一个请求就能验两边。
TEST(MiddlewareBodyTest, UseBodyMiddleware_ReadsFullBody_PlainBeforeItSeesEmpty)
{
	HttpServer server(0);
	server.use(
		[](HttpRequest& req, MiddlewareNext next) -> Awaitable<HttpResponse>
		{
			req.setAttribute("plainBody", std::string(req.body()));
			co_return co_await next(req);
		});
	server.useBody(
		[](HttpRequest& req, MiddlewareNext next) -> Awaitable<HttpResponse>
		{
			req.setAttribute("gatedBody", std::string(req.body()));
			co_return co_await next(req);
		});
	server.router().post("/echo",
						 [](const HttpRequest& req) -> Awaitable<HttpResponse>
						 {
							 auto plain = req.getAttribute<std::string>("plainBody").value_or("<none>");
							 auto gated = req.getAttribute<std::string>("gatedBody").value_or("<none>");
							 co_return HttpResponse::ok("plain[" + plain + "] gated[" + gated + "]");
						 });

	std::thread serverThread;
	uint16_t port = startServerAndWait(server, serverThread);
	ASSERT_NE(port, 0);

	hical::test::HttpTestClient client(port);
	auto res = client.post("/echo", "hello-sign");
	EXPECT_EQ(res.status(), 200u);
	// 门前中间件 body 为空，门后声明者拿到完整 body
	EXPECT_EQ(res.body(), "plain[] gated[hello-sign]");

	server.stop();
	serverThread.join();
}

// 组级 useBody 的局部链挂在路由 handler 上，它的门要等 dispatchResolved 才跑到——那时
// 全局路径（这里挂了个普通中间件，让 handleSession 走中间件路径、把 ctx 塞进 req）早把
// body 读过了。确认这道晚到的门不会把 body 读第二遍（读重会把 body 弄脏）。
TEST(MiddlewareBodyTest, RouteGroupUseBody_PostBody_NotReadTwice)
{
	HttpServer server(0);
	// 全局挂个普通中间件：把 handleSession 逼进「有中间件」分支，ctx 才会被塞进 req
	server.use(
		[](HttpRequest& req, MiddlewareNext next) -> Awaitable<HttpResponse>
		{
			co_return co_await next(req);
		});

	auto api = server.router().group("/api");
	api.useBody(
		[](HttpRequest& req, MiddlewareNext next) -> Awaitable<HttpResponse>
		{
			req.setAttribute("groupBody", std::string(req.body()));
			co_return co_await next(req);
		});
	api.post("/echo",
			 [](const HttpRequest& req) -> Awaitable<HttpResponse>
			 {
				 auto g = req.getAttribute<std::string>("groupBody").value_or("<none>");
				 co_return HttpResponse::ok("g[" + g + "] h[" + req.body() + "]");
			 });

	std::thread serverThread;
	uint16_t port = startServerAndWait(server, serverThread);
	ASSERT_NE(port, 0);

	hical::test::HttpTestClient client(port);
	auto res = client.post("/api/echo", "group-body");
	EXPECT_EQ(res.status(), 200u);
	EXPECT_EQ(res.body(), "g[group-body] h[group-body]");

	server.stop();
	serverThread.join();
}

// 还拿得到完整 body。这条路径不能被「body 交给门读」的改动饿着。
TEST(MiddlewareBodyTest, PlainPostNoMiddleware_HandlerGetsFullBody)
{
	HttpServer server(0);
	server.router().post("/echo",
						 [](const HttpRequest& req) -> Awaitable<HttpResponse>
						 {
							 co_return HttpResponse::ok(req.body());
						 });

	std::thread serverThread;
	uint16_t port = startServerAndWait(server, serverThread);
	ASSERT_NE(port, 0);

	hical::test::HttpTestClient client(port);
	auto res = client.post("/echo", "regression-body");
	EXPECT_EQ(res.status(), 200u);
	EXPECT_EQ(res.body(), "regression-body");

	server.stop();
	serverThread.join();
}

// 同上，但链上挂了个普通中间件（有中间件路径）——走的是 tail 兜底读那条线，
// 没声明 needsBody 时行为与改动前逐字节一致。
TEST(MiddlewareBodyTest, PlainPostWithPlainMiddleware_HandlerGetsFullBody)
{
	HttpServer server(0);
	server.use(
		[](HttpRequest& req, MiddlewareNext next) -> Awaitable<HttpResponse>
		{
			co_return co_await next(req);
		});
	server.router().post("/echo",
						 [](const HttpRequest& req) -> Awaitable<HttpResponse>
						 {
							 co_return HttpResponse::ok(req.body());
						 });

	std::thread serverThread;
	uint16_t port = startServerAndWait(server, serverThread);
	ASSERT_NE(port, 0);

	hical::test::HttpTestClient client(port);
	auto res = client.post("/echo", "middleware-body");
	EXPECT_EQ(res.status(), 200u);
	EXPECT_EQ(res.body(), "middleware-body");

	server.stop();
	serverThread.join();
}

// 带 body 的无效请求在阶段 B.5 就被拒，body 一字不读。
// 把 maxBodySize 压到 8 字节，再打一个 64 字节的 body：要是服务器先读 body 会是 413，
// 实际能拿到 404/405 就反证了「没读」。
TEST(MiddlewareBodyTest, BodyRequestToUnmatchedPath_RejectedWithoutReadingBody)
{
	HttpServer server(0);
	server.setMaxBodySize(8);
	server.router().post("/exists",
						 [](const HttpRequest&) -> Awaitable<HttpResponse>
						 {
							 co_return HttpResponse::ok("ok");
						 });
	server.router().get("/only-get",
						[](const HttpRequest&) -> Awaitable<HttpResponse>
						{
							co_return HttpResponse::ok("got");
						});

	std::thread serverThread;
	uint16_t port = startServerAndWait(server, serverThread);
	ASSERT_NE(port, 0);

	hical::test::HttpTestClient client(port);
	std::string bigBody(64, 'x');

	// 路径不存在：404，且没读 body（否则超 8 字节会 413）
	auto notFound = client.post("/nope", bigBody);
	EXPECT_EQ(notFound.status(), 404u);

	// 路径存在但方法不对：405，同样没读 body
	auto notAllowed = client.post("/only-get", bigBody);
	EXPECT_EQ(notAllowed.status(), 405u);

	server.stop();
	serverThread.join();
}

// 读 body 失败（超 maxBodySize）时门短路：门后的声明者和 handler 都不执行，
// 门外普通中间件的后置仍要跑（洋葱语义）。
TEST(MiddlewareBodyTest, BodyTooLarge_GateShortCircuits_LaterHandlersSkipped)
{
	HttpServer server(0);
	server.setMaxBodySize(8);
	server.use(
		[](HttpRequest& req, MiddlewareNext next) -> Awaitable<HttpResponse>
		{
			auto res = co_await next(req);
			res.setHeader("X-Plain", "1");
			co_return res;
		});
	server.useBody(
		[](HttpRequest& req, MiddlewareNext next) -> Awaitable<HttpResponse>
		{
			auto res = co_await next(req);
			res.setHeader("X-Gated", "1");
			co_return res;
		});
	server.router().post("/upload",
						 [](const HttpRequest&) -> Awaitable<HttpResponse>
						 {
							 HttpResponse r = HttpResponse::ok("handler");
							 r.setHeader("X-Handler", "1");
							 co_return r;
						 });

	std::thread serverThread;
	uint16_t port = startServerAndWait(server, serverThread);
	ASSERT_NE(port, 0);

	hical::test::HttpTestClient client(port);
	auto res = client.post("/upload", std::string(64, 'x'));
	EXPECT_EQ(res.status(), 413u);
	EXPECT_EQ(res.headerValue("X-Plain"), "1");  // 门外中间件后置照跑
	EXPECT_EQ(res.headerValue("X-Gated"), "");   // 门后的声明者没执行
	EXPECT_EQ(res.headerValue("X-Handler"), ""); // handler 没执行

	server.stop();
	serverThread.join();
}

// ============ needsBody 与 Sync 中间件混排（任务 4 补齐） ============
//
// 门是 hAsync，插进链里会把本来能合并的连续 Sync 截成两段。这批用例钉住两件事：
// ① 混排时每个 Sync 中间件的相对位置没被打乱；② 相邻 Sync 被门截断后不再共用一个协程帧。

// 普通 Sync（前+后）与普通 async、needsBody 声明者交错排布时，各自的进出时机都不变。
// 门落在 P 与 B 之间；A 在最外层所以它的后置最后跑，C 在声明者 B 之后才进。
TEST(MiddlewareBodyTest, NeedsBodyMixedWithSync_KeepsRelativeOrder)
{
	std::vector<std::string> order;

	MiddlewarePipeline pipeline;
	// A：Sync 前置+后置，整个链最外层
	pipeline.use(
		[&order](HttpRequest&) -> SyncMiddlewareResult
		{
			order.push_back("A-pre");
			return std::nullopt;
		},
		[&order](HttpRequest&, HttpResponse&)
		{
			order.push_back("A-post");
		});
	// P：普通 async 中间件
	pipeline.use(
		[&order](HttpRequest& req, MiddlewareNext next) -> Awaitable<HttpResponse>
		{
			order.push_back("P");
			co_return co_await next(req);
		});
	// B：声明 needsBody，门插在它前面
	pipeline.useBody(
		[&order](HttpRequest& req, MiddlewareNext next) -> Awaitable<HttpResponse>
		{
			order.push_back("B");
			co_return co_await next(req);
		});
	// C：Sync 前置+后置，排在声明者之后
	pipeline.use(
		[&order](HttpRequest&) -> SyncMiddlewareResult
		{
			order.push_back("C-pre");
			return std::nullopt;
		},
		[&order](HttpRequest&, HttpResponse&)
		{
			order.push_back("C-post");
		});

	pipeline.build(
		[&order](HttpRequest&) -> Awaitable<HttpResponse>
		{
			order.push_back("HANDLER");
			co_return HttpResponse::ok("");
		});

	HttpRequest req;
	detail::RequestDispatchContext ctx;
	ctx.bodyObj = &order;
	ctx.readBody = [](void* obj) -> Awaitable<std::optional<HttpResponse>>
	{
		static_cast<std::vector<std::string>*>(obj)->push_back("BODY");
		co_return std::nullopt;
	};
	detail::RequestDispatchContext::attach(req, &ctx);

	auto result = runCoroutine(
		[&]()
		{
			return pipeline.execute(req);
		});

	ASSERT_TRUE(result.has_value());
	EXPECT_EQ(order, (std::vector<std::string> {"A-pre", "P", "BODY", "B", "C-pre", "HANDLER", "C-post", "A-post"}));
}

// 两个相邻 Sync 条目里后一个是 useBody(Sync)：门插在中间把本来能合并成一个协程帧的两段截开。
// "BODY" 出现在 A-pre 与 B-pre 之间，正是「门确实插进来了、且打断了合并」的证据——
// 要是门没插，A/B 会合成一帧，B-pre 会紧跟在 A-pre 后面，中间不会冒出 BODY。
TEST(MiddlewareBodyTest, NeedsBodySync_GateSplitsAdjacentSyncMerge)
{
	std::vector<std::string> order;

	MiddlewarePipeline pipeline;
	pipeline.use(
		[&order](HttpRequest&) -> SyncMiddlewareResult
		{
			order.push_back("A-pre");
			return std::nullopt;
		},
		[&order](HttpRequest&, HttpResponse&)
		{
			order.push_back("A-post");
		});
	pipeline.useBody(
		[&order](HttpRequest&) -> SyncMiddlewareResult
		{
			order.push_back("B-pre");
			return std::nullopt;
		},
		[&order](HttpRequest&, HttpResponse&)
		{
			order.push_back("B-post");
		});

	pipeline.build(
		[&order](HttpRequest&) -> Awaitable<HttpResponse>
		{
			order.push_back("HANDLER");
			co_return HttpResponse::ok("");
		});

	HttpRequest req;
	detail::RequestDispatchContext ctx;
	ctx.bodyObj = &order;
	ctx.readBody = [](void* obj) -> Awaitable<std::optional<HttpResponse>>
	{
		static_cast<std::vector<std::string>*>(obj)->push_back("BODY");
		co_return std::nullopt;
	};
	detail::RequestDispatchContext::attach(req, &ctx);

	auto result = runCoroutine(
		[&]()
		{
			return pipeline.execute(req);
		});

	ASSERT_TRUE(result.has_value());
	EXPECT_EQ(order, (std::vector<std::string> {"A-pre", "BODY", "B-pre", "HANDLER", "B-post", "A-post"}));
}

// ============ 无中间件路径（任务 4 补齐） ============
//
// 一个中间件都不注册时，handleSession 走的是「无中间件」那另一条分支，压根不经过 body 门。
// 这条分支要保证照常工作：GET 正常，带 body 的 POST 仍拿得到完整 body。

TEST(MiddlewareBodyTest, NoMiddleware_ServerServesGetAndPostBody)
{
	HttpServer server(0);
	server.router().get("/ping",
						[](const HttpRequest&) -> Awaitable<HttpResponse>
						{
							co_return HttpResponse::ok("pong");
						});
	server.router().post("/echo",
						 [](const HttpRequest& req) -> Awaitable<HttpResponse>
						 {
							 co_return HttpResponse::ok(std::string(req.body()));
						 });

	std::thread serverThread;
	uint16_t port = startServerAndWait(server, serverThread);
	ASSERT_NE(port, 0);

	auto get = httpGet("127.0.0.1", port, "/ping");
	EXPECT_EQ(get.first, 200u);
	EXPECT_EQ(get.second, "pong");

	hical::test::HttpTestClient client(port);
	auto post = client.post("/echo", "no-mw-body");
	EXPECT_EQ(post.status(), 200u);
	EXPECT_EQ(post.body(), "no-mw-body");

	server.stop();
	serverThread.join();
}

// ============ WS / SSE 升级路径不受门影响（任务 4 补齐） ============
//
// WS/SSE 升级走的是 buildFor() 出来的 wsMiddlewareChain_，它的 req 不塞分发上下文。
// 就算全局声明了 useBody（链上带门），门取不到 ctx 也只能直接放行——升级照样完成。
// 顺带用计数器确认 wsMiddlewareChain_ 里的中间件确实跑了，门没把整条链吞掉。

TEST(MiddlewareBodyTest, WsUpgrade_WithGlobalUseBody_GatePassesThroughAndEchoWorks)
{
	HttpServer server(0);
	std::atomic<int> middlewareRuns {0};
	server.useBody(
		[&middlewareRuns](HttpRequest& req, MiddlewareNext next) -> Awaitable<HttpResponse>
		{
			middlewareRuns.fetch_add(1);
			co_return co_await next(req);
		});
	server.router().ws("/ws/echo",
					   [](const std::string& msg, WebSocketSession& session) -> Awaitable<void>
					   {
						   co_await session.send(msg);
					   });

	std::thread serverThread;
	uint16_t port = startServerAndWait(server, serverThread);
	ASSERT_NE(port, 0);

	boost::asio::io_context ioCtx;
	hical::test::TestWsClient wsClient(ioCtx);
	wsClient.connect("127.0.0.1", port, "/ws/echo");

	// 握手已完成：wsMiddlewareChain_（含 body 门）跑过一遍，声明者中间件照常执行
	EXPECT_GE(middlewareRuns.load(), 1);

	wsClient.write("hello-ws");
	EXPECT_EQ(wsClient.read(), "hello-ws");

	wsClient.close();
	server.stop();
	serverThread.join();
}

TEST(MiddlewareBodyTest, SseUpgrade_WithGlobalUseBody_GatePassesThroughAndStreams)
{
	HttpServer server(0);
	server.setIdleTimeout(0);
	std::atomic<int> middlewareRuns {0};
	server.useBody(
		[&middlewareRuns](HttpRequest& req, MiddlewareNext next) -> Awaitable<HttpResponse>
		{
			middlewareRuns.fetch_add(1);
			co_return co_await next(req);
		});
	server.router().sse("/events",
						[](std::shared_ptr<SseSession> session) -> Awaitable<void>
						{
							co_await session->sendData("hello-sse");
							co_await session->close();
						});

	std::thread serverThread;
	uint16_t port = startServerAndWait(server, serverThread);
	ASSERT_NE(port, 0);

	boost::asio::io_context ioCtx;
	tcp::socket sock(ioCtx);
	sock.connect(tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), port));

	std::string req = "GET /events HTTP/1.1\r\n"
					  "Host: 127.0.0.1\r\n"
					  "Connection: close\r\n"
					  "\r\n";
	boost::asio::write(sock, boost::asio::buffer(req));

	// 非阻塞轮询：凑够断言要用的字节就收工，不赌服务端何时真正断开连接
	// （SSE 会话的断开时机不受客户端控制）。deadline 兜底，绝不会挂死。
	sock.non_blocking(true);
	std::string raw;
	char buf[4096];
	auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (std::chrono::steady_clock::now() < deadline)
	{
		boost::system::error_code readEc;
		auto n = sock.read_some(boost::asio::buffer(buf), readEc);
		if (!readEc)
		{
			raw.append(buf, n);
			if (raw.find("hello-sse") != std::string::npos)
			{
				break;
			}
		}
		else if (readEc == boost::asio::error::would_block)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
		else
		{
			break; // EOF / 连接重置
		}
	}
	sock.close();

	EXPECT_GE(middlewareRuns.load(), 1);
	EXPECT_NE(raw.find("200 OK"), std::string::npos) << "raw: " << raw;
	EXPECT_NE(raw.find("Content-Type: text/event-stream"), std::string::npos) << "raw: " << raw;
	EXPECT_NE(raw.find("hello-sse"), std::string::npos) << "raw: " << raw;

	server.stop();
	serverThread.join();
}

// ============ chunked 请求的 req.body() 语义（任务 4 补齐） ============
//
// 防改回归：chunked 请求的 req.body() 必须是 phr_decode_chunked 解出来的实体载荷，
// chunk 帧头（长度行、帧尾 CRLF、终止帧）都已被剥掉。将来谁想动 chunked 行为会先撞到这条。

namespace
{
	// 裸 socket 手写一个 chunked 传输的 POST（TestHttpClient 只会发 Content-Length），
	// 返回解析后的完整响应。
	hical::test::detail::ParsedResponse rawChunkedPost(uint16_t port,
													   const std::string& target,
													   const std::string& chunkedBody)
	{
		boost::asio::io_context ioCtx;
		tcp::socket sock(ioCtx);
		sock.connect(tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), port));

		std::string req = "POST " + target
						  + " HTTP/1.1\r\n"
							"Host: 127.0.0.1\r\n"
							"Content-Type: text/plain\r\n"
							"Transfer-Encoding: chunked\r\n"
							"Connection: close\r\n"
							"\r\n"
						  + chunkedBody;
		boost::asio::write(sock, boost::asio::buffer(req));

		std::string residual;
		auto resp = hical::test::detail::readHttpResponse(sock, residual);
		boost::system::error_code ec;
		sock.close(ec);
		return resp;
	}
} // namespace

// 走 needsBody 门路径的 chunked 请求：门里 -> readRequestBody 的 chunked 分支解码，
// 声明者中间件和 handler 拿到的 req.body() 都该是解码后的实体载荷，绝不能带 chunk 帧头。
TEST(MiddlewareBodyTest, ChunkedPost_GateReadsDecodedPayload)
{
	HttpServer server(0);
	server.useBody(
		[](HttpRequest& req, MiddlewareNext next) -> Awaitable<HttpResponse>
		{
			req.setAttribute("gated", std::string(req.body()));
			co_return co_await next(req);
		});
	server.router().post("/echo-chunk",
						 [](const HttpRequest& req) -> Awaitable<HttpResponse>
						 {
							 auto gated = req.getAttribute<std::string>("gated").value_or("<none>");
							 co_return HttpResponse::ok("gated[" + gated + "] handler[" + std::string(req.body())
														+ "]");
						 });

	std::thread serverThread;
	uint16_t port = startServerAndWait(server, serverThread);
	ASSERT_NE(port, 0);

	// "Hello, "(7) + "chunked"(7) + " world"(6)，解码后应为 "Hello, chunked world"
	const std::string chunked = "7\r\nHello, \r\n7\r\nchunked\r\n6\r\n world\r\n0\r\n\r\n";
	auto resp = rawChunkedPost(port, "/echo-chunk", chunked);

	EXPECT_EQ(resp.status, 200u) << "raw body: " << resp.body;
	EXPECT_EQ(resp.body, "gated[Hello, chunked world] handler[Hello, chunked world]");

	server.stop();
	serverThread.join();
}
