#include "core/Coroutine.h"
#include "asio/AsioEventLoop.h"
#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <thread>

using namespace hical;

namespace
{
	// 轮询等条件成立（最多 timeoutMs 毫秒）。CI runner 负载高时，"硬等固定时长
	// 再断言"不可靠——定时器触发 + 协程恢复 + 置位这一串没跑完就断言，必挂。
	template <typename Pred>
	bool waitUntil(Pred pred, int timeoutMs = 5000)
	{
		for (int waited = 0; waited < timeoutMs; waited += 5)
		{
			if (pred())
			{
				return true;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
		return pred();
	}
} // namespace

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

	EXPECT_TRUE(waitUntil(
		[&]
		{
			return executed.load();
		}));

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

	EXPECT_TRUE(waitUntil(
		[&]
		{
			return executed.load();
		}));

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

	EXPECT_TRUE(waitUntil(
		[&]
		{
			return executed.load();
		}));

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
