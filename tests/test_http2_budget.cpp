/**
 * @file test_http2_budget.cpp
 * @brief HTTP/2 资源额度与租约生命周期验证
 */

#include "core/http2/Http2Budget.h"
#include <gtest/gtest.h>
#include <atomic>
#include <barrier>
#include <limits>
#include <memory>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

using hical::Http2Options;
using hical::detail::Http2Budget;
using hical::detail::Http2ServiceBudget;

namespace
{
	Http2Options smallOptions()
	{
		Http2Options options;
		options.maxConcurrentStreams = 2;
		options.maxApplicationTasks = 2;
		options.maxServerApplicationTasks = 3;
		options.maxBodyBytes = 4;
		options.maxConnectionReceiveBytes = 6;
		options.maxConnectionSendBytes = 8;
		options.maxOutputBatchBytes = 4;
		options.maxFilePrefetchBytes = 4;
		options.maxResponseBytes = 12;
		options.maxClientConnectionReceiveBytes = 16;
		return options;
	}
} // namespace

static_assert(!std::is_copy_constructible_v<Http2Budget::Lease>);
static_assert(!std::is_copy_assignable_v<Http2Budget::Lease>);
static_assert(std::is_nothrow_move_constructible_v<Http2Budget::Lease>);
static_assert(std::is_nothrow_move_assignable_v<Http2Budget::Lease>);

TEST(Http2BudgetTest, ProtocolSlotsExhaustAndRecoverIndependently)
{
	Http2ServiceBudget service(3);
	Http2Budget budget(Http2Budget::Role::hServer, smallOptions(), &service);
	auto first = budget.tryAcquireProtocolStream();
	auto second = budget.tryAcquireProtocolStream();
	ASSERT_TRUE(first);
	ASSERT_TRUE(second);
	EXPECT_FALSE(budget.tryAcquireProtocolStream());
	EXPECT_EQ(budget.usage().protocolStreams, 2U);
	EXPECT_EQ(budget.usage().applicationTasks, 0U);
	EXPECT_EQ(service.used(), 0U);
	first.reset();
	EXPECT_TRUE(budget.tryAcquireProtocolStream());
	EXPECT_EQ(budget.usage().protocolStreams, 1U);
}

TEST(Http2BudgetTest, PeerLimitCanPauseNewClientStreams)
{
	Http2Budget budget(Http2Budget::Role::hClient, smallOptions());
	EXPECT_FALSE(budget.tryAcquireProtocolStream(0));
	auto stream = budget.tryAcquireProtocolStream(1);
	ASSERT_TRUE(stream);
	EXPECT_FALSE(budget.tryAcquireProtocolStream(1));
	EXPECT_TRUE(budget.tryAcquireProtocolStream(3));
	EXPECT_EQ(budget.usage().protocolStreams, 1U);
}

TEST(Http2BudgetTest, PeerOutboundLimitDoesNotRestrictServerInboundStreams)
{
	Http2ServiceBudget service(3);
	Http2Budget budget(Http2Budget::Role::hServer, smallOptions(), &service);
	auto first = budget.tryAcquireProtocolStream(0);
	auto second = budget.tryAcquireProtocolStream(0);
	ASSERT_TRUE(first);
	ASSERT_TRUE(second);
	EXPECT_FALSE(budget.tryAcquireProtocolStream(0));
	EXPECT_EQ(budget.usage().protocolStreams, 2U);
}

TEST(Http2BudgetTest, ApplicationSlotsEnforceConnectionAndServiceLimits)
{
	Http2ServiceBudget service(3);
	Http2Budget first(Http2Budget::Role::hServer, smallOptions(), &service);
	Http2Budget second(Http2Budget::Role::hServer, smallOptions(), &service);
	auto firstTask = first.tryAcquireApplicationTask();
	auto secondTask = first.tryAcquireApplicationTask();
	ASSERT_TRUE(firstTask);
	ASSERT_TRUE(secondTask);
	EXPECT_FALSE(first.tryAcquireApplicationTask());
	auto thirdTask = second.tryAcquireApplicationTask();
	ASSERT_TRUE(thirdTask);
	EXPECT_FALSE(second.tryAcquireApplicationTask());
	EXPECT_EQ(second.usage().applicationTasks, 1U);
	EXPECT_EQ(service.used(), 3U);
	EXPECT_EQ(service.limit(), 3U);
	firstTask.reset();
	EXPECT_TRUE(second.tryAcquireApplicationTask());
	EXPECT_EQ(service.used(), 2U);
}

TEST(Http2BudgetTest, ReceiveReservationsCheckStreamAndConnectionBeforeGrowth)
{
	Http2ServiceBudget service(3);
	Http2Budget budget(Http2Budget::Role::hServer, smallOptions(), &service);
	auto first = budget.tryAcquireReceive(4);
	auto second = budget.tryAcquireReceive(2);
	ASSERT_TRUE(first);
	ASSERT_TRUE(second);
	EXPECT_FALSE(budget.tryAcquireReceive(1));
	EXPECT_FALSE(budget.tryAcquireReceive(5));
	EXPECT_FALSE(first.tryResize(5));
	EXPECT_FALSE(second.tryResize(3));
	EXPECT_EQ(second.amount(), 2U);
	EXPECT_EQ(budget.usage().receiveBytes, 6U);
	ASSERT_TRUE(first.tryResize(2));
	ASSERT_TRUE(second.tryResize(4));
	EXPECT_EQ(budget.usage().receiveBytes, 6U);
	second.reset();
	EXPECT_EQ(budget.usage().receiveBytes, 2U);
	EXPECT_TRUE(budget.tryAcquireReceive(4));
}

TEST(Http2BudgetTest, ClientReceivesUseResponseLimits)
{
	Http2Budget budget(Http2Budget::Role::hClient, smallOptions());
	auto first = budget.tryAcquireReceive(12);
	auto second = budget.tryAcquireReceive(4);
	ASSERT_TRUE(first);
	ASSERT_TRUE(second);
	EXPECT_FALSE(budget.tryAcquireReceive(1));
	EXPECT_FALSE(first.tryResize(13));
	EXPECT_EQ(budget.usage().receiveBytes, 16U);
	first.reset();
	EXPECT_TRUE(budget.tryAcquireReceive(12));
}

TEST(Http2BudgetTest, SendPayloadIsIndependentAndReturnsOnShrink)
{
	Http2Budget budget(Http2Budget::Role::hClient, smallOptions());
	auto receive = budget.tryAcquireReceive(12);
	auto first = budget.tryAcquireSend(6);
	auto second = budget.tryAcquireSend(2);
	ASSERT_TRUE(receive);
	ASSERT_TRUE(first);
	ASSERT_TRUE(second);
	EXPECT_FALSE(first.tryResize(7));
	EXPECT_FALSE(budget.tryAcquireSend(1));
	EXPECT_EQ(first.amount(), 6U);
	ASSERT_TRUE(first.tryResize(3));
	ASSERT_TRUE(second.tryResize(5));
	EXPECT_EQ(budget.usage().sendBytes, 8U);
	EXPECT_EQ(budget.usage().receiveBytes, 12U);
}

TEST(Http2BudgetTest, EmptyPayloadLeaseCanGrowAndReturnToZero)
{
	Http2Budget budget(Http2Budget::Role::hClient, smallOptions());
	auto payload = budget.tryAcquireSend();
	ASSERT_TRUE(payload);
	EXPECT_EQ(payload.amount(), 0U);
	ASSERT_TRUE(payload.tryResize(8));
	ASSERT_TRUE(payload.tryResize(0));
	EXPECT_TRUE(payload);
	EXPECT_EQ(budget.usage().sendBytes, 0U);
	payload.reset();
	EXPECT_FALSE(payload.tryResize(1));
}

TEST(Http2BudgetTest, MovesAndRepeatedResetReturnEachReservationOnce)
{
	Http2Budget budget(Http2Budget::Role::hClient, smallOptions());
	auto first = budget.tryAcquireSend(3);
	auto second = budget.tryAcquireSend(5);
	auto moved = std::move(first);
	first.reset();
	EXPECT_FALSE(first);
	EXPECT_EQ(first.amount(), 0U);
	EXPECT_EQ(budget.usage().sendBytes, 8U);
	second = std::move(moved);
	EXPECT_FALSE(moved);
	EXPECT_EQ(second.amount(), 3U);
	EXPECT_EQ(budget.usage().sendBytes, 3U);
	auto& leaseAlias = second;
	second = std::move(leaseAlias);
	EXPECT_EQ(budget.usage().sendBytes, 3U);
	second.reset();
	second.reset();
	EXPECT_EQ(budget.usage().sendBytes, 0U);
}

TEST(Http2BudgetTest, SlotLeaseCannotChangeItsReservationSize)
{
	Http2Budget budget(Http2Budget::Role::hClient, smallOptions());
	auto protocol = budget.tryAcquireProtocolStream();
	auto task = budget.tryAcquireApplicationTask();
	EXPECT_FALSE(protocol.tryResize(0));
	EXPECT_FALSE(task.tryResize(2));
	EXPECT_EQ(budget.usage().protocolStreams, 1U);
	EXPECT_EQ(budget.usage().applicationTasks, 1U);
}

TEST(Http2BudgetTest, MovingTasksAcrossConnectionsReturnsReplacedServiceSlot)
{
	Http2ServiceBudget service(2);
	Http2Budget first(Http2Budget::Role::hServer, smallOptions(), &service);
	Http2Budget second(Http2Budget::Role::hServer, smallOptions(), &service);
	auto firstTask = first.tryAcquireApplicationTask();
	auto secondTask = second.tryAcquireApplicationTask();
	ASSERT_TRUE(firstTask);
	ASSERT_TRUE(secondTask);
	firstTask = std::move(secondTask);
	EXPECT_EQ(first.usage().applicationTasks, 0U);
	EXPECT_EQ(second.usage().applicationTasks, 1U);
	EXPECT_EQ(service.used(), 1U);
	secondTask.reset();
	firstTask.reset();
	EXPECT_EQ(second.usage().applicationTasks, 0U);
	EXPECT_EQ(service.used(), 0U);
}

TEST(Http2BudgetTest, ResetReturnsOnlyProtocolSlotUntilApplicationExits)
{
	auto options = smallOptions();
	options.maxApplicationTasks = 1;
	Http2ServiceBudget service(1);
	Http2Budget budget(Http2Budget::Role::hServer, options, &service);
	auto protocol = budget.tryAcquireProtocolStream();
	auto runningTask = budget.tryAcquireApplicationTask();
	auto requestMemory = budget.tryAcquireReceive(4);
	ASSERT_TRUE(protocol);
	ASSERT_TRUE(runningTask);
	ASSERT_TRUE(requestMemory);
	protocol.reset();
	protocol.reset();
	EXPECT_TRUE(budget.tryAcquireProtocolStream());
	EXPECT_FALSE(budget.tryAcquireApplicationTask());
	EXPECT_EQ(budget.usage().receiveBytes, 4U);
	EXPECT_EQ(service.used(), 1U);
	runningTask.reset();
	EXPECT_EQ(service.used(), 0U);
	EXPECT_EQ(budget.usage().receiveBytes, 4U);
	requestMemory.reset();
	EXPECT_EQ(budget.usage().receiveBytes, 0U);
}

TEST(Http2BudgetTest, LeaseCanOutliveConnectionAndServiceFacades)
{
	Http2Budget::Lease runningTask;
	Http2Budget::Lease payload;
	{
		Http2ServiceBudget service(1);
		{
			Http2Budget budget(Http2Budget::Role::hServer, smallOptions(), &service);
			runningTask = budget.tryAcquireApplicationTask();
			payload = budget.tryAcquireReceive(3);
		}
		EXPECT_EQ(service.used(), 1U);
		ASSERT_TRUE(payload.tryResize(4));
	}
	ASSERT_TRUE(payload.tryResize(2));
	runningTask.reset();
	runningTask.reset();
	payload.reset();
}

TEST(Http2BudgetTest, MaximumByteLimitsDoNotOverflowOnReserveOrGrowth)
{
	auto options = smallOptions();
	constexpr auto maxBytes = std::numeric_limits<size_t>::max();
	options.maxBodyBytes = maxBytes;
	options.maxConnectionReceiveBytes = maxBytes;
	options.maxConnectionSendBytes = maxBytes;
	Http2ServiceBudget service(1);
	Http2Budget budget(Http2Budget::Role::hServer, options, &service);
	auto first = budget.tryAcquireReceive(maxBytes - 1);
	auto second = budget.tryAcquireReceive(1);
	ASSERT_TRUE(first);
	ASSERT_TRUE(second);
	EXPECT_FALSE(budget.tryAcquireReceive(1));
	EXPECT_FALSE(first.tryResize(maxBytes));
	EXPECT_EQ(first.amount(), maxBytes - 1);
	EXPECT_EQ(budget.usage().receiveBytes, maxBytes);
	second.reset();
	ASSERT_TRUE(first.tryResize(maxBytes));
	EXPECT_EQ(budget.usage().receiveBytes, maxBytes);
	first.reset();
	EXPECT_EQ(budget.usage().receiveBytes, 0U);
}

TEST(Http2BudgetTest, RejectsInvalidConfigurationAndMissingServerBudget)
{
	EXPECT_THROW(Http2ServiceBudget(0), std::invalid_argument);
	EXPECT_THROW(Http2Budget(Http2Budget::Role::hServer, smallOptions()), std::invalid_argument);
	auto options = smallOptions();
	options.maxApplicationTasks = 0;
	EXPECT_THROW(Http2Budget(Http2Budget::Role::hClient, options), std::invalid_argument);
	EXPECT_THROW(Http2Budget(static_cast<Http2Budget::Role>(255), smallOptions()), std::invalid_argument);
	Http2ServiceBudget service(1);
	auto moved = std::move(service);
	EXPECT_THROW(Http2Budget(Http2Budget::Role::hServer, smallOptions(), &service), std::invalid_argument);
	EXPECT_EQ(moved.used(), 0U);
}

TEST(Http2BudgetTest, SharedServiceLimitHoldsAcrossWorkerThreads)
{
	constexpr size_t kWorkers = 8;
	constexpr size_t kServiceLimit = 3;
	Http2ServiceBudget service(kServiceLimit);
	std::barrier phase(static_cast<std::ptrdiff_t>(kWorkers + 1));
	std::atomic<size_t> acquired {0};
	std::vector<std::jthread> workers;
	for (size_t index = 0; index < kWorkers; ++index)
	{
		workers.emplace_back(
			[&]
			{
				Http2Budget budget(Http2Budget::Role::hServer, smallOptions(), &service);
				phase.arrive_and_wait();
				auto task = budget.tryAcquireApplicationTask();
				if (task)
				{
					acquired.fetch_add(1, std::memory_order_relaxed);
				}
				phase.arrive_and_wait();
				phase.arrive_and_wait();
			});
	}
	phase.arrive_and_wait();
	phase.arrive_and_wait();
	EXPECT_EQ(acquired.load(std::memory_order_relaxed), kServiceLimit);
	EXPECT_EQ(service.used(), kServiceLimit);
	phase.arrive_and_wait();
	workers.clear();
	EXPECT_EQ(service.used(), 0U);
}
