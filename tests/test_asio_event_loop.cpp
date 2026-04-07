#include "asio/AsioEventLoop.h"
#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <thread>

using namespace hical;

// 测试基本的事件循环启动和退出
TEST(AsioEventLoopTest, BasicLoopStartStop)
{
	AsioEventLoop loop;
	EXPECT_FALSE(loop.isRunning());

	std::thread loopThread(
		[&loop]()
		{
			loop.run();
		});

	// 等待循环启动
	std::this_thread::sleep_for(std::chrono::milliseconds(100));
	EXPECT_TRUE(loop.isRunning());

	// 退出循环
	loop.stop();
	loopThread.join();

	EXPECT_FALSE(loop.isRunning());
}

// 测试 post（总是异步投递）
TEST(AsioEventLoopTest, Post)
{
	AsioEventLoop loop;
	std::atomic<int> counter {0};

	std::thread loopThread(
		[&loop]()
		{
			loop.run();
		});

	// 投递 10 个任务
	for (int i = 0; i < 10; ++i)
	{
		loop.post(
			[&counter]()
			{
				counter++;
			});
	}

	// 等待任务执行
	std::this_thread::sleep_for(std::chrono::milliseconds(100));

	EXPECT_EQ(counter.load(), 10);

	loop.stop();
	loopThread.join();
}

// 测试 dispatch（同线程直接执行）
TEST(AsioEventLoopTest, Dispatch)
{
	AsioEventLoop loop;
	std::atomic<int> counter {0};
	std::atomic<bool> inLoopThread {false};

	std::thread loopThread(
		[&]()
		{
			loop.run();
		});

	std::this_thread::sleep_for(std::chrono::milliseconds(50));

	// 跨线程调用 dispatch（应该投递到队列）
	loop.dispatch(
		[&]()
		{
			counter++;
			inLoopThread = loop.isInLoopThread();
		});

	std::this_thread::sleep_for(std::chrono::milliseconds(100));

	EXPECT_EQ(counter.load(), 1);
	EXPECT_TRUE(inLoopThread.load());

	loop.stop();
	loopThread.join();
}

// 测试 runAfter（延迟执行）
TEST(AsioEventLoopTest, RunAfter)
{
	AsioEventLoop loop;
	std::atomic<bool> executed {false};
	auto start = std::chrono::steady_clock::now();

	std::thread loopThread(
		[&loop]()
		{
			loop.run();
		});

	std::this_thread::sleep_for(std::chrono::milliseconds(50));

	// 延迟 0.2 秒执行
	loop.runAfter(0.2,
				  [&]()
				  {
					  executed = true;
				  });

	// 等待定时器触发
	std::this_thread::sleep_for(std::chrono::milliseconds(300));

	auto end = std::chrono::steady_clock::now();
	auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

	EXPECT_TRUE(executed.load());
	EXPECT_GE(duration, 200); // 至少延迟了 200ms

	loop.stop();
	loopThread.join();
}

// 测试 runEvery（周期执行）
TEST(AsioEventLoopTest, RunEvery)
{
	AsioEventLoop loop;
	std::atomic<int> counter {0};

	std::thread loopThread(
		[&loop]()
		{
			loop.run();
		});

	std::this_thread::sleep_for(std::chrono::milliseconds(50));

	// 每 0.1 秒执行一次
	TimerId timerId = loop.runEvery(0.1,
									[&]()
									{
										counter++;
									});

	// 等待约 0.35 秒（应该执行 3 次）
	std::this_thread::sleep_for(std::chrono::milliseconds(350));

	int count = counter.load();
	EXPECT_GE(count, 3);
	EXPECT_LE(count, 4); // 允许误差

	// 取消定时器
	loop.cancelTimer(timerId);
	std::this_thread::sleep_for(std::chrono::milliseconds(200));

	// 计数器不应再增加
	EXPECT_EQ(counter.load(), count);

	loop.stop();
	loopThread.join();
}

// 测试 cancelTimer（取消定时器）
TEST(AsioEventLoopTest, CancelTimer)
{
	AsioEventLoop loop;
	std::atomic<bool> executed {false};

	std::thread loopThread(
		[&loop]()
		{
			loop.run();
		});

	std::this_thread::sleep_for(std::chrono::milliseconds(50));

	// 创建定时器
	TimerId timerId = loop.runAfter(0.2,
									[&]()
									{
										executed = true;
									});

	// 立即取消
	loop.cancelTimer(timerId);

	// 等待超过定时器时间
	std::this_thread::sleep_for(std::chrono::milliseconds(300));

	// 不应该执行
	EXPECT_FALSE(executed.load());

	loop.stop();
	loopThread.join();
}

// 测试 runOnQuit（退出回调）
TEST(AsioEventLoopTest, RunOnQuit)
{
	AsioEventLoop loop;
	std::atomic<int> quitCounter {0};

	loop.runOnQuit(
		[&]()
		{
			quitCounter++;
		});

	loop.runOnQuit(
		[&]()
		{
			quitCounter++;
		});

	std::thread loopThread(
		[&loop]()
		{
			loop.run();
		});

	std::this_thread::sleep_for(std::chrono::milliseconds(50));

	loop.stop();
	loopThread.join();

	// 退出回调应该执行
	EXPECT_EQ(quitCounter.load(), 2);
}

// 测试线程安全（多线程投递任务）
TEST(AsioEventLoopTest, ThreadSafety)
{
	AsioEventLoop loop;
	std::atomic<int> counter {0};

	std::thread loopThread(
		[&loop]()
		{
			loop.run();
		});

	std::this_thread::sleep_for(std::chrono::milliseconds(50));

	// 10 个线程同时投递任务
	std::vector<std::thread> threads;
	for (int i = 0; i < 10; ++i)
	{
		threads.emplace_back(
			[&]()
			{
				for (int j = 0; j < 100; ++j)
				{
					loop.post(
						[&]()
						{
							counter++;
						});
				}
			});
	}

	for (auto& t : threads)
	{
		t.join();
	}

	// 等待所有任务执行
	std::this_thread::sleep_for(std::chrono::milliseconds(200));

	EXPECT_EQ(counter.load(), 1000);

	loop.stop();
	loopThread.join();
}

// 测试索引设置
TEST(AsioEventLoopTest, IndexGetSet)
{
	AsioEventLoop loop;
	EXPECT_EQ(loop.index(), 0);

	loop.setIndex(42);
	EXPECT_EQ(loop.index(), 42);
}

// 测试 pmr 分配器
TEST(AsioEventLoopTest, PmrAllocator)
{
	AsioEventLoop loop;
	auto allocator = loop.allocator();

	// 验证分配器可用
	std::pmr::vector<int> vec(allocator);
	vec.push_back(1);
	vec.push_back(2);
	vec.push_back(3);

	EXPECT_EQ(vec.size(), 3);
	EXPECT_EQ(vec[0], 1);
}
