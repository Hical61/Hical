/**
 * @file test_http2_stream.cpp
 * @brief HTTP/2 流状态、body 等待与任务生命周期测试
 */

#include "core/http2/Http2Stream.h"
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/use_future.hpp>
#include <gtest/gtest.h>
#include <chrono>
#include <future>
#include <stdexcept>
#include <type_traits>
#include <utility>

using namespace hical;
using namespace hical::detail;

namespace
{
	Http2Options streamOptions()
	{
		Http2Options options;
		options.maxConcurrentStreams = 2;
		options.maxApplicationTasks = 2;
		options.maxBodyBytes = 4;
		options.maxConnectionReceiveBytes = 6;
		options.maxConnectionSendBytes = 8;
		options.maxOutputBatchBytes = 4;
		options.maxFilePrefetchBytes = 4;
		options.maxResponseBytes = 8;
		options.maxClientConnectionReceiveBytes = 12;
		return options;
	}

	Http2Stream::Config streamConfig(int32_t id)
	{
		Http2Stream::Config config;
		config.streamId = id;
		config.options = streamOptions();
		return config;
	}

	void sealRequest(const std::shared_ptr<Http2Stream>& stream)
	{
		ASSERT_FALSE(stream->initialHeaders().append(":method", "POST"));
		ASSERT_FALSE(stream->initialHeaders().append(":scheme", "https"));
		ASSERT_FALSE(stream->initialHeaders().append(":path", "/upload"));
		ASSERT_FALSE(stream->initialHeaders().append(":authority", "test.local"));
		ASSERT_FALSE(stream->initialHeaders().seal());
	}
} // namespace

static_assert(!std::is_copy_constructible_v<Http2Stream::ApplicationGuard>);
static_assert(std::is_nothrow_move_constructible_v<Http2Stream::ApplicationGuard>);

TEST(Http2StreamTest, FactoryRejectsCapacityBeforePublishingStream)
{
	boost::asio::io_context io;
	Http2ServiceBudget service(1);
	Http2Budget budget(Http2Budget::Role::hServer, streamOptions(), &service);
	auto first = Http2Stream::create(io.get_executor(), streamConfig(1), budget);
	ASSERT_TRUE(first);
	EXPECT_FALSE(Http2Stream::create(io.get_executor(), streamConfig(3), budget));
	EXPECT_EQ(budget.usage().protocolStreams, 1U);
	EXPECT_EQ(budget.usage().applicationTasks, 1U);
	EXPECT_EQ(service.used(), 1U);
	first.reset();
	EXPECT_TRUE(Http2Stream::create(io.get_executor(), streamConfig(3), budget));
	EXPECT_EQ(service.used(), 0U);
}

TEST(Http2StreamTest, InvalidFactoryArgumentsDoNotReserveResources)
{
	boost::asio::io_context io;
	Http2Budget budget(Http2Budget::Role::hClient, streamOptions());
	auto config = streamConfig(0);
	EXPECT_THROW((void)Http2Stream::create(io.get_executor(), config, budget), std::invalid_argument);
	config = streamConfig(1);
	config.incomingKind = Http2Codec::HeaderKind::hTrailers;
	EXPECT_THROW((void)Http2Stream::create(io.get_executor(), config, budget), std::invalid_argument);
	EXPECT_EQ(budget.usage().protocolStreams, 0U);
	EXPECT_EQ(budget.usage().applicationTasks, 0U);
}

TEST(Http2StreamTest, ClientFactoryHonorsPeerConcurrencyLimit)
{
	boost::asio::io_context io;
	Http2Budget budget(Http2Budget::Role::hClient, streamOptions());
	auto config = streamConfig(1);
	config.peerLimit = 0;
	EXPECT_FALSE(Http2Stream::create(io.get_executor(), config, budget));
	EXPECT_EQ(budget.usage().protocolStreams, 0U);
	EXPECT_EQ(budget.usage().applicationTasks, 0U);
}

TEST(Http2StreamTest, TwoStreamsOwnIndependentHeadersBodiesAndResponses)
{
	boost::asio::io_context io;
	Http2Budget budget(Http2Budget::Role::hClient, streamOptions());
	auto first = Http2Stream::create(io.get_executor(), streamConfig(1), budget);
	auto second = Http2Stream::create(io.get_executor(), streamConfig(3), budget);
	ASSERT_TRUE(first);
	ASSERT_TRUE(second);
	sealRequest(first);
	sealRequest(second);
	ASSERT_FALSE(first->trailers().append("x-checksum", "one"));
	ASSERT_FALSE(first->trailers().seal());
	EXPECT_TRUE(second->trailers().fields().empty());
	EXPECT_FALSE(first->appendBody("abc"));
	EXPECT_FALSE(second->appendBody("de"));
	EXPECT_TRUE(first->request().body().empty());
	EXPECT_TRUE(second->request().body().empty());
	EXPECT_FALSE(first->completeBody());
	EXPECT_TRUE(first->request().body().empty());
	EXPECT_EQ(first->body(), "abc");
	EXPECT_FALSE(first->publishBody());
	EXPECT_EQ(first->request().body(), "abc");
	EXPECT_TRUE(second->request().body().empty());
	EXPECT_FALSE(second->completeBody());
	EXPECT_FALSE(second->publishBody());
	EXPECT_EQ(second->request().body(), "de");
	first->response().setBody("first response");
	EXPECT_TRUE(second->response().body().empty());
	EXPECT_EQ(first->initialHeaders().target(), "/upload");
	EXPECT_EQ(first->trailers().find("x-checksum"), "one");
	EXPECT_EQ(budget.usage().receiveBytes, 5U);
}

TEST(Http2StreamTest, ResponseBodyUsesClientStorageAndRemainsHiddenUntilComplete)
{
	boost::asio::io_context io;
	Http2Budget budget(Http2Budget::Role::hClient, streamOptions());
	auto config = streamConfig(1);
	config.incomingKind = Http2Codec::HeaderKind::hResponse;
	auto stream = Http2Stream::create(io.get_executor(), config, budget);
	ASSERT_TRUE(stream);
	ASSERT_FALSE(stream->initialHeaders().append(":status", "200"));
	ASSERT_FALSE(stream->initialHeaders().seal());
	EXPECT_FALSE(stream->appendBody("response"));
	EXPECT_TRUE(stream->response().body().empty());
	EXPECT_FALSE(stream->completeBody());
	EXPECT_TRUE(stream->response().body().empty());
	EXPECT_EQ(stream->body(), "response");
	EXPECT_FALSE(stream->publishBody());
	EXPECT_EQ(stream->response().body(), "response");
	EXPECT_TRUE(stream->request().body().empty());
}

TEST(Http2StreamTest, BodyBudgetFailureKeepsPreviouslyAcceptedBytes)
{
	boost::asio::io_context io;
	Http2ServiceBudget service(2);
	Http2Budget budget(Http2Budget::Role::hServer, streamOptions(), &service);
	auto stream = Http2Stream::create(io.get_executor(), streamConfig(1), budget);
	ASSERT_TRUE(stream);
	sealRequest(stream);
	ASSERT_FALSE(stream->appendBody("abc"));
	auto error = stream->appendBody("de");
	EXPECT_EQ(error.code, ErrorCode::hResourceLimitExceeded);
	EXPECT_EQ(stream->receivedBytes(), 3U);
	EXPECT_EQ(budget.usage().receiveBytes, 3U);
	EXPECT_TRUE(stream->request().body().empty());
	EXPECT_FALSE(stream->protocolOpen());
	EXPECT_EQ(service.used(), 0U);
	EXPECT_EQ(stream->completeBody().code, ErrorCode::hResourceLimitExceeded);
	stream.reset();
	EXPECT_EQ(budget.usage().receiveBytes, 0U);
}

TEST(Http2StreamTest, BodyPublicationRequiresCompletionAndRejectsResetBeforeHandler)
{
	boost::asio::io_context io;
	Http2Budget budget(Http2Budget::Role::hClient, streamOptions());
	auto stream = Http2Stream::create(io.get_executor(), streamConfig(1), budget);
	ASSERT_TRUE(stream);
	sealRequest(stream);
	ASSERT_FALSE(stream->appendBody("body"));
	EXPECT_TRUE(stream->body().empty());
	EXPECT_EQ(stream->publishBody().code, ErrorCode::hOperationInProgress);
	EXPECT_FALSE(stream->completeBody());
	EXPECT_TRUE(stream->request().body().empty());
	EXPECT_EQ(stream->body(), "body");
	stream->reset();
	EXPECT_EQ(stream->publishBody().code, ErrorCode::hHttp2StreamError);
	EXPECT_TRUE(stream->request().body().empty());
	EXPECT_EQ(stream->body(), "body");
}

TEST(Http2StreamTest, NormalCloseReturnsUnstartedSlotsAndIgnoresLateFailure)
{
	boost::asio::io_context io;
	Http2Budget budget(Http2Budget::Role::hClient, streamOptions());
	auto stream = Http2Stream::create(io.get_executor(), streamConfig(1), budget);
	ASSERT_TRUE(stream);
	sealRequest(stream);
	ASSERT_FALSE(stream->completeBody());
	stream->closeProtocol();
	stream->closeProtocol();
	EXPECT_FALSE(stream->fail({ErrorCode::hConnectionReset, "late connection failure"}));
	EXPECT_FALSE(stream->error());
	EXPECT_FALSE(stream->startApplication());
	EXPECT_EQ(budget.usage().protocolStreams, 0U);
	EXPECT_EQ(budget.usage().applicationTasks, 0U);
}

TEST(Http2StreamTest, PrematureProtocolCloseFailsBodyWaiters)
{
	boost::asio::io_context io;
	Http2Budget budget(Http2Budget::Role::hClient, streamOptions());
	auto stream = Http2Stream::create(io.get_executor(), streamConfig(1), budget);
	ASSERT_TRUE(stream);
	auto result = boost::asio::co_spawn(io, stream->waitBody(), boost::asio::use_future);
	boost::asio::post(io,
					  [stream]
					  {
						  stream->closeProtocol();
					  });
	io.run();
	EXPECT_EQ(result.get().code, ErrorCode::hHttp2StreamError);
	ASSERT_TRUE(stream->error().http2);
	EXPECT_EQ(stream->error().http2->code, static_cast<uint32_t>(Http2Code::hStreamClosed));
}

TEST(Http2StreamTest, SendReservationRejectsLateProductionAndAllowsReturningBytes)
{
	boost::asio::io_context io;
	Http2Budget budget(Http2Budget::Role::hClient, streamOptions());
	auto first = Http2Stream::create(io.get_executor(), streamConfig(1), budget);
	auto second = Http2Stream::create(io.get_executor(), streamConfig(3), budget);
	ASSERT_TRUE(first);
	ASSERT_TRUE(second);
	ASSERT_TRUE(first->reserveSend(6));
	ASSERT_TRUE(second->reserveSend(2));
	EXPECT_FALSE(first->reserveSend(7));
	EXPECT_EQ(budget.usage().sendBytes, 8U);
	first->reset();
	EXPECT_FALSE(first->reserveSend(7));
	EXPECT_TRUE(first->reserveSend(3));
	EXPECT_EQ(budget.usage().sendBytes, 5U);
	first.reset();
	EXPECT_EQ(budget.usage().sendBytes, 2U);
}

TEST(Http2StreamTest, ResetPreservesRunningTaskAndPayloadUntilGuardExits)
{
	boost::asio::io_context io;
	Http2ServiceBudget service(1);
	Http2Budget budget(Http2Budget::Role::hServer, streamOptions(), &service);
	auto stream = Http2Stream::create(io.get_executor(), streamConfig(1), budget);
	ASSERT_TRUE(stream);
	sealRequest(stream);
	ASSERT_FALSE(stream->appendBody("body"));
	ASSERT_FALSE(stream->completeBody());
	ASSERT_FALSE(stream->publishBody());
	auto application = stream->startApplication();
	ASSERT_TRUE(application);
	EXPECT_FALSE(stream->startApplication());
	std::weak_ptr<Http2Stream> weak = stream;
	stream->reset();
	stream->reset();
	EXPECT_EQ(stream->request().body(), "body");
	EXPECT_EQ(budget.usage().protocolStreams, 0U);
	EXPECT_EQ(service.used(), 1U);
	EXPECT_FALSE(Http2Stream::create(io.get_executor(), streamConfig(3), budget));
	stream.reset();
	EXPECT_FALSE(weak.expired());
	EXPECT_EQ(budget.usage().receiveBytes, 4U);
	application.reset();
	application.reset();
	EXPECT_TRUE(weak.expired());
	EXPECT_EQ(service.used(), 0U);
	EXPECT_EQ(budget.usage().receiveBytes, 0U);
}

TEST(Http2StreamTest, ApplicationGuardMovesAndExceptionExitReturnOnlyOneSlot)
{
	boost::asio::io_context io;
	Http2ServiceBudget service(1);
	Http2Budget budget(Http2Budget::Role::hServer, streamOptions(), &service);
	auto stream = Http2Stream::create(io.get_executor(), streamConfig(1), budget);
	ASSERT_TRUE(stream);
	try
	{
		auto first = stream->startApplication();
		auto moved = std::move(first);
		EXPECT_FALSE(first);
		ASSERT_TRUE(moved);
		throw std::runtime_error("handler failed");
	}
	catch (const std::runtime_error&)
	{
	}
	EXPECT_EQ(service.used(), 0U);
	EXPECT_FALSE(stream->startApplication());
	stream->reset();
	EXPECT_EQ(service.used(), 0U);
}

TEST(Http2StreamTest, StreamAndTaskCanOutliveBudgetFacades)
{
	boost::asio::io_context io;
	std::shared_ptr<Http2Stream> stream;
	Http2Stream::ApplicationGuard application;
	{
		Http2ServiceBudget service(1);
		Http2Budget budget(Http2Budget::Role::hServer, streamOptions(), &service);
		stream = Http2Stream::create(io.get_executor(), streamConfig(1), budget);
		ASSERT_TRUE(stream);
		application = stream->startApplication();
	}
	sealRequest(stream);
	EXPECT_FALSE(stream->appendBody("body"));
	EXPECT_FALSE(stream->completeBody());
	EXPECT_FALSE(stream->publishBody());
	stream->reset();
	EXPECT_EQ(stream->request().body(), "body");
	application.reset();
	stream.reset();
}

TEST(Http2StreamTest, SuccessfulEndWakesAllWaitersAndLateResetDoesNotRewriteBodyResult)
{
	boost::asio::io_context io;
	Http2Budget budget(Http2Budget::Role::hClient, streamOptions());
	auto stream = Http2Stream::create(io.get_executor(), streamConfig(1), budget);
	ASSERT_TRUE(stream);
	sealRequest(stream);
	auto first = boost::asio::co_spawn(io, stream->waitBody(), boost::asio::use_future);
	auto second = boost::asio::co_spawn(io, stream->waitBody(), boost::asio::use_future);
	boost::asio::post(io,
					  [stream]
					  {
						  EXPECT_FALSE(stream->appendBody("body"));
						  EXPECT_FALSE(stream->completeBody());
						  EXPECT_FALSE(stream->completeBody());
						  EXPECT_FALSE(stream->publishBody());
						  stream->reset();
					  });
	io.run();
	EXPECT_FALSE(first.get());
	EXPECT_FALSE(second.get());
	EXPECT_TRUE(stream->bodyComplete());
	EXPECT_EQ(stream->error().code, ErrorCode::hHttp2StreamError);
	EXPECT_EQ(stream->request().body(), "body");
}

TEST(Http2StreamTest, ResetWakesAllWaitersAndIgnoresLateBodyCompletion)
{
	boost::asio::io_context io;
	Http2Budget budget(Http2Budget::Role::hClient, streamOptions());
	auto stream = Http2Stream::create(io.get_executor(), streamConfig(1), budget);
	ASSERT_TRUE(stream);
	sealRequest(stream);
	auto first = boost::asio::co_spawn(io, stream->waitBody(), boost::asio::use_future);
	auto second = boost::asio::co_spawn(io, stream->waitBody(), boost::asio::use_future);
	boost::asio::post(io,
					  [stream]
					  {
						  stream->reset(Http2Code::hCancel);
						  stream->fail({ErrorCode::hConnectionReset, "late transport failure"});
						  EXPECT_EQ(stream->completeBody().code, ErrorCode::hHttp2StreamError);
						  EXPECT_EQ(stream->appendBody("late").code, ErrorCode::hHttp2StreamError);
					  });
	io.run();
	EXPECT_EQ(first.get().code, ErrorCode::hHttp2StreamError);
	EXPECT_EQ(second.get().code, ErrorCode::hHttp2StreamError);
	EXPECT_EQ(stream->error().http2->code, static_cast<uint32_t>(Http2Code::hCancel));
	EXPECT_TRUE(stream->request().body().empty());
	EXPECT_EQ(budget.usage().protocolStreams, 0U);
	EXPECT_EQ(budget.usage().applicationTasks, 0U);
}

TEST(Http2StreamTest, ConnectionFailureCompletesEachAffectedStreamOnce)
{
	boost::asio::io_context io;
	Http2Budget budget(Http2Budget::Role::hClient, streamOptions());
	auto first = Http2Stream::create(io.get_executor(), streamConfig(1), budget);
	auto second = Http2Stream::create(io.get_executor(), streamConfig(3), budget);
	ASSERT_TRUE(first);
	ASSERT_TRUE(second);
	auto firstWait = boost::asio::co_spawn(io, first->waitBody(), boost::asio::use_future);
	auto secondWait = boost::asio::co_spawn(io, second->waitBody(), boost::asio::use_future);
	boost::asio::post(io,
					  [first, second]
					  {
						  first->fail({ErrorCode::hConnectionReset, "connection failed"});
						  second->fail({ErrorCode::hConnectionReset, "connection failed"});
						  first->reset();
					  });
	io.run();
	EXPECT_EQ(firstWait.get().code, ErrorCode::hConnectionReset);
	EXPECT_EQ(secondWait.get().code, ErrorCode::hConnectionReset);
}

TEST(Http2StreamTest, AbsoluteDeadlineFailsBodyAndWakesOtherWaiters)
{
	boost::asio::io_context io;
	Http2Budget budget(Http2Budget::Role::hClient, streamOptions());
	auto stream = Http2Stream::create(io.get_executor(), streamConfig(1), budget);
	ASSERT_TRUE(stream);
	auto first = boost::asio::co_spawn(io, stream->waitBody(std::chrono::steady_clock::now()), boost::asio::use_future);
	auto second = boost::asio::co_spawn(io, stream->waitBody(), boost::asio::use_future);
	io.run();
	EXPECT_EQ(first.get().code, ErrorCode::hTimedOut);
	EXPECT_EQ(second.get().code, ErrorCode::hTimedOut);
	EXPECT_FALSE(stream->protocolOpen());
}

TEST(Http2StreamTest, LateEndCannotBeatAlreadyExpiredWaiterBeforeTimerCallback)
{
	boost::asio::io_context io;
	Http2Budget budget(Http2Budget::Role::hClient, streamOptions());
	auto stream = Http2Stream::create(io.get_executor(), streamConfig(1), budget);
	ASSERT_TRUE(stream);
	sealRequest(stream);
	const auto deadline = Http2Stream::Clock::now() - std::chrono::seconds {1};
	auto result = boost::asio::co_spawn(io, stream->waitBody(deadline), boost::asio::use_future);
	// 只运行启动 waiter 的回调，过期 timer 的完成回调仍在同一事件循环中排队。
	ASSERT_EQ(io.poll_one(), 1U);
	ASSERT_EQ(result.wait_for(std::chrono::seconds {0}), std::future_status::timeout);
	ASSERT_FALSE(stream->bodyComplete());
	ASSERT_FALSE(stream->appendBody("late"));
	ASSERT_FALSE(stream->completeBody());
	io.run();
	EXPECT_EQ(result.get().code, ErrorCode::hTimedOut);
	EXPECT_EQ(stream->error().code, ErrorCode::hTimedOut);
	EXPECT_FALSE(stream->protocolOpen());
	EXPECT_TRUE(stream->body().empty());
}

TEST(Http2StreamTest, CallerCancellationFailsStreamAndWakesEveryWaiter)
{
	boost::asio::io_context io;
	boost::asio::cancellation_signal cancellation;
	Http2Budget budget(Http2Budget::Role::hClient, streamOptions());
	auto stream = Http2Stream::create(io.get_executor(), streamConfig(1), budget);
	ASSERT_TRUE(stream);
	auto first =
		boost::asio::co_spawn(io,
							  stream->waitBody(),
							  boost::asio::bind_cancellation_slot(cancellation.slot(), boost::asio::use_future));
	auto second = boost::asio::co_spawn(io, stream->waitBody(), boost::asio::use_future);
	boost::asio::post(io,
					  [&cancellation]
					  {
						  cancellation.emit(boost::asio::cancellation_type::terminal);
					  });
	io.run();
	EXPECT_EQ(first.get().code, ErrorCode::hOperationAborted);
	EXPECT_EQ(second.get().code, ErrorCode::hOperationAborted);
}

TEST(Http2StreamTest, WaitCapturesOwnerBeforeLazyCoroutineStarts)
{
	boost::asio::io_context io;
	Http2Budget budget(Http2Budget::Role::hClient, streamOptions());
	auto stream = Http2Stream::create(io.get_executor(), streamConfig(1), budget);
	ASSERT_TRUE(stream);
	sealRequest(stream);
	EXPECT_FALSE(stream->completeBody());
	auto wait = stream->waitBody();
	std::weak_ptr<Http2Stream> weak = stream;
	stream.reset();
	EXPECT_FALSE(weak.expired());
	auto result = boost::asio::co_spawn(io, std::move(wait), boost::asio::use_future);
	io.run();
	EXPECT_FALSE(result.get());
	EXPECT_TRUE(weak.expired());
}

TEST(Http2StreamTest, RunningCoroutineSurvivesResetUntilItsOwnWaitCompletes)
{
	boost::asio::io_context io;
	Http2ServiceBudget service(1);
	Http2Budget budget(Http2Budget::Role::hServer, streamOptions(), &service);
	auto stream = Http2Stream::create(io.get_executor(), streamConfig(1), budget);
	ASSERT_TRUE(stream);
	sealRequest(stream);
	ASSERT_FALSE(stream->appendBody("body"));
	ASSERT_FALSE(stream->completeBody());
	ASSERT_FALSE(stream->publishBody());
	boost::asio::steady_timer work(io, std::chrono::steady_clock::time_point::max());
	auto task = [](std::shared_ptr<Http2Stream> owner, boost::asio::steady_timer& work) -> boost::asio::awaitable<void>
	{
		auto application = owner->startApplication();
		EXPECT_TRUE(application);
		boost::system::error_code ignored;
		co_await work.async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, ignored));
		EXPECT_EQ(owner->request().body(), "body");
	};
	auto finished = boost::asio::co_spawn(io, task(stream, work), boost::asio::use_future);
	boost::asio::post(io,
					  [stream, &budget, &service, &work]
					  {
						  stream->reset();
						  EXPECT_EQ(service.used(), 1U);
						  EXPECT_EQ(budget.usage().receiveBytes, 4U);
						  work.cancel();
					  });
	std::weak_ptr<Http2Stream> weak = stream;
	stream.reset();
	io.run();
	EXPECT_NO_THROW(finished.get());
	EXPECT_TRUE(weak.expired());
	EXPECT_EQ(service.used(), 0U);
	EXPECT_EQ(budget.usage().receiveBytes, 0U);
}
