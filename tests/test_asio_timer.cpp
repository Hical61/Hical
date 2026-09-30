#include "asio/AsioTimer.h"
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

// 测试单次定时器
TEST(AsioTimerTest, RunOnce)
{
	AsioEventLoop loop;
	std::atomic<bool> executed {false};

	std::thread loopThread(
		[&loop]()
		{
			loop.run();
		});

	std::this_thread::sleep_for(std::chrono::milliseconds(50));

	auto timer = std::make_shared<AsioTimer>(&loop,
											 0.1,
											 [&]()
											 {
												 executed = true;
											 });

	timer->start();

	EXPECT_TRUE(timer->isActive());
	EXPECT_FALSE(timer->isRepeating());
	EXPECT_NEAR(timer->interval(), 0.1, 0.01);

	std::this_thread::sleep_for(std::chrono::milliseconds(200));

	EXPECT_TRUE(executed.load());

	loop.stop();
	loopThread.join();
}

// 测试周期定时器
TEST(AsioTimerTest, RunRepeatedly)
{
	AsioEventLoop loop;
	std::atomic<int> counter {0};

	std::thread loopThread(
		[&loop]()
		{
			loop.run();
		});

	std::this_thread::sleep_for(std::chrono::milliseconds(50));

	auto timer = std::make_shared<AsioTimer>(
		&loop,
		0.1,
		[&]()
		{
			counter++;
		},
		true);

	EXPECT_TRUE(timer->isRepeating());
	timer->start();

	// 等到至少触发 3 次。CI 上墙钟不可靠，别硬等 350ms 就断言
	ASSERT_TRUE(waitUntil(
		[&]
		{
			return counter.load() >= 3;
		}))
		<< "重复定时器没触发到 3 次";

	const int count = counter.load();
	EXPECT_GE(count, 3);
	EXPECT_LE(count, 5); // 轮询停下后最多再多触发一次

	timer->cancel();
	EXPECT_FALSE(timer->isActive());

	loop.stop();
	loopThread.join();
}

// 测试取消单次定时器
TEST(AsioTimerTest, CancelOnce)
{
	AsioEventLoop loop;
	std::atomic<bool> executed {false};

	std::thread loopThread(
		[&loop]()
		{
			loop.run();
		});

	std::this_thread::sleep_for(std::chrono::milliseconds(50));

	auto timer = std::make_shared<AsioTimer>(&loop,
											 0.2,
											 [&]()
											 {
												 executed = true;
											 });

	timer->start();

	// 立即取消
	timer->cancel();
	EXPECT_FALSE(timer->isActive());

	std::this_thread::sleep_for(std::chrono::milliseconds(300));
	EXPECT_FALSE(executed.load());

	loop.stop();
	loopThread.join();
}

// 测试取消周期定时器
TEST(AsioTimerTest, CancelRepeating)
{
	AsioEventLoop loop;
	std::atomic<int> counter {0};

	std::thread loopThread(
		[&loop]()
		{
			loop.run();
		});

	std::this_thread::sleep_for(std::chrono::milliseconds(50));

	auto timer = std::make_shared<AsioTimer>(
		&loop,
		0.1,
		[&]()
		{
			counter++;
		},
		true);

	timer->start();

	// 等到至少触发 2 次（别硬等固定时长）
	ASSERT_TRUE(waitUntil(
		[&]
		{
			return counter.load() >= 2;
		}))
		<< "重复定时器没触发到 2 次";

	timer->cancel();

	// 先让已经排上队的回调跑完再取快照。否则快照和 cancel() 之间插进来一次触发，后面的 EXPECT_EQ 会误判
	std::this_thread::sleep_for(std::chrono::milliseconds(50));
	const int countAfterCancel = counter.load();

	std::this_thread::sleep_for(std::chrono::milliseconds(200));

	// 取消后不应再增加
	EXPECT_EQ(counter.load(), countAfterCancel);

	loop.stop();
	loopThread.join();
}

// 测试定时器精度
TEST(AsioTimerTest, TimingPrecision)
{
	AsioEventLoop loop;
	auto start = std::chrono::steady_clock::now();
	std::atomic<bool> executed {false};
	std::chrono::steady_clock::time_point execTime;

	std::thread loopThread(
		[&loop]()
		{
			loop.run();
		});

	std::this_thread::sleep_for(std::chrono::milliseconds(50));

	auto timer = std::make_shared<AsioTimer>(&loop,
											 0.1,
											 [&]()
											 {
												 execTime = std::chrono::steady_clock::now();
												 executed = true;
											 });

	start = std::chrono::steady_clock::now();
	timer->start();

	std::this_thread::sleep_for(std::chrono::milliseconds(200));

	EXPECT_TRUE(executed.load());

	auto delay = std::chrono::duration_cast<std::chrono::milliseconds>(execTime - start).count();

	// 允许 ±50ms 误差
	EXPECT_GE(delay, 80);
	EXPECT_LE(delay, 150);

	loop.stop();
	loopThread.join();
}

// 测试通过 EventLoop 接口使用定时器
TEST(AsioTimerTest, ViaEventLoop)
{
	AsioEventLoop loop;
	std::atomic<bool> executed {false};

	std::thread loopThread(
		[&loop]()
		{
			loop.run();
		});

	std::this_thread::sleep_for(std::chrono::milliseconds(50));

	TimerId id = loop.runAfter(0.1,
							   [&]()
							   {
								   executed = true;
							   });

	EXPECT_NE(id, hInvalidTimerId);

	std::this_thread::sleep_for(std::chrono::milliseconds(200));
	EXPECT_TRUE(executed.load());

	loop.stop();
	loopThread.join();
}

// 测试通过 EventLoop 取消定时器
TEST(AsioTimerTest, CancelViaEventLoop)
{
	AsioEventLoop loop;
	std::atomic<bool> executed {false};

	std::thread loopThread(
		[&loop]()
		{
			loop.run();
		});

	std::this_thread::sleep_for(std::chrono::milliseconds(50));

	TimerId id = loop.runAfter(0.2,
							   [&]()
							   {
								   executed = true;
							   });

	loop.cancelTimer(id);

	std::this_thread::sleep_for(std::chrono::milliseconds(300));
	EXPECT_FALSE(executed.load());

	loop.stop();
	loopThread.join();
}

// 测试 getLoop
TEST(AsioTimerTest, GetLoop)
{
	AsioEventLoop loop;
	auto timer = std::make_shared<AsioTimer>(&loop,
											 1.0,
											 []()
											 {
											 });

	EXPECT_EQ(timer->getLoop(), &loop);
}
