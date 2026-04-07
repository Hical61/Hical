#include "asio/GenericConnection.h"
#include "asio/AsioEventLoop.h"
#include "core/PmrBuffer.h"
#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <thread>

using namespace hical;

// 测试辅助：创建一对已连接的 socket
std::pair<boost::asio::ip::tcp::socket, boost::asio::ip::tcp::socket> createConnectedSockets(
	boost::asio::io_context& io1,
	boost::asio::io_context& io2)
{
	using boost::asio::ip::tcp;

	// 创建 acceptor，绑定到 localhost
	tcp::acceptor acceptor(io1, tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), 0));
	auto port = acceptor.local_endpoint().port();

	// 客户端连接
	tcp::socket clientSocket(io2);
	tcp::socket serverSocket(io1);

	std::atomic<bool> connected {false};

	// 异步接受连接
	acceptor.async_accept(serverSocket,
						  [&](boost::system::error_code ec)
						  {
							  EXPECT_FALSE(ec);
							  connected = true;
						  });

	// 客户端连接到 localhost
	clientSocket.connect(tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), port));

	// 等待连接建立
	while (!connected)
	{
		io1.poll();
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}

	return {std::move(serverSocket), std::move(clientSocket)};
}

// 测试基本的连接建立
TEST(PlainConnectionTest, BasicConnection)
{
	AsioEventLoop loop;
	auto [socket1, socket2] = createConnectedSockets(loop.getIoContext(), loop.getIoContext());

	InetAddress localAddr("127.0.0.1", 12345);
	InetAddress peerAddr("127.0.0.1", 54321);

	auto conn = std::make_shared<PlainConnection>(&loop, std::move(socket1), localAddr, peerAddr);

	EXPECT_FALSE(conn->connected());

	std::atomic<bool> established {false};
	conn->onConnection(
		[&](const TcpConnection::Ptr& c)
		{
			if (c->connected())
			{
				established = true;
			}
		});

	conn->connectEstablished();

	std::thread loopThread(
		[&loop]()
		{
			loop.run();
		});

	std::this_thread::sleep_for(std::chrono::milliseconds(100));

	EXPECT_TRUE(established.load());
	EXPECT_TRUE(conn->connected());

	conn->close();
	std::this_thread::sleep_for(std::chrono::milliseconds(50));

	loop.stop();
	loopThread.join();
}

// 测试数据发送和接收
TEST(PlainConnectionTest, SendAndReceive)
{
	AsioEventLoop loop;
	auto [socket1, socket2] = createConnectedSockets(loop.getIoContext(), loop.getIoContext());

	InetAddress localAddr("127.0.0.1", 12345);
	InetAddress peerAddr("127.0.0.1", 54321);

	auto conn = std::make_shared<PlainConnection>(&loop, std::move(socket1), localAddr, peerAddr);

	std::atomic<bool> received {false};
	std::string receivedData;

	conn->onMessage(
		[&](const TcpConnection::Ptr& /*c*/, PmrBuffer* buf)
		{
			receivedData = buf->read(buf->readableBytes());
			received = true;
		});

	conn->connectEstablished();

	std::thread loopThread(
		[&loop]()
		{
			loop.run();
		});

	std::this_thread::sleep_for(std::chrono::milliseconds(50));

	// 从 socket2 发送数据
	std::string testData = "Hello PlainConnection!";
	boost::asio::write(socket2, boost::asio::buffer(testData));

	// 等待接收
	std::this_thread::sleep_for(std::chrono::milliseconds(100));

	EXPECT_TRUE(received.load());
	EXPECT_EQ(receivedData, testData);
	EXPECT_EQ(conn->bytesReceived(), testData.size());

	conn->close();
	loop.stop();
	loopThread.join();
}

// 测试从 PlainConnection 发送数据
TEST(PlainConnectionTest, SendFromConnection)
{
	AsioEventLoop loop;
	auto [socket1, socket2] = createConnectedSockets(loop.getIoContext(), loop.getIoContext());

	InetAddress localAddr("127.0.0.1", 12345);
	InetAddress peerAddr("127.0.0.1", 54321);

	auto conn = std::make_shared<PlainConnection>(&loop, std::move(socket1), localAddr, peerAddr);

	conn->connectEstablished();

	std::thread loopThread(
		[&loop]()
		{
			loop.run();
		});

	std::this_thread::sleep_for(std::chrono::milliseconds(50));

	// 从连接发送数据
	std::string testData = "Hello from PlainConnection!";
	conn->send(testData);

	// 等待异步写操作完成
	std::this_thread::sleep_for(std::chrono::milliseconds(100));

	// 从 socket2 接收数据
	char buffer[1024];
	boost::system::error_code ec;
	size_t len = socket2.read_some(boost::asio::buffer(buffer), ec);

	EXPECT_FALSE(ec);
	EXPECT_EQ(std::string(buffer, len), testData);

	// 等待 bytesSent 统计更新
	std::this_thread::sleep_for(std::chrono::milliseconds(50));
	EXPECT_EQ(conn->bytesSent(), testData.size());

	conn->close();
	loop.stop();
	loopThread.join();
}

// 测试写完成回调
TEST(PlainConnectionTest, WriteCompleteCallback)
{
	AsioEventLoop loop;
	auto [socket1, socket2] = createConnectedSockets(loop.getIoContext(), loop.getIoContext());

	InetAddress localAddr("127.0.0.1", 12345);
	InetAddress peerAddr("127.0.0.1", 54321);

	auto conn = std::make_shared<PlainConnection>(&loop, std::move(socket1), localAddr, peerAddr);

	std::atomic<int> writeCompleteCount {0};
	conn->onWriteComplete(
		[&](const TcpConnection::Ptr&)
		{
			writeCompleteCount++;
		});

	conn->connectEstablished();

	std::thread loopThread(
		[&loop]()
		{
			loop.run();
		});

	std::this_thread::sleep_for(std::chrono::milliseconds(50));

	// 发送数据
	conn->send("Test message");

	// 等待写完成
	std::this_thread::sleep_for(std::chrono::milliseconds(100));

	EXPECT_GE(writeCompleteCount.load(), 1);

	conn->close();
	loop.stop();
	loopThread.join();
}

// 测试连接关闭回调
TEST(PlainConnectionTest, CloseCallback)
{
	AsioEventLoop loop;
	auto [socket1, socket2] = createConnectedSockets(loop.getIoContext(), loop.getIoContext());

	InetAddress localAddr("127.0.0.1", 12345);
	InetAddress peerAddr("127.0.0.1", 54321);

	auto conn = std::make_shared<PlainConnection>(&loop, std::move(socket1), localAddr, peerAddr);

	std::atomic<bool> closed {false};
	conn->onClose(
		[&](const TcpConnection::Ptr&)
		{
			closed = true;
		});

	conn->connectEstablished();

	std::thread loopThread(
		[&loop]()
		{
			loop.run();
		});

	std::this_thread::sleep_for(std::chrono::milliseconds(50));

	// 关闭连接
	conn->close();

	std::this_thread::sleep_for(std::chrono::milliseconds(100));

	EXPECT_TRUE(closed.load());
	EXPECT_TRUE(conn->disconnected());

	loop.stop();
	loopThread.join();
}

// 测试 TCP_NODELAY
TEST(PlainConnectionTest, TcpNoDelay)
{
	AsioEventLoop loop;
	auto [socket1, socket2] = createConnectedSockets(loop.getIoContext(), loop.getIoContext());

	InetAddress localAddr("127.0.0.1", 12345);
	InetAddress peerAddr("127.0.0.1", 54321);

	auto conn = std::make_shared<PlainConnection>(&loop, std::move(socket1), localAddr, peerAddr);

	conn->connectEstablished();

	// 设置 TCP_NODELAY
	conn->setTcpNoDelay(true);

	// 无异常即为成功
	EXPECT_TRUE(true);

	conn->close();
}

// 测试用户上下文
TEST(PlainConnectionTest, UserContext)
{
	AsioEventLoop loop;
	auto [socket1, socket2] = createConnectedSockets(loop.getIoContext(), loop.getIoContext());

	InetAddress localAddr("127.0.0.1", 12345);
	InetAddress peerAddr("127.0.0.1", 54321);

	auto conn = std::make_shared<PlainConnection>(&loop, std::move(socket1), localAddr, peerAddr);

	EXPECT_FALSE(conn->hasContext());

	auto context = std::make_shared<int>(42);
	conn->setContext(context);

	EXPECT_TRUE(conn->hasContext());
	EXPECT_EQ(*conn->getContext<int>(), 42);

	conn->clearContext();
	EXPECT_FALSE(conn->hasContext());

	conn->close();
}

// 测试 PmrBuffer 使用
TEST(PlainConnectionTest, PmrBufferUsage)
{
	AsioEventLoop loop;
	auto [socket1, socket2] = createConnectedSockets(loop.getIoContext(), loop.getIoContext());

	InetAddress localAddr("127.0.0.1", 12345);
	InetAddress peerAddr("127.0.0.1", 54321);

	auto conn = std::make_shared<PlainConnection>(&loop, std::move(socket1), localAddr, peerAddr);

	std::atomic<bool> received {false};
	PmrBuffer receivedBuffer(loop.allocator());

	conn->onMessage(
		[&](const TcpConnection::Ptr& /*c*/, PmrBuffer* buf)
		{
			receivedBuffer.append(*buf);
			buf->retrieveAll();
			received = true;
		});

	conn->connectEstablished();

	std::thread loopThread(
		[&loop]()
		{
			loop.run();
		});

	std::this_thread::sleep_for(std::chrono::milliseconds(50));

	// 发送数据
	std::string testData = "PmrBuffer test data";
	boost::asio::write(socket2, boost::asio::buffer(testData));

	// 等待接收
	std::this_thread::sleep_for(std::chrono::milliseconds(100));

	EXPECT_TRUE(received.load());
	EXPECT_EQ(receivedBuffer.readableBytes(), testData.size());
	EXPECT_EQ(std::string(receivedBuffer.peek(), receivedBuffer.readableBytes()), testData);

	conn->close();
	loop.stop();
	loopThread.join();
}
