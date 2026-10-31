/**
 * @file test_http2_middleware.cpp
 * @brief HTTP/2 中间件、body 读取和请求缓存测试
 */

#include "core/http2/Http2ServerSession.h"
#include "core/RequestDispatch.h"
#include "core/RouteGroup.h"
#include "core/RateLimiter.h"
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/use_future.hpp>
#include <gtest/gtest.h>
#include <chrono>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace hical;
using namespace hical::detail;

namespace
{
	class Http2MiddlewareTest : public testing::Test
	{
	protected:
		using Clock = std::chrono::steady_clock;
		static constexpr auto kTestTimeout = std::chrono::seconds {10};
		boost::asio::io_context io;
		boost::asio::ssl::context tls {boost::asio::ssl::context::tls_server};
		Http2Connection::TlsSocket socket {io, tls};
		Router router;
		MiddlewarePipeline pipeline;
		Http2Options options;
		std::shared_ptr<Http2ServiceBudget> service = std::make_shared<Http2ServiceBudget>(64);
		Http2Budget budget {Http2Budget::Role::hServer, options, service.get()};
		std::shared_ptr<Http2ServerSession> session;

		void prepare(std::function<HttpResponse(const std::exception&, const HttpRequest&)> errorHandler = {})
		{
			pipeline.build(
				[](HttpRequest& request)
				{
					return runInternalDispatch(request);
				});
			session = Http2ServerSession::create(
				socket,
				options,
				{router, pipeline, InetAddress("127.0.0.1", 54321), std::move(errorHandler), service, {}});
		}

		std::shared_ptr<Http2Stream> request(std::string_view target = "/echo",
											 int32_t id = 1,
											 std::string_view contentType = "text/plain",
											 std::string_view method = "POST")
		{
			Http2Stream::Config config;
			config.streamId = id;
			config.options = options;
			auto stream = Http2Stream::create(io.get_executor(), config, budget);
			if (!stream)
			{
				ADD_FAILURE() << "未取得测试流额度";
				return {};
			}
			auto& headers = stream->initialHeaders();
			EXPECT_FALSE(headers.append(":method", method));
			EXPECT_FALSE(headers.append(":scheme", "https"));
			EXPECT_FALSE(headers.append(":authority", "localhost"));
			EXPECT_FALSE(headers.append(":path", target));
			EXPECT_FALSE(headers.append("content-type", contentType));
			EXPECT_FALSE(headers.append("x-trace", "kept"));
			EXPECT_FALSE(headers.seal());
			EXPECT_FALSE(Http2ServerSession::mapRequest(*stream, InetAddress("127.0.0.1", 54321)));
			return stream;
		}

		void receive(const std::shared_ptr<Http2Stream>& stream, std::string_view bytes)
		{
			ASSERT_FALSE(stream->appendBody(bytes));
			ASSERT_FALSE(stream->completeBody());
		}

		HttpResponse execute(const std::shared_ptr<Http2Stream>& stream)
		{
			auto result = boost::asio::co_spawn(io,
												session->dispatch(stream, Clock::now() + kTestTimeout),
												boost::asio::use_future);
			io.run();
			io.restart();
			return result.get();
		}
	};
} // namespace

TEST_F(Http2MiddlewareTest, PublishBodyDropsEmptyJsonCacheWithoutReplacingHeaders)
{
	auto stream = request("/json", 1, "application/json");
	ASSERT_TRUE(stream);
	const auto original = stream->request().header("x-trace").data();
	EXPECT_TRUE(stream->request().jsonBody().is_null());
	receive(stream, "{\"value\":42}");
	ASSERT_FALSE(stream->publishBody());
	ASSERT_TRUE(stream->request().jsonBody().is_object());
	EXPECT_EQ(stream->request().jsonBody().as_object().at("value").as_int64(), 42);
	EXPECT_EQ(stream->request().header("x-trace"), "kept");
	EXPECT_EQ(stream->request().header("x-trace").data(), original);
	EXPECT_EQ(stream->request().contentType(), "application/json");
}

TEST_F(Http2MiddlewareTest, PublishBodyDropsEmptyFormCacheWithoutReplacingHeaders)
{
	auto stream = request("/form", 1, "application/x-www-form-urlencoded");
	ASSERT_TRUE(stream);
	EXPECT_FALSE(stream->request().formParam("signature"));
	receive(stream, "signature=ready&name=a%20b");
	ASSERT_FALSE(stream->publishBody());
	EXPECT_EQ(stream->request().formParam("signature"), "ready");
	EXPECT_EQ(stream->request().formParam("name"), "a b");
	EXPECT_EQ(stream->request().header("x-trace"), "kept");
}

TEST_F(Http2MiddlewareTest, BufferedDataStaysHiddenUntilGateAndOuterAfterSeesPublishedBody)
{
	std::vector<std::string> order;
	pipeline.use(
		[&](HttpRequest& req, MiddlewareNext next) -> Awaitable<HttpResponse>
		{
			order.push_back("outer-before:" + req.body());
			EXPECT_TRUE(req.jsonBody().is_null());
			auto response = co_await next(req);
			order.push_back("outer-after:" + req.body());
			EXPECT_EQ(req.jsonBody().as_object().at("value").as_int64(), 42);
			co_return response;
		});
	pipeline.useBody(
		[&](HttpRequest& req, MiddlewareNext next) -> Awaitable<HttpResponse>
		{
			order.push_back("declared:" + req.body());
			co_return co_await next(req);
		});
	pipeline.use(
		[&](HttpRequest& req) -> SyncMiddlewareResult
		{
			order.push_back("inner-before:" + req.body());
			return std::nullopt;
		},
		[&](HttpRequest& req, HttpResponse&)
		{
			order.push_back("inner-after:" + req.body());
		});
	router.post("/echo",
				[&](const HttpRequest& req)
				{
					order.push_back("handler:" + req.body());
					return HttpResponse::ok(req.body());
				});
	prepare();
	auto stream = request("/echo", 1, "application/json");
	receive(stream, "{\"value\":42}");
	const auto response = execute(stream);
	const std::string body = "{\"value\":42}";
	EXPECT_EQ(response.body(), body);
	EXPECT_EQ(order,
			  (std::vector<std::string> {"outer-before:",
										 "declared:" + body,
										 "inner-before:" + body,
										 "handler:" + body,
										 "inner-after:" + body,
										 "outer-after:" + body}));
	EXPECT_EQ(RequestDispatchContext::get(stream->request()), nullptr);
	EXPECT_EQ(budget.usage().applicationTasks, 0U);
}

TEST_F(Http2MiddlewareTest, MultipleGlobalAndGroupBodyDeclarationsReuseTheSameGate)
{
	size_t globalCalls = 0;
	size_t groupCalls = 0;
	pipeline.useBody(
		[&](HttpRequest& req) -> SyncMiddlewareResult
		{
			EXPECT_EQ(req.body(), "group-body");
			++globalCalls;
			return std::nullopt;
		});
	pipeline.useBody(
		[&](HttpRequest& req, MiddlewareNext next) -> Awaitable<HttpResponse>
		{
			EXPECT_EQ(req.body(), "group-body");
			++globalCalls;
			co_return co_await next(req);
		});
	auto group = router.group("/api");
	group.useBody(
		[&](HttpRequest& req, MiddlewareNext next) -> Awaitable<HttpResponse>
		{
			EXPECT_EQ(req.body(), "group-body");
			++groupCalls;
			co_return co_await next(req);
		});
	group.post("/echo",
			   [](const HttpRequest& req)
			   {
				   return HttpResponse::ok(req.body());
			   });
	prepare();
	auto stream = request("/api/echo");
	receive(stream, "group-body");
	EXPECT_EQ(execute(stream).body(), "group-body");
	EXPECT_EQ(globalCalls, 2U);
	EXPECT_EQ(groupCalls, 1U);
	EXPECT_EQ(stream->receivedBytes(), 10U);
	EXPECT_EQ(stream->request().body(), "group-body");
}

TEST_F(Http2MiddlewareTest, OrdinaryPipelineWaitsAtTerminalAndPreservesSyncHandlerResult)
{
	bool called = false;
	pipeline.use(
		[](HttpRequest& req) -> SyncMiddlewareResult
		{
			EXPECT_TRUE(req.body().empty());
			return std::nullopt;
		});
	router.post("/echo",
				[&](const HttpRequest& req)
				{
					called = true;
					return HttpResponse::ok(req.body());
				});
	prepare();
	auto stream = request();
	auto result =
		boost::asio::co_spawn(io, session->dispatch(stream, Clock::now() + kTestTimeout), boost::asio::use_future);
	io.poll();
	EXPECT_FALSE(called);
	receive(stream, "late");
	io.run();
	EXPECT_TRUE(called);
	EXPECT_EQ(result.get().body(), "late");
}

TEST_F(Http2MiddlewareTest, WaitingBodyOnOneStreamDoesNotBlockAnotherAsyncHandler)
{
	router.post("/echo",
				[](const HttpRequest& req) -> Awaitable<HttpResponse>
				{
					co_await boost::asio::post(boost::asio::use_awaitable);
					co_return HttpResponse::ok(req.body());
				});
	prepare();
	auto first = request("/echo", 1);
	auto second = request("/echo", 3);
	receive(second, "second");
	const auto deadline = Clock::now() + kTestTimeout;
	auto waiting = boost::asio::co_spawn(io, session->dispatch(first, deadline), boost::asio::use_future);
	auto ready = boost::asio::co_spawn(io, session->dispatch(second, deadline), boost::asio::use_future);
	io.poll();
	ASSERT_EQ(ready.wait_for(std::chrono::seconds {0}), std::future_status::ready);
	EXPECT_EQ(ready.get().body(), "second");
	EXPECT_EQ(waiting.wait_for(std::chrono::seconds {0}), std::future_status::timeout);
	EXPECT_EQ(service->used(), 1U);
	receive(first, "first");
	io.run();
	EXPECT_EQ(waiting.get().body(), "first");
	EXPECT_EQ(service->used(), 0U);
}

TEST_F(Http2MiddlewareTest, EarlyAuthenticationFailureDoesNotWaitForBodyOrAffectOtherStreams)
{
	pipeline.use(
		[](HttpRequest& req) -> SyncMiddlewareResult
		{
			EXPECT_TRUE(req.body().empty());
			if (req.path() == "/denied")
			{
				auto response = HttpResponse::badRequest("denied");
				response.setStatus(HttpStatusCode::hUnauthorized);
				return response;
			}
			return std::nullopt;
		});
	router.post("/denied",
				[](const HttpRequest&)
				{
					return HttpResponse::serverError();
				});
	router.post("/echo",
				[](const HttpRequest& req)
				{
					return HttpResponse::ok(req.body());
				});
	prepare();
	auto denied = request("/denied", 1);
	auto accepted = request("/echo", 3);
	receive(accepted, "accepted");
	const auto deadline = Clock::now() + kTestTimeout;
	auto rejected = boost::asio::co_spawn(io, session->dispatch(denied, deadline), boost::asio::use_future);
	auto completed = boost::asio::co_spawn(io, session->dispatch(accepted, deadline), boost::asio::use_future);
	io.run();
	EXPECT_EQ(rejected.get().native().status, HttpStatusCode::hUnauthorized);
	EXPECT_EQ(completed.get().body(), "accepted");
	EXPECT_FALSE(denied->bodyComplete());
	EXPECT_EQ(denied->receivedBytes(), 0U);
}

TEST_F(Http2MiddlewareTest, RateLimiterRejectsBeforeBodyGate)
{
	RateLimiterOptions rate;
	rate.config.rate = 0.001;
	rate.config.burst = 1;
	rate.keyExtractor = [](const HttpRequest&)
	{
		return std::string("same");
	};
	pipeline.use(makeRateLimiterMiddleware(rate));
	router.post("/echo",
				[](const HttpRequest& req)
				{
					return HttpResponse::ok(req.body());
				});
	prepare();
	auto first = request("/echo", 1);
	auto second = request("/echo", 3);
	receive(first, "first");
	EXPECT_EQ(execute(first).body(), "first");
	auto response = execute(second);
	EXPECT_EQ(response.native().status, HttpStatusCode::hTooManyRequests);
	EXPECT_FALSE(second->bodyComplete());
	EXPECT_EQ(second->receivedBytes(), 0U);
}

TEST_F(Http2MiddlewareTest, CachedRouteAndParametersSurviveMiddlewareTargetChanges)
{
	pipeline.use(
		[](HttpRequest& req) -> SyncMiddlewareResult
		{
			EXPECT_EQ(req.param("id"), "original");
			req.setParam("id", "edited");
			req.setTarget("/changed");
			return std::nullopt;
		});
	router.post("/items/{id}",
				[](const HttpRequest& req)
				{
					return HttpResponse::ok("cached:" + req.param("id"));
				});
	router.post("/changed",
				[](const HttpRequest&)
				{
					return HttpResponse::ok("wrong-route");
				});
	prepare();
	auto stream = request("/items/original");
	receive(stream, "body");
	EXPECT_EQ(execute(stream).body(), "cached:edited");
}

TEST_F(Http2MiddlewareTest, InvalidPathsAndMethodMismatchReturnBeforeBodyAndMiddleware)
{
	size_t middlewareCalls = 0;
	pipeline.use(
		[&](HttpRequest&) -> SyncMiddlewareResult
		{
			++middlewareCalls;
			return std::nullopt;
		});
	router.get("/only-get",
			   [](const HttpRequest&)
			   {
				   return HttpResponse::ok("get");
			   });
	prepare();
	auto missing = request("/missing", 1);
	auto method = request("/only-get", 3);
	EXPECT_EQ(execute(missing).native().status, HttpStatusCode::hNotFound);
	auto response = execute(method);
	EXPECT_EQ(response.native().status, HttpStatusCode::hMethodNotAllowed);
	EXPECT_NE(response.header("Allow").find("GET"), std::string::npos);
	std::string tooDeep;
	for (size_t index = 0; index <= Router::kMaxPathSegments; ++index)
	{
		tooDeep += "/x";
	}
	auto deep = request(tooDeep, 5);
	EXPECT_EQ(execute(deep).native().status, HttpStatusCode::hBadRequest);
	EXPECT_EQ(middlewareCalls, 0U);
	EXPECT_FALSE(missing->bodyComplete());
	EXPECT_FALSE(method->bodyComplete());
	EXPECT_FALSE(deep->bodyComplete());
}

TEST_F(Http2MiddlewareTest, SseOnlyAndOrdinaryConnectReturnNotImplementedBeforeBody)
{
	router.sse("/events",
			   [](std::shared_ptr<SseSession>) -> Awaitable<void>
			   {
				   co_return;
			   });
	prepare();
	auto events = request("/events", 1, "text/plain", "GET");
	EXPECT_EQ(execute(events).native().status, HttpStatusCode::hNotImplemented);
	Http2Stream::Config config;
	config.streamId = 3;
	auto connect = Http2Stream::create(io.get_executor(), config, budget);
	ASSERT_TRUE(connect);
	ASSERT_FALSE(connect->initialHeaders().append(":method", "CONNECT"));
	ASSERT_FALSE(connect->initialHeaders().append(":authority", "target.test:443"));
	ASSERT_FALSE(connect->initialHeaders().seal());
	ASSERT_FALSE(Http2ServerSession::mapRequest(*connect, InetAddress("127.0.0.1", 1)));
	EXPECT_EQ(execute(connect).native().status, HttpStatusCode::hNotImplemented);
	EXPECT_FALSE(connect->bodyComplete());
}

TEST_F(Http2MiddlewareTest, ErrorHandlerReceivesMappedRequestAndContextAlwaysDetaches)
{
	router.post("/echo",
				[](const HttpRequest&) -> HttpResponse
				{
					throw std::runtime_error("handler failed");
				});
	bool handled = false;
	prepare(
		[&](const std::exception& error, const HttpRequest& req)
		{
			handled = true;
			EXPECT_EQ(req.protocol(), HttpProtocol::hHttp2);
			EXPECT_EQ(req.body(), "body");
			EXPECT_STREQ(error.what(), "handler failed");
			return HttpResponse::badRequest("handled");
		});
	auto stream = request();
	receive(stream, "body");
	EXPECT_EQ(execute(stream).body(), "handled");
	EXPECT_TRUE(handled);
	EXPECT_EQ(RequestDispatchContext::get(stream->request()), nullptr);
	EXPECT_EQ(service->used(), 0U);
}

TEST_F(Http2MiddlewareTest, ThrowingErrorHandlerFallsBackToServerError)
{
	router.post("/echo",
				[](const HttpRequest&) -> HttpResponse
				{
					throw std::runtime_error("failed");
				});
	prepare(
		[](const std::exception&, const HttpRequest&) -> HttpResponse
		{
			throw std::runtime_error("also failed");
		});
	auto stream = request();
	receive(stream, "body");
	EXPECT_EQ(execute(stream).native().status, HttpStatusCode::hInternalServerError);
	EXPECT_EQ(RequestDispatchContext::get(stream->request()), nullptr);
}

TEST_F(Http2MiddlewareTest, ResourceFailureDoesNotInvokeHandler)
{
	bool called = false;
	router.post("/echo",
				[&](const HttpRequest&)
				{
					called = true;
					return HttpResponse::ok("wrong");
				});
	prepare();
	auto stream = request();
	const std::string oversized(options.maxBodyBytes + 1, 'x');
	ASSERT_EQ(stream->appendBody(oversized).code, ErrorCode::hResourceLimitExceeded);
	EXPECT_EQ(execute(stream).native().status, HttpStatusCode::hPayloadTooLarge);
	EXPECT_FALSE(called);
	EXPECT_TRUE(stream->request().body().empty());
}

TEST_F(Http2MiddlewareTest, BodyGateUsesOriginalAbsoluteDeadline)
{
	bool called = false;
	pipeline.use(
		[](HttpRequest& req, MiddlewareNext next) -> Awaitable<HttpResponse>
		{
			co_await boost::asio::post(boost::asio::use_awaitable);
			co_return co_await next(req);
		});
	pipeline.useBody(
		[](HttpRequest&) -> SyncMiddlewareResult
		{
			ADD_FAILURE() << "过期的 body 门不应继续调用声明者";
			return std::nullopt;
		});
	router.post("/echo",
				[&](const HttpRequest&)
				{
					called = true;
					return HttpResponse::ok("wrong");
				});
	prepare();
	auto stream = request();
	auto result = boost::asio::co_spawn(io,
										session->dispatch(stream, Clock::now() - std::chrono::seconds {1}),
										boost::asio::use_future);
	io.run();
	EXPECT_EQ(static_cast<int>(result.get().native().status), 408);
	EXPECT_EQ(stream->error().code, ErrorCode::hTimedOut);
	EXPECT_FALSE(called);
	EXPECT_TRUE(stream->request().body().empty());
	EXPECT_EQ(RequestDispatchContext::get(stream->request()), nullptr);
	EXPECT_EQ(service->used(), 0U);
}

TEST_F(Http2MiddlewareTest, BodyCompletedAfterDeadlineDuringMiddlewareIsRejected)
{
	std::shared_ptr<Http2Stream> stream;
	bool bodyMiddlewareCalled = false;
	bool handlerCalled = false;
	pipeline.use(
		[&](HttpRequest& req, MiddlewareNext next) -> Awaitable<HttpResponse>
		{
			EXPECT_FALSE(stream->bodyComplete());
			boost::asio::post(io,
							  [&]
							  {
								  receive(stream, "late");
							  });
			co_await boost::asio::post(boost::asio::use_awaitable);
			EXPECT_TRUE(stream->bodyComplete());
			EXPECT_TRUE(req.body().empty());
			co_return co_await next(req);
		});
	pipeline.useBody(
		[&](HttpRequest&) -> SyncMiddlewareResult
		{
			bodyMiddlewareCalled = true;
			return std::nullopt;
		});
	router.post("/echo",
				[&](const HttpRequest& req)
				{
					handlerCalled = true;
					return HttpResponse::ok(req.body());
				});
	prepare();
	stream = request();
	auto result = boost::asio::co_spawn(io,
										session->dispatch(stream, Clock::now() - std::chrono::seconds {1}),
										boost::asio::use_future);
	io.run();
	EXPECT_EQ(static_cast<int>(result.get().native().status), 408);
	EXPECT_EQ(stream->error().code, ErrorCode::hTimedOut);
	EXPECT_FALSE(bodyMiddlewareCalled);
	EXPECT_FALSE(handlerCalled);
	EXPECT_TRUE(stream->request().body().empty());
	EXPECT_EQ(RequestDispatchContext::get(stream->request()), nullptr);
	EXPECT_EQ(service->used(), 0U);
}

TEST_F(Http2MiddlewareTest, BodyCompletedBeforeDeadlineSurvivesLateMiddlewareResume)
{
	bool bodyMiddlewareCalled = false;
	pipeline.use(
		[](HttpRequest& req, MiddlewareNext next) -> Awaitable<HttpResponse>
		{
			EXPECT_TRUE(req.body().empty());
			co_await boost::asio::post(boost::asio::use_awaitable);
			co_return co_await next(req);
		});
	pipeline.useBody(
		[&](HttpRequest& req) -> SyncMiddlewareResult
		{
			bodyMiddlewareCalled = true;
			EXPECT_EQ(req.body(), "on-time");
			return std::nullopt;
		});
	router.post("/echo",
				[](const HttpRequest& req)
				{
					return HttpResponse::ok(req.body());
				});
	prepare();
	auto stream = request();
	receive(stream, "on-time");
	const auto deadline = Clock::now();
	auto result = boost::asio::co_spawn(io, session->dispatch(stream, deadline), boost::asio::use_future);
	io.run();
	EXPECT_EQ(result.get().body(), "on-time");
	EXPECT_TRUE(bodyMiddlewareCalled);
	EXPECT_FALSE(stream->error());
	EXPECT_EQ(RequestDispatchContext::get(stream->request()), nullptr);
	EXPECT_EQ(service->used(), 0U);
}

TEST_F(Http2MiddlewareTest, DispatchEntryOwnsSessionAndStreamBeforeCoroutineStarts)
{
	router.post("/echo",
				[](const HttpRequest& req) -> Awaitable<HttpResponse>
				{
					co_await boost::asio::post(boost::asio::use_awaitable);
					co_return HttpResponse::ok(req.body());
				});
	prepare();
	auto stream = request();
	receive(stream, "owned");
	std::weak_ptr<Http2ServerSession> weakSession = session;
	std::weak_ptr<Http2Stream> weakStream = stream;
	auto operation = session->dispatch(stream, Clock::now() + kTestTimeout);
	session.reset();
	stream.reset();
	EXPECT_FALSE(weakSession.expired());
	EXPECT_FALSE(weakStream.expired());
	auto result = boost::asio::co_spawn(io, std::move(operation), boost::asio::use_future);
	io.run();
	EXPECT_EQ(result.get().body(), "owned");
	EXPECT_TRUE(weakSession.expired());
	EXPECT_TRUE(weakStream.expired());
	EXPECT_EQ(service->used(), 0U);
}
