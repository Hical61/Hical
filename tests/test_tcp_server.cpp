#include "asio/TcpServer.h"
#include "asio/AsioEventLoop.h"
#include "asio/EventLoopPool.h"
#include "core/PmrBuffer.h"
#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <thread>

using namespace hical;
using boost::asio::ip::tcp;

// ============ EventLoopPool 测试 ============

TEST(EventLoopPoolTest, CreateAndStart)
{
	EventLoopPool pool(2);
	EXPECT_EQ(pool.size(), 2);
	EXPECT_FALSE(pool.isRunning());

	pool.start();
	EXPECT_TRUE(pool.isRunning());

	std::this_thread::sleep_for(std::chrono::milliseconds(50));

	pool.stop();
	EXPECT_FALSE(pool.isRunning());
}

TEST(EventLoopPoolTest, RoundRobin)
{
	EventLoopPool pool(3);
	pool.start();

	auto* loop1 = pool.getNextLoop();
	auto* loop2 = pool.getNextLoop();
	auto* loop3 = pool.getNextLoop();
	auto* loop4 = pool.getNextLoop();

	EXPECT_NE(loop1, nullptr);
	EXPECT_NE(loop2, nullptr);
	EXPECT_NE(loop3, nullptr);
	// loop4 应该循环回到 loop1
	EXPECT_EQ(loop4, loop1);

	pool.stop();
}

TEST(EventLoopPoolTest, GetAllLoops)
{
	EventLoopPool pool(4);
	auto loops = pool.getAllLoops();
	EXPECT_EQ(loops.size(), 4);

	for (auto* loop : loops)
	{
		EXPECT_NE(loop, nullptr);
	}
}

TEST(EventLoopPoolTest, PostTaskToPoolLoop)
{
	EventLoopPool pool(2);
	pool.start();

	std::this_thread::sleep_for(std::chrono::milliseconds(50));

	std::atomic<int> counter {0};
	auto* loop = pool.getNextLoop();
	loop->post(
		[&counter]()
		{
			counter++;
		});

	std::this_thread::sleep_for(std::chrono::milliseconds(100));
	EXPECT_EQ(counter.load(), 1);

	pool.stop();
}

// ============ TcpServer 测试 ============

TEST(TcpServerTest, StartAndStop)
{
	AsioEventLoop loop;
	InetAddress addr("127.0.0.1", 0);

	TcpServer server(&loop, addr, "test-server");
	EXPECT_EQ(server.name(), "test-server");
	EXPECT_FALSE(server.isRunning());

	std::thread loopThread(
		[&loop]()
		{
			loop.run();
		});

	std::this_thread::sleep_for(std::chrono::milliseconds(50));

	server.start();
	EXPECT_TRUE(server.isRunning());

	std::this_thread::sleep_for(std::chrono::milliseconds(50));

	server.stop();
	EXPECT_FALSE(server.isRunning());

	loop.stop();
	loopThread.join();
}

TEST(TcpServerTest, AcceptConnection)
{
	AsioEventLoop loop;
	InetAddress addr("127.0.0.1", 0);

	TcpServer server(&loop, addr, "test-server");

	std::atomic<bool> connected {false};
	server.onNewConnection(
		[&connected](const TcpConnection::Ptr&)
		{
			connected = true;
		});

	std::thread loopThread(
		[&loop]()
		{
			loop.run();
		});

	std::this_thread::sleep_for(std::chrono::milliseconds(50));

	server.start();

	std::this_thread::sleep_for(std::chrono::milliseconds(50));

	// 获取实际监听端口
	auto localEp = server.listenAddr();

	// 客户端连接
	tcp::socket client(loop.getIoContext());
	client.connect(tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), localEp.port()));

	std::this_thread::sleep_for(std::chrono::milliseconds(200));

	EXPECT_TRUE(connected.load());
	EXPECT_GE(server.connectionCount(), 1);

	client.close();
	server.stop();
	loop.stop();
	loopThread.join();
}

TEST(TcpServerTest, ReceiveMessage)
{
	AsioEventLoop loop;
	InetAddress addr("127.0.0.1", 0);

	TcpServer server(&loop, addr, "test-server");

	std::atomic<bool> received {false};
	std::string receivedData;
	server.onMessage(
		[&received, &receivedData](const TcpConnection::Ptr&, PmrBuffer* buf)
		{
			receivedData = buf->readAll();
			received = true;
		});

	std::thread loopThread(
		[&loop]()
		{
			loop.run();
		});

	std::this_thread::sleep_for(std::chrono::milliseconds(50));

	server.start();
	std::this_thread::sleep_for(std::chrono::milliseconds(50));

	auto localEp = server.listenAddr();

	tcp::socket client(loop.getIoContext());
	client.connect(tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), localEp.port()));

	std::this_thread::sleep_for(std::chrono::milliseconds(100));

	// 发送数据
	std::string msg = "Hello TcpServer!";
	boost::asio::write(client, boost::asio::buffer(msg));

	std::this_thread::sleep_for(std::chrono::milliseconds(200));

	EXPECT_TRUE(received.load());
	EXPECT_EQ(receivedData, msg);

	client.close();
	server.stop();
	loop.stop();
	loopThread.join();
}

TEST(TcpServerTest, WithIoPool)
{
	AsioEventLoop loop;
	InetAddress addr("127.0.0.1", 0);

	TcpServer server(&loop, addr, "test-pool-server");
	server.setIoLoopNum(2);

	std::atomic<int> connCount {0};
	server.onNewConnection(
		[&connCount](const TcpConnection::Ptr&)
		{
			connCount++;
		});

	std::thread loopThread(
		[&loop]()
		{
			loop.run();
		});

	std::this_thread::sleep_for(std::chrono::milliseconds(50));
	server.start();
	std::this_thread::sleep_for(std::chrono::milliseconds(50));

	auto localEp = server.listenAddr();

	// 3 个客户端连接
	std::vector<tcp::socket> clients;
	for (int i = 0; i < 3; ++i)
	{
		clients.emplace_back(loop.getIoContext());
		clients.back().connect(tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), localEp.port()));
	}

	std::this_thread::sleep_for(std::chrono::milliseconds(300));

	EXPECT_EQ(connCount.load(), 3);

	for (auto& c : clients)
	{
		c.close();
	}
	server.stop();
	loop.stop();
	loopThread.join();
}
