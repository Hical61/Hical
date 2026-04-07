#include "asio/AsioTimer.h"
#include "asio/AsioEventLoop.h"
#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <thread>

using namespace hical;

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

	// 等待约 0.35 秒（应执行 3 次）
	std::this_thread::sleep_for(std::chrono::milliseconds(350));

	int count = counter.load();
	EXPECT_GE(count, 3);
	EXPECT_LE(count, 4);

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

	// 等待执行几次
	std::this_thread::sleep_for(std::chrono::milliseconds(250));

	int countBefore = counter.load();
	EXPECT_GE(countBefore, 2);

	timer->cancel();

	std::this_thread::sleep_for(std::chrono::milliseconds(200));

	// 取消后不应再增加
	EXPECT_EQ(counter.load(), countBefore);

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
