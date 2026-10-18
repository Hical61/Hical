/**
 * @file test_timer_service.cpp
 * @brief 集中式定时器服务的单元测试
 */

#include "core/TimerService.h"
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <gtest/gtest.h>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

using namespace hical;
using namespace std::chrono_literals;

namespace
{
	// 记下触发顺序用
	struct FiredOrder
	{
		std::vector<int> order;

		TimerService::Callback record(int tag)
		{
			return [this, tag]()
			{
				order.push_back(tag);
			};
		}
	};
} // namespace

TEST(TimerServiceTest, ScheduleAfter_OneTimer_FiresOnce)
{
	boost::asio::io_context ioContext;
	TimerService service(ioContext.get_executor());

	int fired = 0;
	auto id = service.scheduleAfter(5ms,
									[&fired]()
									{
										++fired;
									});

	ASSERT_NE(id, TimerService::kInvalidId);
	EXPECT_EQ(service.pendingCount(), 1u);

	ioContext.run();

	EXPECT_EQ(fired, 1);
	EXPECT_EQ(service.pendingCount(), 0u);
}

TEST(TimerServiceTest, SameDeadline_MultipleTimers_FireInScheduleOrder)
{
	boost::asio::io_context ioContext;
	TimerService service(ioContext.get_executor());

	FiredOrder recorder;
	const auto deadline = TimerService::Clock::now() + 5ms;
	for (int i = 0; i < 3; ++i)
	{
		service.schedule(deadline, recorder.record(i));
	}

	ioContext.run();

	ASSERT_EQ(recorder.order.size(), 3u);
	EXPECT_EQ(recorder.order[0], 0);
	EXPECT_EQ(recorder.order[1], 1);
	EXPECT_EQ(recorder.order[2], 2);
}

TEST(TimerServiceTest, OutOfOrderDeadlines_FireByDeadline)
{
	boost::asio::io_context ioContext;
	TimerService service(ioContext.get_executor());

	FiredOrder recorder;
	// 故意倒着挂，堆得自己排好
	service.scheduleAfter(30ms, recorder.record(30));
	service.scheduleAfter(10ms, recorder.record(10));
	service.scheduleAfter(20ms, recorder.record(20));

	ioContext.run();

	ASSERT_EQ(recorder.order.size(), 3u);
	EXPECT_EQ(recorder.order[0], 10);
	EXPECT_EQ(recorder.order[1], 20);
	EXPECT_EQ(recorder.order[2], 30);
}

TEST(TimerServiceTest, Cancel_BeforeDue_CallbackNeverFires)
{
	boost::asio::io_context ioContext;
	TimerService service(ioContext.get_executor());

	bool fired = false;
	auto id = service.scheduleAfter(10ms,
									[&fired]()
									{
										fired = true;
									});

	EXPECT_TRUE(service.cancel(id));
	EXPECT_EQ(service.pendingCount(), 0u);
	// 幂等
	EXPECT_FALSE(service.cancel(id));

	ioContext.run();

	EXPECT_FALSE(fired);
}

TEST(TimerServiceTest, CancelNeverIssuedId_ReturnsFalse)
{
	boost::asio::io_context ioContext;
	TimerService service(ioContext.get_executor());

	EXPECT_FALSE(service.cancel(TimerService::kInvalidId));
	EXPECT_FALSE(service.cancel(12345));
	EXPECT_EQ(service.pendingCount(), 0u);
}

TEST(TimerServiceTest, CancelHeapTop_OtherTimersStillFire)
{
	boost::asio::io_context ioContext;
	TimerService service(ioContext.get_executor());

	FiredOrder recorder;
	auto lateId = service.scheduleAfter(40ms, recorder.record(40));
	auto topId = service.scheduleAfter(5ms, recorder.record(5));

	EXPECT_TRUE(service.cancel(topId));
	EXPECT_EQ(service.pendingCount(), 1u);

	ioContext.run();

	ASSERT_EQ(recorder.order.size(), 1u);
	EXPECT_EQ(recorder.order[0], 40);
	EXPECT_NE(lateId, TimerService::kInvalidId);
}

TEST(TimerServiceTest, CancelSameBatchInsideCallback_DropsThatTimer)
{
	boost::asio::io_context ioContext;
	TimerService service(ioContext.get_executor());

	int first = 0;
	int second = 0;
	const auto deadline = TimerService::Clock::now() + 2ms;

	TimerService::TimerId secondId = TimerService::kInvalidId;
	service.schedule(deadline,
					 [&service, &secondId, &first]()
					 {
						 ++first;
						 service.cancel(secondId);
					 });
	secondId = service.schedule(deadline,
								[&second]()
								{
									++second;
								});

	ioContext.run();

	// 同一批已经摘出来了，取消也得拦住它
	EXPECT_EQ(first, 1);
	EXPECT_EQ(second, 0);
}

TEST(TimerServiceTest, CallbackSchedulesAnother_BothFire)
{
	boost::asio::io_context ioContext;
	TimerService service(ioContext.get_executor());

	FiredOrder recorder;
	service.scheduleAfter(1ms,
						  [&service, &recorder]()
						  {
							  recorder.order.push_back(1);
							  service.scheduleAfter(5ms, recorder.record(2));
						  });

	ioContext.run();

	ASSERT_EQ(recorder.order.size(), 2u);
	EXPECT_EQ(recorder.order[0], 1);
	EXPECT_EQ(recorder.order[1], 2);
}

TEST(TimerServiceTest, PastDeadline_FiresImmediately)
{
	boost::asio::io_context ioContext;
	TimerService service(ioContext.get_executor());

	bool fired = false;
	service.scheduleAfter(-1s,
						  [&fired]()
						  {
							  fired = true;
						  });
	service.schedule(TimerService::Clock::now() - 1ms,
					 [&fired]()
					 {
						 fired = true;
					 });

	ioContext.run();

	EXPECT_TRUE(fired);
}

TEST(TimerServiceTest, ManyTimers_SameTick_AllFire)
{
	boost::asio::io_context ioContext;
	TimerService service(ioContext.get_executor());

	constexpr std::size_t kTimerCount = 10000;
	std::size_t fired = 0;
	const auto base = TimerService::Clock::now() + 2ms;
	for (std::size_t i = 0; i < kTimerCount; ++i)
	{
		service.schedule(base + std::chrono::microseconds(i % 1000),
						 [&fired]()
						 {
							 ++fired;
						 });
	}

	ASSERT_EQ(service.pendingCount(), kTimerCount);
	ioContext.run();

	EXPECT_EQ(fired, kTimerCount);
	EXPECT_EQ(service.pendingCount(), 0u);
}

TEST(TimerServiceTest, Shutdown_PendingTimersNeverFire)
{
	boost::asio::io_context ioContext;
	TimerService service(ioContext.get_executor());

	bool fired = false;
	service.scheduleAfter(1h,
						  [&fired]()
						  {
							  fired = true;
						  });

	service.shutdown();
	EXPECT_EQ(service.pendingCount(), 0u);
	// 停了之后新挂的也不受理
	EXPECT_EQ(service.scheduleAfter(1ms,
									[&fired]()
									{
										fired = true;
									}),
			  TimerService::kInvalidId);

	ioContext.run();

	EXPECT_FALSE(fired);
}

TEST(TimerServiceTest, IoContextStoppedEarly_NoCallbackNoCrash)
{
	boost::asio::io_context ioContext;
	bool fired = false;
	{
		TimerService service(ioContext.get_executor());
		service.scheduleAfter(1h,
							  [&fired]()
							  {
								  fired = true;
							  });

		// run() 一进循环就把 stop 处理掉，那个 1 小时的定时器还挂在 timer 上
		boost::asio::post(ioContext,
						  [&ioContext]()
						  {
							  ioContext.stop();
						  });
		ioContext.run();

		EXPECT_FALSE(fired);
		EXPECT_EQ(service.pendingCount(), 1u);
	}

	EXPECT_FALSE(fired);
}

TEST(TimerServiceTest, ShutdownInsideCallback_RemainingCallbacksDropped)
{
	boost::asio::io_context ioContext;
	TimerService service(ioContext.get_executor());

	int fired = 0;
	const auto deadline = TimerService::Clock::now() + 2ms;
	service.schedule(deadline,
					 [&service, &fired]()
					 {
						 ++fired;
						 service.shutdown();
					 });
	service.schedule(deadline,
					 [&fired]()
					 {
						 ++fired;
					 });

	ioContext.run();

	EXPECT_EQ(fired, 1);
}

TEST(TimerServiceTest, ThreadLocal_BindAndUnbind_ReturnsService)
{
	boost::asio::io_context ioContext;
	{
		TimerService service(ioContext.get_executor());
		EXPECT_EQ(currentThreadTimerService(), nullptr);

		service.bindToCurrentThread();
		EXPECT_EQ(currentThreadTimerService(), &service);

		service.unbindFromCurrentThread();
		EXPECT_EQ(currentThreadTimerService(), nullptr);

		service.bindToCurrentThread();
	}
	// 析构顺手解绑，不然线程上就留了个悬空指针
	EXPECT_EQ(currentThreadTimerService(), nullptr);
}

TEST(TimerServiceTest, CallbackDestroyingService_NoCrash)
{
	boost::asio::io_context ioContext;
	auto service = std::make_unique<TimerService>(ioContext.get_executor());

	bool fired = false;
	service->scheduleAfter(2ms,
						   [&service, &fired]()
						   {
							   fired = true;
							   service.reset();
						   });

	ioContext.run();

	EXPECT_TRUE(fired);
	EXPECT_EQ(service, nullptr);
}
