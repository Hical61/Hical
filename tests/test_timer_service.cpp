/**
 * @file test_timer_service.cpp
 * @brief 集中式定时器服务的单元测试
 */

#include "core/Coroutine.h"
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

// ---------- sleep() 走哪条路 ----------
// 两条路径都得能等到时长，区别只在「有没有往服务里挂定时器」

TEST(TimerSleepTest, ServiceBound_RegistersInServiceAndWaits)
{
	boost::asio::io_context ioContext;
	TimerService service(ioContext.get_executor());
	service.bindToCurrentThread();

	bool hadService = false;
	bool probeSawSleepTimer = false;
	std::chrono::steady_clock::duration slept {};

	coSpawn(ioContext,
			[&]() -> Awaitable<void>
			{
				hadService = currentThreadTimerService() == &service;
				const auto start = TimerService::Clock::now();
				co_await sleep(30ms);
				slept = TimerService::Clock::now() - start;
			});

	// 探针 5ms 到点：那会儿 sleep 还该挂在服务里，pendingCount 是 1
	service.scheduleAfter(5ms,
						  [&]()
						  {
							  probeSawSleepTimer = service.pendingCount() == 1;
						  });

	ioContext.run();

	EXPECT_TRUE(hadService);
	EXPECT_TRUE(probeSawSleepTimer) << "sleep 应该挂到 TimerService 上，而不是自己造 timer";
	EXPECT_GE(slept, 20ms); // 定时器可能早个把 tick，留点余量
	EXPECT_EQ(service.pendingCount(), 0u);
}

TEST(TimerSleepTest, NoService_FallsBackToOwnTimerAndWaits)
{
	boost::asio::io_context ioContext;
	ASSERT_EQ(currentThreadTimerService(), nullptr);

	bool hadService = true;
	std::chrono::steady_clock::duration slept {};

	coSpawn(ioContext,
			[&]() -> Awaitable<void>
			{
				hadService = currentThreadTimerService() != nullptr;
				const auto start = TimerService::Clock::now();
				co_await sleep(20ms);
				slept = TimerService::Clock::now() - start;
			});

	ioContext.run();

	EXPECT_FALSE(hadService);
	EXPECT_GE(slept, 10ms);
}

TEST(TimerSleepTest, ServiceStopped_FallsBackInsteadOfReturningImmediately)
{
	boost::asio::io_context ioContext;
	TimerService service(ioContext.get_executor());
	service.bindToCurrentThread();
	service.shutdown();

	std::chrono::steady_clock::duration slept {};
	coSpawn(ioContext,
			[&]() -> Awaitable<void>
			{
				const auto start = TimerService::Clock::now();
				co_await sleep(20ms);
				slept = TimerService::Clock::now() - start;
			});

	ioContext.run();

	// 停了就立刻返回的话，accept 循环里那个 while 会变成空转
	EXPECT_GE(slept, 10ms);
}

TEST(TimerServiceTest, CancelArmedHeapTop_SurvivingTimerNotFiredEarly)
{
	boost::asio::io_context ioContext;
	TimerService service(ioContext.get_executor());

	bool laterFired = false;
	service.scheduleAfter(150ms,
						  [&laterFired]()
						  {
							  laterFired = true;
						  });
	// 这个挂进来的时候堆顶是它，timer 已经被改到 5ms。取消它之后 TimerService 不会
	// 主动重挂（堆顶一定是到期最早的，取消堆顶不会让别的项提前），所以真正的考验是
	// 到点那一刻 fireDue 拿到新堆顶后有没有把 timer 挪到 150ms 去
	auto topId = service.scheduleAfter(5ms,
									   []()
									   {
									   });
	EXPECT_TRUE(service.cancel(topId));
	EXPECT_EQ(service.pendingCount(), 1u);

	ioContext.run_for(60ms);
	EXPECT_FALSE(laterFired) << "堆顶取消之后不该让后面的项提前触发";
	EXPECT_EQ(service.pendingCount(), 1u);

	ioContext.run_for(200ms);
	EXPECT_TRUE(laterFired);
	EXPECT_EQ(service.pendingCount(), 0u);
}

TEST(TimerServiceTest, LaterDeadlineScheduled_EarlierArmedStayOnTime)
{
	boost::asio::io_context ioContext;
	TimerService service(ioContext.get_executor());

	bool earlyFired = false;
	bool lateFired = false;
	service.scheduleAfter(2ms,
						  [&earlyFired]()
						  {
							  earlyFired = true;
						  });
	// 后挂的这个晚得多。rearm 看到堆里已经挂着更早的就不该动 timer，
	// 动了的话 2ms 那个会被推到 600ms 才回调
	service.scheduleAfter(600ms,
						  [&lateFired]()
						  {
							  lateFired = true;
						  });

	ioContext.run_for(200ms);
	EXPECT_TRUE(earlyFired);
	EXPECT_EQ(service.pendingCount(), 1u);

	ioContext.run_for(600ms);
	EXPECT_TRUE(lateFired);
	EXPECT_EQ(service.pendingCount(), 0u);
}

TEST(TimerServiceTest, EarlierTimerScheduledLater_PullsArmedDeadlineForward)
{
	boost::asio::io_context ioContext;
	TimerService service(ioContext.get_executor());

	bool earlyFired = false;
	bool lateFired = false;
	// 先把唯一的 timer 挂到 3 秒去
	service.scheduleAfter(3s,
						  [&lateFired]()
						  {
							  lateFired = true;
						  });
	// 再挂一个马上到期的，堆顶比挂着的那条早，rearm 必须把 timer 往前挪
	service.scheduleAfter(2ms,
						  [&earlyFired]()
						  {
							  earlyFired = true;
						  });

	ioContext.run_for(150ms);

	EXPECT_TRUE(earlyFired) << "更早的定时器挂进来之后 timer 没往前挪，得等到 3 秒才回调";
	EXPECT_FALSE(lateFired);
	EXPECT_EQ(service.pendingCount(), 1u);
}

TEST(TimerServiceTest, CancelLaterTimerInsideCallback_ThatTimerNeverFires)
{
	boost::asio::io_context ioContext;
	TimerService service(ioContext.get_executor());

	int fired = 0;
	TimerService::TimerId laterId = TimerService::kInvalidId;
	service.scheduleAfter(2ms,
						  [&service, &laterId, &fired]()
						  {
							  ++fired;
							  // 这一批只摘出来了自己，后面的项还在堆里躺着
							  service.cancel(laterId);
						  });
	laterId = service.scheduleAfter(40ms,
									[&fired]()
									{
										++fired;
									});

	ioContext.run_for(200ms);

	EXPECT_EQ(fired, 1);
	EXPECT_EQ(service.pendingCount(), 0u);
}

TEST(TimerServiceTest, CancelFiredIds_CompactionDropsGhostsKeepsLiveMarks)
{
	boost::asio::io_context ioContext;
	TimerService service(ioContext.get_executor());

	constexpr int kCount = 300;

	// 先烧掉 300 个定时器，拿到一批「已经触发过」的 id。拿它们去 cancel
	// 只会留下没人清的幽灵标记
	std::vector<TimerService::TimerId> firedIds;
	firedIds.reserve(kCount);
	for (int i = 0; i < kCount; ++i)
	{
		firedIds.push_back(service.scheduleAfter(0ms,
												 []()
												 {
												 }));
	}
	ioContext.run_for(50ms);
	ASSERT_EQ(service.pendingCount(), 0u);
	// run_for 把活儿干完之后 io_context 是 stopped 的，不重启后面再 run 就立刻返回
	ioContext.restart();

	// 再挂 300 个然后全取消：取消标记和堆里节点一样多，压不出重建
	int firedAfterCancel = 0;
	std::vector<TimerService::TimerId> cancelledIds;
	cancelledIds.reserve(kCount);
	for (int i = 0; i < kCount; ++i)
	{
		cancelledIds.push_back(service.scheduleAfter(100ms,
													 [&firedAfterCancel]()
													 {
														 ++firedAfterCancel;
													 }));
	}
	for (auto id : cancelledIds)
	{
		EXPECT_TRUE(service.cancel(id));
	}
	EXPECT_EQ(service.pendingCount(), 0u);

	// 幽灵标记攒够 257 个就把 cancelled 顶到 heap.size() + 256 以上，触发重建
	for (auto id : firedIds)
	{
		EXPECT_TRUE(service.cancel(id));
	}

	// 重建之后最早那个幽灵 id 已经从表里扔掉，能再取消一次；不重建的话表只涨不清，
	// 这里会拿到 false
	ASSERT_EQ(service.pendingCount(), 0u);
	EXPECT_TRUE(service.cancel(firedIds.front())) << "幽灵标记没被回收，cancelled 表会一直涨";

	// 哨兵：证明最后这段 run_for 真的等到了 100ms 之后，不是上下文空转一遍就返回
	bool sentinelFired = false;
	service.scheduleAfter(150ms,
						  [&sentinelFired]()
						  {
							  sentinelFired = true;
						  });
	ASSERT_EQ(service.pendingCount(), 1u);

	// 重建按堆重建，堆里那些还活着的取消标记不能一起扔掉
	ioContext.run_for(300ms);
	EXPECT_TRUE(sentinelFired) << "这段 run_for 没真跑起来，下面的断言等于没测";
	EXPECT_EQ(firedAfterCancel, 0) << "重建把还挂在堆里的取消标记丢了";
	EXPECT_EQ(service.pendingCount(), 0u);
}

TEST(TimerServiceTest, CancelFiredId_DoesNotEatPendingCount)
{
	boost::asio::io_context ioContext;
	TimerService service(ioContext.get_executor());

	// 先烧掉一个，手上拿到个「已经触发过」的 id
	auto firedId = service.scheduleAfter(0ms,
										 []()
										 {
										 });
	ioContext.run_for(50ms);
	ASSERT_EQ(service.pendingCount(), 0u);
	ioContext.restart();

	// 再挂一个真定时器，待触发计数是 1
	bool laterFired = false;
	service.scheduleAfter(20ms,
						  [&laterFired]()
						  {
							  laterFired = true;
						  });
	ASSERT_EQ(service.pendingCount(), 1u);

	// 拿已经触发过的 id 去取消：标记照插、照约定返回 true，但那个定时器早就不是待触发了
	EXPECT_TRUE(service.cancel(firedId));
	EXPECT_EQ(service.pendingCount(), 1u) << "取消一个已经触发过的 id 不该动待触发计数";

	ioContext.run_for(200ms);
	EXPECT_TRUE(laterFired);
	// 计量靠维护计数器的话，上面那次取消先扣成 0，这里真定时器触发再扣一次就下溢成 SIZE_MAX
	EXPECT_EQ(service.pendingCount(), 0u);
}

TEST(TimerServiceTest, Shutdown_ThenCancelAndSchedule_AllRejected)
{
	boost::asio::io_context ioContext;
	TimerService service(ioContext.get_executor());

	int fired = 0;
	auto id = service.scheduleAfter(1h,
									[&fired]()
									{
										++fired;
									});
	service.shutdown();

	EXPECT_EQ(service.pendingCount(), 0u);
	EXPECT_FALSE(service.cancel(id)) << "停了之后 cancel 一律不认";
	EXPECT_EQ(service.scheduleAfter(1ms,
									[&fired]()
									{
										++fired;
									}),
			  TimerService::kInvalidId);

	// 重复停不该出问题
	service.shutdown();

	ioContext.run();
	EXPECT_EQ(fired, 0);
	EXPECT_EQ(service.pendingCount(), 0u);
}

TEST(TimerServiceTest, TwoServicesSameIoContext_StateIndependent)
{
	boost::asio::io_context ioContext;
	TimerService first(ioContext.get_executor());
	TimerService second(ioContext.get_executor());

	FiredOrder recorder;
	auto firstId = first.scheduleAfter(2ms, recorder.record(1));
	auto secondId = second.scheduleAfter(4ms, recorder.record(2));

	ASSERT_EQ(first.pendingCount(), 1u);
	ASSERT_EQ(second.pendingCount(), 1u);

	EXPECT_TRUE(first.cancel(firstId));
	EXPECT_EQ(first.pendingCount(), 0u);
	EXPECT_EQ(second.pendingCount(), 1u) << "取消第一个服务的定时器不该动到第二个";

	ioContext.run();

	ASSERT_EQ(recorder.order.size(), 1u);
	EXPECT_EQ(recorder.order[0], 2);
	EXPECT_NE(secondId, TimerService::kInvalidId);
	EXPECT_EQ(second.pendingCount(), 0u);
}

TEST(TimerServiceTest, UnbindOneOfTwoBoundServices_KeepsTheOther)
{
	boost::asio::io_context ioContext;
	TimerService first(ioContext.get_executor());
	TimerService second(ioContext.get_executor());

	first.bindToCurrentThread();
	second.bindToCurrentThread();
	ASSERT_EQ(currentThreadTimerService(), &second);

	// 后绑的覆盖了先绑的；先绑的那个解绑时不该把别人绑的也抹掉
	first.unbindFromCurrentThread();
	EXPECT_EQ(currentThreadTimerService(), &second);

	second.unbindFromCurrentThread();
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
