/**
 * @file TcpServer.cpp
 * @brief TCP 服务器 accept 循环实现
 */

#include "TcpServer.h"
#include "GenericConnection.h"
#include "SslConnection.h"
#include <algorithm>
#include <future>
#include <vector>

namespace hical
{

	TcpServer::TcpServer(AsioEventLoop* baseLoop, const InetAddress& listenAddr, const std::string& name)
		: baseLoop_(baseLoop)
		, listenAddr_(listenAddr)
		, name_(name)
		, acceptor_(baseLoop->getIoContext())
		, alive_(std::make_shared<std::atomic<bool>>(true))
	{
	}

	TcpServer::~TcpServer()
	{
		alive_->store(false);
		if (running_.load())
		{
			stop();
		}
	}

	void TcpServer::setIoLoopNum(size_t num)
	{
		ioLoopNum_ = num;
	}

	void TcpServer::start()
	{
		if (running_.exchange(true))
		{
			return;
		}

		// 创建并启动 IO 线程池
		if (ioLoopNum_ > 0)
		{
			ioPool_ = std::make_unique<EventLoopPool>(ioLoopNum_);
			ioPool_->start();
		}

		// 初始化 per-loop 连接分片
		// baseLoop 始终作为第一个 shard（即使有 ioPool，accept 也在 baseLoop）
		shards_.clear();
		shards_.push_back(LoopShard {baseLoop_, {}});
		if (ioPool_)
		{
			for (auto* loop : ioPool_->getAllLoops())
			{
				shards_.push_back(LoopShard {loop, {}});
			}
		}

		// 打开 acceptor
		using boost::asio::ip::tcp;
		auto endpoint = tcp::endpoint(listenAddr_.isIpV6() ? tcp::v6() : tcp::v4(), listenAddr_.port());

		acceptor_.open(endpoint.protocol());
		acceptor_.set_option(boost::asio::socket_base::reuse_address(true));
		acceptor_.bind(endpoint);
		acceptor_.listen();

		// 获取实际监听端口（端口 0 时由系统分配）
		auto actualEndpoint = acceptor_.local_endpoint();
		listenAddr_ = InetAddress(actualEndpoint.address().to_string(), actualEndpoint.port());

		// 启动 accept 循环，alive_ 防析构后协程还在跑
		auto aliveFlag = alive_;
		coSpawn(baseLoop_->getIoContext(),
				[this, aliveFlag]() -> Awaitable<void>
				{
					co_await acceptLoop();
				});

		// 在每个 shard 上启动独立的 idle check 协程
		if (idleTimeout_ > 0)
		{
			for (auto& shard : shards_)
			{
				coSpawn(shard.loop->getIoContext(),
						[this, &shard, aliveFlag]() -> Awaitable<void>
						{
							co_await idleCheckLoop(&shard);
						});
			}
		}
	}

	void TcpServer::stop()
	{
		if (!running_.exchange(false))
		{
			return;
		}

		// acceptor 只能在它所在的 io_context 线程上关闭
		auto closeAcceptor = [this]()
		{
			boost::system::error_code ec;
			acceptor_.close(ec);
		};

		if (baseLoop_->isInLoopThread())
		{
			closeAcceptor();
		}
		else
		{
			std::promise<void> done;
			auto future = done.get_future();
			boost::asio::post(baseLoop_->getIoContext(),
							  [&closeAcceptor, &done]()
							  {
								  closeAcceptor();
								  done.set_value();
							  });
			future.get();
		}

		// 向每个 shard 所在的 loop 线程 post 关闭任务
		// 注意：必须在 ioPool_->stop() 之前完成，否则 post 的任务无法执行
		for (auto& shard : shards_)
		{
			if (shard.loop->isInLoopThread())
			{
				for (auto& conn : shard.connections)
				{
					conn->close();
				}
				shard.connections.clear();
			}
			else
			{
				std::promise<void> shardDone;
				auto shardFuture = shardDone.get_future();
				boost::asio::post(shard.loop->getIoContext(),
								  [&shard, &shardDone]()
								  {
									  for (auto& conn : shard.connections)
									  {
										  conn->close();
									  }
									  shard.connections.clear();
									  shardDone.set_value();
								  });
				shardFuture.get();
			}
		}
		totalConnections_.store(0, std::memory_order_relaxed);

		// 停止 IO 线程池
		if (ioPool_)
		{
			ioPool_->stop();
		}

		shards_.clear();
	}

	void TcpServer::onNewConnection(NewConnectionCallback cb)
	{
		newConnectionCallback_ = std::move(cb);
	}

	void TcpServer::onMessage(TcpConnection::MessageCallback cb)
	{
		messageCallback_ = std::move(cb);
	}

	void TcpServer::onClose(TcpConnection::CloseCallback cb)
	{
		closeCallback_ = std::move(cb);
	}

	void TcpServer::enableSsl(std::shared_ptr<SslContext> ctx)
	{
		sslCtx_ = std::move(ctx);
	}

	const std::string& TcpServer::name() const
	{
		return name_;
	}

	const InetAddress& TcpServer::listenAddr() const
	{
		return listenAddr_;
	}

	size_t TcpServer::connectionCount() const
	{
		return totalConnections_.load(std::memory_order_relaxed);
	}

	bool TcpServer::isRunning() const
	{
		return running_.load();
	}

	void TcpServer::setIdleTimeout(double seconds)
	{
		idleTimeout_ = seconds;
	}

	AsioEventLoop* TcpServer::getNextIoLoop()
	{
		if (ioPool_ && ioPool_->size() > 0)
		{
			return ioPool_->getNextLoop();
		}
		return baseLoop_;
	}

	LoopShard& TcpServer::findShard(AsioEventLoop* loop)
	{
		for (auto& shard : shards_)
		{
			if (shard.loop == loop)
			{
				return shard;
			}
		}
		// 不应到达此处：所有 loop 都在 start() 时注册了 shard
		return shards_[0];
	}

	void TcpServer::addConnection(LoopShard& shard, const TcpConnection::Ptr& conn)
	{
		shard.connections.insert(conn);
		totalConnections_.fetch_add(1, std::memory_order_relaxed);
		shard.loop->incrementConnections();
	}

	void TcpServer::removeConnection(LoopShard& shard, const TcpConnection::Ptr& conn)
	{
		shard.connections.erase(conn);
		totalConnections_.fetch_sub(1, std::memory_order_relaxed);
		shard.loop->decrementConnections();
	}

	Awaitable<void> TcpServer::acceptLoop()
	{
		using boost::asio::ip::tcp;

		while (running_.load() && alive_->load())
		{
			bool needSleep = false;

			try
			{
				tcp::socket socket = co_await acceptor_.async_accept(boost::asio::use_awaitable);

				// accept 返回后再查一次还活着没
				if (!alive_->load())
				{
					break;
				}

				// 获取地址信息
				auto remoteEp = socket.remote_endpoint();
				auto localEp = socket.local_endpoint();

				InetAddress peerAddr(remoteEp.address().to_string(), remoteEp.port());
				InetAddress localAddr(localEp.address().to_string(), localEp.port());

				// 获取目标 IO 循环
				auto* ioLoop = getNextIoLoop();

				// 创建连接
				TcpConnection::Ptr conn;

				if (sslCtx_)
				{
					boost::asio::ssl::stream<tcp::socket> sslStream(std::move(socket), sslCtx_->native());
					conn = std::make_shared<SslConnection>(ioLoop, std::move(sslStream), localAddr, peerAddr);
				}
				else
				{
					conn = std::make_shared<PlainConnection>(ioLoop, std::move(socket), localAddr, peerAddr);
				}

				// 设置回调
				if (messageCallback_)
				{
					conn->onMessage(messageCallback_);
				}

				// 设置关闭回调（在原始回调之上添加连接移除逻辑）
				// 使用 alive_ 标志防止 TcpServer 析构后回调中的 use-after-free
				// closeCallback 在连接所在 loop 线程内触发（Asio 保证），与 shard 操作线程一致
				auto aliveFlag = alive_;
				auto* self = this;
				auto* targetLoop = ioLoop;
				conn->onClose(
					[aliveFlag, self, targetLoop, userCb = closeCallback_](const TcpConnection::Ptr& c)
					{
						if (userCb)
						{
							userCb(c);
						}
						if (aliveFlag->load())
						{
							auto& shard = self->findShard(targetLoop);
							self->removeConnection(shard, c);
						}
					});

				// 将连接注册和建立调度到目标 loop 线程
				// 确保 addConnection 在 shard 所属线程内执行（无锁安全）
				if (ioLoop == baseLoop_)
				{
					// 目标就是当前 acceptLoop 所在的 baseLoop，直接操作
					auto& shard = findShard(ioLoop);
					addConnection(shard, conn);

					if (newConnectionCallback_)
					{
						newConnectionCallback_(conn);
					}

					conn->connectEstablished();
				}
				else
				{
					// 跨线程：post 到目标 loop
					auto newConnCb = newConnectionCallback_;
					boost::asio::post(ioLoop->getIoContext(),
									  [self, ioLoop, conn, newConnCb]()
									  {
										  auto& shard = self->findShard(ioLoop);
										  self->addConnection(shard, conn);

										  if (newConnCb)
										  {
											  newConnCb(conn);
										  }

										  conn->connectEstablished();
									  });
				}
			}
			catch (const boost::system::system_error& e)
			{
				if (e.code() == boost::asio::error::operation_aborted)
				{
					break; // acceptor 被关闭
				}

				// fd 用光了：放掉预留 fd → accept 进来立即关 → 再占住预留 fd
				if (e.code() == boost::asio::error::no_descriptors)
				{
					idleFd_.temporaryRelease();
					{
						boost::system::error_code acceptEc;
						tcp::socket tmpSocket(baseLoop_->getIoContext());
						(void)acceptor_.accept(tmpSocket, acceptEc);
					}
					idleFd_.reacquire();
				}

				needSleep = true;
			}

			// MSVC 的 catch 里不能 co_await，挪到外面来
			if (needSleep)
			{
				co_await hical::sleep(0.05);
			}
		}
	}

	Awaitable<void> TcpServer::idleCheckLoop(LoopShard* shard)
	{
		// 扫描间隔：超时时间的 1/4，至少 1 秒
		auto intervalSec = (std::max)(1.0, idleTimeout_ / 4.0);

		while (running_.load() && alive_->load())
		{
			co_await hical::sleep(intervalSec);

			if (!running_.load() || !alive_->load())
			{
				break;
			}

			auto now = std::chrono::steady_clock::now();
			auto timeout = std::chrono::milliseconds(static_cast<int64_t>(idleTimeout_ * 1000));

			// 无锁遍历：本协程运行在 shard->loop 线程上，与 add/remove 串行
			std::vector<TcpConnection::Ptr> toClose;
			for (const auto& conn : shard->connections)
			{
				if (now - conn->lastActiveTime() > timeout)
				{
					toClose.push_back(conn);
				}
			}

			for (const auto& conn : toClose)
			{
				conn->close();
			}
		}
	}

} // namespace hical
