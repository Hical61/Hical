/**
 * @file test_periodic_task.cpp
 * @brief 周期性任务接口层测试（注册、参数校验、时序守卫）
 */

#include "core/HttpServer.h"
#include <boost/asio.hpp>
#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace hical;
using boost::asio::ip::tcp;

namespace
{
	// 每次触发啥也不干的协程体，接口层测试只关心能不能存下来
	Awaitable<void> noopCallback()
	{
		co_return;
	}

	// 轮询等条件成立，别用 sleep_for 硬等——CI 上硬等容易 flaky。
	// 命中立刻返回 true，超时返回最后一次判定结果。
	template <typename Predicate>
	bool waitUntil(Predicate pred, std::chrono::milliseconds timeout)
	{
		const auto deadline = std::chrono::steady_clock::now() + timeout;
		while (std::chrono::steady_clock::now() < deadline)
		{
			if (pred())
			{
				return true;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
		return pred();
	}

	// 跑 server.start() 的线程 + 一个能带超时等的「start() 是否已返回」信号。
	// std::thread::join 没有超时，用它来抓「run() 被定时器拖住」这种卡死。
	struct ServerRun
	{
		std::thread thread;
		std::mutex mutex;
		std::condition_variable cv;
		bool returned {false};
	};

	void launchServer(std::shared_ptr<HttpServer> server, std::shared_ptr<ServerRun> run)
	{
		run->thread = std::thread(
			[server, run]()
			{
				server->start();
				{
					std::lock_guard<std::mutex> lock(run->mutex);
					run->returned = true;
				}
				run->cv.notify_all();
			});
	}

	// 等 start() 返回，最多 timeout；true 表示已返回
	bool waitStartReturned(ServerRun& run, std::chrono::milliseconds timeout)
	{
		std::unique_lock<std::mutex> lock(run.mutex);
		return run.cv.wait_for(lock,
							   timeout,
							   [&run]()
							   {
								   return run.returned;
							   });
	}

	// 失败兜底容器：若 run() 真被卡死，detached 线程还在用这些对象，
	// 挪进静态表保活，进程退出时自然回收——总好过让测试进程挂死。
	std::vector<std::shared_ptr<void>>& leakedOnFailure()
	{
		static std::vector<std::shared_ptr<void>> leaked;
		return leaked;
	}

	// 启动服务器并等到能连上，返回实际端口
	uint16_t startServerAndWait(HttpServer& server, std::thread& serverThread)
	{
		serverThread = std::thread(
			[&server]()
			{
				server.start();
			});

		uint16_t port = 0;
		for (int i = 0; i < 100; ++i)
		{
			port = server.port();
			if (port != 0)
			{
				break;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
		}

		for (int i = 0; i < 100; ++i)
		{
			try
			{
				boost::asio::io_context io;
				tcp::socket sock(io);
				sock.connect(tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), port));
				sock.close();
				return port;
			}
			catch (...)
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(20));
			}
		}
		return port;
	}
} // namespace

// 注册多个任务不该抛异常
TEST(PeriodicTaskTest, RegisterMultipleBeforeStart_NoThrow)
{
	HttpServer server(0);

	EXPECT_NO_THROW(server.addPeriodicTask("tick-a", std::chrono::milliseconds(100), noopCallback));
	EXPECT_NO_THROW(server.addPeriodicTask("tick-b", std::chrono::milliseconds(250), noopCallback));
	EXPECT_NO_THROW(server.addPeriodicTask("tick-c", std::chrono::seconds(1), noopCallback));
}

// start() 之后再注册要抛 std::logic_error
TEST(PeriodicTaskTest, RegisterAfterStart_ThrowsLogicError)
{
	HttpServer server(0);

	std::thread serverThread;
	startServerAndWait(server, serverThread);

	EXPECT_THROW(server.addPeriodicTask("late", std::chrono::milliseconds(100), noopCallback), std::logic_error);

	server.stop();
	if (serverThread.joinable())
	{
		serverThread.join();
	}
}

// interval 非正一律拒绝
TEST(PeriodicTaskTest, NonPositiveInterval_ThrowsInvalidArgument)
{
	HttpServer server(0);

	EXPECT_THROW(server.addPeriodicTask("zero", std::chrono::milliseconds(0), noopCallback), std::invalid_argument);
	EXPECT_THROW(server.addPeriodicTask("neg", std::chrono::milliseconds(-5), noopCallback), std::invalid_argument);
}

// 空 callback 一律拒绝
TEST(PeriodicTaskTest, EmptyCallback_ThrowsInvalidArgument)
{
	HttpServer server(0);

	std::function<Awaitable<void>()> emptyCallback;

	EXPECT_THROW(server.addPeriodicTask("empty", std::chrono::milliseconds(100), emptyCallback), std::invalid_argument);
}

// 注册了任务之后启停仍然干净，不崩不挂
TEST(PeriodicTaskTest, RegisteredTasks_ServerStartsAndStopsCleanly)
{
	HttpServer server(0);
	server.setShutdownTimeout(5.0);

	server.addPeriodicTask("tick-a", std::chrono::milliseconds(100), noopCallback);
	server.addPeriodicTask("tick-b", std::chrono::milliseconds(200), noopCallback);

	std::thread serverThread;
	startServerAndWait(server, serverThread);

	server.stop();
	if (serverThread.joinable())
	{
		serverThread.join();
	}
}

// 反复注册 / 重建 HttpServer 不泄漏、不崩
TEST(PeriodicTaskTest, RepeatedRebuild_NoResourceIssue)
{
	for (int round = 0; round < 3; ++round)
	{
		HttpServer server(0);

		for (int i = 0; i < 8; ++i)
		{
			server.addPeriodicTask("task-" + std::to_string(i), std::chrono::milliseconds(100 + i), noopCallback);
		}

		std::thread serverThread;
		startServerAndWait(server, serverThread);

		server.stop();
		if (serverThread.joinable())
		{
			serverThread.join();
		}
	}
}

// 核心验收：关服不卡。interval 远大于 join 超时阈值，如果 stop() 没把定时器
// cancel 掉，run() 就得等定时器自然到期，join 必然超时，测试这才抓得住。
TEST(PeriodicTaskTest, StopWithLongIntervalTask_DoesNotBlockShutdown)
{
	const auto interval = std::chrono::seconds(10);
	const auto joinTimeout = std::chrono::milliseconds(5000);

	auto server = std::make_shared<HttpServer>(0);
	server->setShutdownTimeout(5.0);

	// shared_ptr 捕获：万一走失败兜底 detach 了线程，ticks 得能活到协程停为止
	auto ticks = std::make_shared<std::atomic<int>>(0);
	server->addPeriodicTask("slow-tick",
							interval,
							[ticks]() -> Awaitable<void>
							{
								ticks->fetch_add(1, std::memory_order_relaxed);
								co_return;
							});

	auto run = std::make_shared<ServerRun>();
	launchServer(server, run);

	// 第一次触发在 interval 之后，所以这里得等到 t≈10s
	const bool fired = waitUntil(
		[&ticks]()
		{
			return ticks->load() > 0;
		},
		std::chrono::seconds(20));
	EXPECT_TRUE(fired) << "周期任务在 20s 内一次都没触发";

	const auto stopAt = std::chrono::steady_clock::now();
	server->stop();
	const bool returned = waitStartReturned(*run, joinTimeout);
	const auto elapsed = std::chrono::steady_clock::now() - stopAt;

	if (returned)
	{
		run->thread.join();
	}
	else
	{
		// 关服被定时器拖住，这正是本任务要修的失败点。此时不能 join（会挂死进程），
		// 把还活着的对象挪进静态表保活，再 detach 线程。
		run->thread.detach();
		leakedOnFailure().push_back(server);
		leakedOnFailure().push_back(run);
		leakedOnFailure().push_back(ticks);
	}

	std::fprintf(stdout,
				 "[periodic-stop] interval=%lldms, stop->start-return=%lldms\n",
				 static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(interval).count()),
				 static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()));

	EXPECT_TRUE(returned) << "stop() 后 start() 没在 5s 内返回，run() 被未取消的周期定时器拖住了";
	EXPECT_LT(elapsed, std::chrono::seconds(2));
}

// 任务按 interval 反复触发
TEST(PeriodicTaskTest, FiresRepeatedly_AtInterval)
{
	auto server = std::make_shared<HttpServer>(0);
	auto ticks = std::make_shared<std::atomic<int>>(0);
	server->addPeriodicTask("ticker",
							std::chrono::milliseconds(20),
							[ticks]() -> Awaitable<void>
							{
								ticks->fetch_add(1, std::memory_order_relaxed);
								co_return;
							});

	auto run = std::make_shared<ServerRun>();
	launchServer(server, run);

	const bool firedThreeTimes = waitUntil(
		[&ticks]()
		{
			return ticks->load() >= 3;
		},
		std::chrono::seconds(10));
	EXPECT_TRUE(firedThreeTimes) << "20ms 的任务在 10s 内触发不到 3 次，实际 " << ticks->load();

	server->stop();
	if (waitStartReturned(*run, std::chrono::milliseconds(5000)))
	{
		run->thread.join();
	}
	else
	{
		run->thread.detach();
		leakedOnFailure().push_back(server);
		leakedOnFailure().push_back(run);
		leakedOnFailure().push_back(ticks);
		ADD_FAILURE() << "stop() 后 start() 没在 5s 内返回";
	}
}

// callback 抛异常不该带走整个任务，下一轮照常触发
TEST(PeriodicTaskTest, CallbackThrows_TaskKeepsRunning)
{
	auto server = std::make_shared<HttpServer>(0);
	auto ticks = std::make_shared<std::atomic<int>>(0);
	server->addPeriodicTask("throwing",
							std::chrono::milliseconds(20),
							[ticks]() -> Awaitable<void>
							{
								ticks->fetch_add(1, std::memory_order_relaxed);
								throw std::runtime_error("boom");
							});

	auto run = std::make_shared<ServerRun>();
	launchServer(server, run);

	// 每轮都抛，还能触发 >= 3 次就说明异常被接住了、任务没死
	const bool survived = waitUntil(
		[&ticks]()
		{
			return ticks->load() >= 3;
		},
		std::chrono::seconds(10));
	EXPECT_TRUE(survived) << "callback 每次抛异常后任务被终止了，实际只触发 " << ticks->load() << " 次";

	server->stop();
	if (waitStartReturned(*run, std::chrono::milliseconds(5000)))
	{
		run->thread.join();
	}
	else
	{
		run->thread.detach();
		leakedOnFailure().push_back(server);
		leakedOnFailure().push_back(run);
		leakedOnFailure().push_back(ticks);
		ADD_FAILURE() << "stop() 后 start() 没在 5s 内返回";
	}
}

// 多个任务各自按自己的节奏跑，停服时一起干净退出
TEST(PeriodicTaskTest, MultipleTasks_RunIndependently)
{
	auto server = std::make_shared<HttpServer>(0);
	auto fastTicks = std::make_shared<std::atomic<int>>(0);
	auto slowTicks = std::make_shared<std::atomic<int>>(0);

	server->addPeriodicTask("fast",
							std::chrono::milliseconds(15),
							[fastTicks]() -> Awaitable<void>
							{
								fastTicks->fetch_add(1, std::memory_order_relaxed);
								co_return;
							});
	server->addPeriodicTask("slow",
							std::chrono::milliseconds(60),
							[slowTicks]() -> Awaitable<void>
							{
								slowTicks->fetch_add(1, std::memory_order_relaxed);
								co_return;
							});

	auto run = std::make_shared<ServerRun>();
	launchServer(server, run);

	const bool both = waitUntil(
		[&fastTicks, &slowTicks]()
		{
			return fastTicks->load() >= 2 && slowTicks->load() >= 2;
		},
		std::chrono::seconds(10));
	EXPECT_TRUE(both) << "两个任务没各自独立触发，fast=" << fastTicks->load() << " slow=" << slowTicks->load();

	server->stop();
	if (waitStartReturned(*run, std::chrono::milliseconds(5000)))
	{
		run->thread.join();
	}
	else
	{
		run->thread.detach();
		leakedOnFailure().push_back(server);
		leakedOnFailure().push_back(run);
		leakedOnFailure().push_back(fastTicks);
		leakedOnFailure().push_back(slowTicks);
		ADD_FAILURE() << "stop() 后 start() 没在 5s 内返回";
	}
}

// stop() 之后不再触发
TEST(PeriodicTaskTest, AfterStop_TaskNoLongerFires)
{
	auto server = std::make_shared<HttpServer>(0);
	auto ticks = std::make_shared<std::atomic<int>>(0);
	server->addPeriodicTask("ticker",
							std::chrono::milliseconds(20),
							[ticks]() -> Awaitable<void>
							{
								ticks->fetch_add(1, std::memory_order_relaxed);
								co_return;
							});

	auto run = std::make_shared<ServerRun>();
	launchServer(server, run);

	ASSERT_TRUE(waitUntil(
		[&ticks]()
		{
			return ticks->load() > 0;
		},
		std::chrono::seconds(10)));

	server->stop();
	bool returned = waitStartReturned(*run, std::chrono::milliseconds(5000));
	if (returned)
	{
		run->thread.join();
	}
	else
	{
		run->thread.detach();
		leakedOnFailure().push_back(server);
		leakedOnFailure().push_back(run);
		leakedOnFailure().push_back(ticks);
		ADD_FAILURE() << "stop() 后 start() 没在 5s 内返回";
		return;
	}

	const int frozen = ticks->load();
	// 留出足够时间让（如果没停干净的话）任务再触发几次：20ms 间隔，200ms 够跑十轮
	std::this_thread::sleep_for(std::chrono::milliseconds(200));
	EXPECT_EQ(ticks->load(), frozen) << "stop() 之后任务还在触发";
}

// 高频任务（1ms）：能被高频调度，但 stop() 仍要迅速退出——验证「高频 ≠ 关不掉」。
// 触发次数只取下界断言，不锁精确值，避开调度抖动带来的 flaky。
TEST(PeriodicTaskTest, HighFrequencyTask_StopReturnsPromptly)
{
	auto server = std::make_shared<HttpServer>(0);
	server->setShutdownTimeout(5.0);
	auto ticks = std::make_shared<std::atomic<int>>(0);
	constexpr int kMinFires = 10;
	server->addPeriodicTask("hot-tick",
							std::chrono::milliseconds(1),
							[ticks]() -> Awaitable<void>
							{
								ticks->fetch_add(1, std::memory_order_relaxed);
								co_return;
							});

	auto run = std::make_shared<ServerRun>();
	launchServer(server, run);

	const bool fired = waitUntil(
		[&ticks]()
		{
			return ticks->load() >= kMinFires;
		},
		std::chrono::seconds(10));
	EXPECT_TRUE(fired) << "1ms 任务在 10s 内触发不到 " << kMinFires << " 次，实际 " << ticks->load();

	const auto stopAt = std::chrono::steady_clock::now();
	server->stop();
	const bool returned = waitStartReturned(*run, std::chrono::milliseconds(5000));
	const auto elapsed =
		std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - stopAt);

	if (returned)
	{
		run->thread.join();
	}
	else
	{
		run->thread.detach();
		leakedOnFailure().push_back(server);
		leakedOnFailure().push_back(run);
		leakedOnFailure().push_back(ticks);
	}

	std::fprintf(stdout,
				 "[periodic-hot] fires=%d, stop->start-return=%lldms\n",
				 ticks->load(),
				 static_cast<long long>(elapsed.count()));

	// 高频任务下关服耗时仍要远小于「卡住一个 interval」的量级
	EXPECT_TRUE(returned) << "高频任务下 stop() 后 start() 没在 5s 内返回";
	EXPECT_LT(elapsed, std::chrono::seconds(2));
}

// 三个不同间隔的任务各自按自己的节奏跑，互不干扰。
// 断言用下界 + 相对关系（快任务触发次数必然多于慢任务），不锁精确值。
TEST(PeriodicTaskTest, ThreeTasksDifferentIntervals_PaceIndependently)
{
	auto server = std::make_shared<HttpServer>(0);
	auto fastTicks = std::make_shared<std::atomic<int>>(0);
	auto midTicks = std::make_shared<std::atomic<int>>(0);
	auto slowTicks = std::make_shared<std::atomic<int>>(0);

	server->addPeriodicTask("fast-10ms",
							std::chrono::milliseconds(10),
							[fastTicks]() -> Awaitable<void>
							{
								fastTicks->fetch_add(1, std::memory_order_relaxed);
								co_return;
							});
	server->addPeriodicTask("mid-50ms",
							std::chrono::milliseconds(50),
							[midTicks]() -> Awaitable<void>
							{
								midTicks->fetch_add(1, std::memory_order_relaxed);
								co_return;
							});
	server->addPeriodicTask("slow-200ms",
							std::chrono::milliseconds(200),
							[slowTicks]() -> Awaitable<void>
							{
								slowTicks->fetch_add(1, std::memory_order_relaxed);
								co_return;
							});

	auto run = std::make_shared<ServerRun>();
	launchServer(server, run);

	// 等到慢任务至少触发一次——那一刻快任务必然已经跑了好几轮
	const bool allFired = waitUntil(
		[&fastTicks, &midTicks, &slowTicks]()
		{
			return fastTicks->load() >= 2 && midTicks->load() >= 1 && slowTicks->load() >= 1;
		},
		std::chrono::seconds(10));
	EXPECT_TRUE(allFired) << "三个任务没各自独立触发，fast=" << fastTicks->load() << " mid=" << midTicks->load()
						  << " slow=" << slowTicks->load();

	// 10ms 的到这一刻触发的次数应当多于 200ms 的，节奏确实分开了
	EXPECT_GT(fastTicks->load(), slowTicks->load()) << "快慢任务的节奏没有分开";

	server->stop();
	if (waitStartReturned(*run, std::chrono::milliseconds(5000)))
	{
		run->thread.join();
	}
	else
	{
		run->thread.detach();
		leakedOnFailure().push_back(server);
		leakedOnFailure().push_back(run);
		leakedOnFailure().push_back(fastTicks);
		leakedOnFailure().push_back(midTicks);
		leakedOnFailure().push_back(slowTicks);
		ADD_FAILURE() << "stop() 后 start() 没在 5s 内返回";
	}
}

// stop() 幂等：连着调两次不崩、不重复释放。
TEST(PeriodicTaskTest, StopTwice_IsIdempotent)
{
	auto server = std::make_shared<HttpServer>(0);
	auto ticks = std::make_shared<std::atomic<int>>(0);
	server->addPeriodicTask("ticker",
							std::chrono::milliseconds(20),
							[ticks]() -> Awaitable<void>
							{
								ticks->fetch_add(1, std::memory_order_relaxed);
								co_return;
							});

	auto run = std::make_shared<ServerRun>();
	launchServer(server, run);

	ASSERT_TRUE(waitUntil(
		[&ticks]()
		{
			return ticks->load() > 0;
		},
		std::chrono::seconds(10)));

	server->stop();
	EXPECT_FALSE(server->isRunning());
	// 第二次 stop() 应当直接早退：running_ 已经是 false
	EXPECT_NO_THROW(server->stop());
	EXPECT_FALSE(server->isRunning());

	bool returned = waitStartReturned(*run, std::chrono::milliseconds(5000));
	if (returned)
	{
		run->thread.join();
	}
	else
	{
		run->thread.detach();
		leakedOnFailure().push_back(server);
		leakedOnFailure().push_back(run);
		leakedOnFailure().push_back(ticks);
		ADD_FAILURE() << "stop() 后 start() 没在 5s 内返回";
	}
}

// 注册了任务但从不 start()：直接析构不崩、不挂。
// 顺带确认对未启动的服务器调 stop() 是安全 no-op。
TEST(PeriodicTaskTest, RegisteredButNeverStarted_DestructsCleanly)
{
	{
		HttpServer server(0);
		server.addPeriodicTask("never-a", std::chrono::milliseconds(50), noopCallback);
		server.addPeriodicTask("never-b", std::chrono::milliseconds(100), noopCallback);

		// start() 没调过，stop() 应当直接早退
		EXPECT_NO_THROW(server.stop());
		EXPECT_FALSE(server.isRunning());
	} // 析构在这——能走到下一行就说明没崩没挂

	SUCCEED();
}

// stop() 恰逢 callback 正在执行：stop() 自身不能被回调拖住，回调收尾后协程要能正常退出。
TEST(PeriodicTaskTest, StopWhileCallbackRunning_DoesNotBlock)
{
	auto server = std::make_shared<HttpServer>(0);
	server->setShutdownTimeout(5.0);

	const auto callbackWork = std::chrono::milliseconds(400);
	const auto stopBudget = std::chrono::milliseconds(200);
	auto entered = std::make_shared<std::atomic<bool>>(false);

	server->addPeriodicTask("busy",
							std::chrono::milliseconds(20),
							[entered, callbackWork]() -> Awaitable<void>
							{
								entered->store(true, std::memory_order_relaxed);
								// 占住 loop 线程，模拟一个耗时回调
								std::this_thread::sleep_for(callbackWork);
								co_return;
							});

	auto run = std::make_shared<ServerRun>();
	launchServer(server, run);

	ASSERT_TRUE(waitUntil(
		[&entered]()
		{
			return entered->load();
		},
		std::chrono::seconds(10)))
		<< "回调一直没进入";

	const auto stopAt = std::chrono::steady_clock::now();
	server->stop();
	const auto stopElapsed =
		std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - stopAt);

	// stop() 必须远快于回调本身，不能在里面同步等回调跑完
	EXPECT_LT(stopElapsed, stopBudget) << "stop() 在回调执行期间发生了阻塞等待，耗时 " << stopElapsed.count() << "ms";

	bool returned = waitStartReturned(*run, std::chrono::milliseconds(5000));
	if (returned)
	{
		run->thread.join();
	}
	else
	{
		run->thread.detach();
		leakedOnFailure().push_back(server);
		leakedOnFailure().push_back(run);
		leakedOnFailure().push_back(entered);
	}

	EXPECT_TRUE(returned) << "回调结束后 start() 没在 5s 内返回";
}

// 多 IO 线程配置下周期任务照常启停。任务跑在 baseLoop_ 上，
// 这条主要确认 ioThreads>1 时 stop 时序没被搞坏。
TEST(PeriodicTaskTest, MultiIoThreads_PeriodicTaskTicksAndStops)
{
	auto server = std::make_shared<HttpServer>(0, 4);
	auto ticks = std::make_shared<std::atomic<int>>(0);
	server->addPeriodicTask("ticker",
							std::chrono::milliseconds(20),
							[ticks]() -> Awaitable<void>
							{
								ticks->fetch_add(1, std::memory_order_relaxed);
								co_return;
							});

	auto run = std::make_shared<ServerRun>();
	launchServer(server, run);

	const bool fired = waitUntil(
		[&ticks]()
		{
			return ticks->load() >= 3;
		},
		std::chrono::seconds(10));
	EXPECT_TRUE(fired) << "多 IO 线程下周期任务在 10s 内触发不到 3 次，实际 " << ticks->load();

	server->stop();
	bool returned = waitStartReturned(*run, std::chrono::milliseconds(5000));
	if (returned)
	{
		run->thread.join();
	}
	else
	{
		run->thread.detach();
		leakedOnFailure().push_back(server);
		leakedOnFailure().push_back(run);
		leakedOnFailure().push_back(ticks);
	}

	EXPECT_TRUE(returned) << "多 IO 线程下 stop() 后 start() 没在 5s 内返回";
}
