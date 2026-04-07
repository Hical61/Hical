#include "core/Coroutine.h"
#include "asio/AsioEventLoop.h"
#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <thread>

using namespace hical;

// 测试协程 sleep（使用 executor 版本）
TEST(CoroutineTest, Sleep)
{
	AsioEventLoop loop;
	std::atomic<bool> executed {false};
	auto start = std::chrono::steady_clock::now();

	coSpawn(loop.getIoContext(),
			[&]() -> Awaitable<void>
			{
				co_await sleep(0.1);
				executed = true;
			});

	std::thread loopThread(
		[&loop]()
		{
			loop.run();
		});

	std::this_thread::sleep_for(std::chrono::milliseconds(200));

	EXPECT_TRUE(executed.load());

	auto elapsed =
		std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
	EXPECT_GE(elapsed, 90);

	loop.stop();
	loopThread.join();
}

// 测试 sleepFor（io_context 版本）
TEST(CoroutineTest, SleepForWithIoContext)
{
	AsioEventLoop loop;
	std::atomic<bool> executed {false};

	coSpawn(loop.getIoContext(),
			[&]() -> Awaitable<void>
			{
				co_await sleepFor(loop.getIoContext(), 0.05);
				executed = true;
			});

	std::thread loopThread(
		[&loop]()
		{
			loop.run();
		});

	std::this_thread::sleep_for(std::chrono::milliseconds(150));

	EXPECT_TRUE(executed.load());

	loop.stop();
	loopThread.join();
}

// 测试 sleepFor（chrono duration 版本）
TEST(CoroutineTest, SleepForChrono)
{
	AsioEventLoop loop;
	std::atomic<bool> executed {false};

	coSpawn(loop.getIoContext(),
			[&]() -> Awaitable<void>
			{
				co_await sleepFor(loop.getIoContext(), std::chrono::milliseconds(50));
				executed = true;
			});

	std::thread loopThread(
		[&loop]()
		{
			loop.run();
		});

	std::this_thread::sleep_for(std::chrono::milliseconds(150));

	EXPECT_TRUE(executed.load());

	loop.stop();
	loopThread.join();
}

// 测试 coSpawn 启动多个协程
TEST(CoroutineTest, MultipleCoroutines)
{
	AsioEventLoop loop;
	std::atomic<int> counter {0};

	for (int i = 0; i < 5; ++i)
	{
		coSpawn(loop.getIoContext(),
				[&counter]() -> Awaitable<void>
				{
					co_await sleep(0.01);
					counter++;
				});
	}

	std::thread loopThread(
		[&loop]()
		{
			loop.run();
		});

	std::this_thread::sleep_for(std::chrono::milliseconds(200));

	EXPECT_EQ(counter.load(), 5);

	loop.stop();
	loopThread.join();
}

// 测试协程返回值
TEST(CoroutineTest, AwaitableWithReturnValue)
{
	AsioEventLoop loop;
	std::atomic<int> result {0};

	auto compute = []() -> Awaitable<int>
	{
		co_await sleep(0.01);
		co_return 42;
	};

	coSpawn(loop.getIoContext(),
			[&result, &compute]() -> Awaitable<void>
			{
				int val = co_await compute();
				result = val;
			});

	std::thread loopThread(
		[&loop]()
		{
			loop.run();
		});

	std::this_thread::sleep_for(std::chrono::milliseconds(200));

	EXPECT_EQ(result.load(), 42);

	loop.stop();
	loopThread.join();
}
