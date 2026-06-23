/**
 * @file HttpServer.cpp
 * @brief HTTP 服务器启动与生命周期实现
 */

#include "HttpServer.h"
#include "MemoryPool.h"
#include "core/Version.h"
#include <iostream>
#include <optional>
#include <stdexcept>
#include <thread>
#include <vector>

namespace hical
{

	using boost::asio::ip::tcp;

	HttpServer::HttpServer(uint16_t port, size_t ioThreads) : port_(port), ioThreads_(ioThreads > 0 ? ioThreads : 1)
	{
	}

	HttpServer::~HttpServer()
	{
		if (running_.load())
		{
			stop();
		}

		// 趁 baseLoop_/ioPool_ 的 io_context 还活着，把 scanner 里的 timer 先干掉。
		// idleScanners_ 比 io_context 后析构（声明在前），如果不提前 reset timer，
		// io_context 析构后 timer 析构访问已销毁的 timer_service 就是 UB。
		// stop() 已经在各自 io_context 线程上 closeAll + stop + shutdown 把 timer
		// 在正确线程上销毁了，这里的 timer_ 已经是 nullopt；下面这行是兜底——如果
		// 有人析构前没调 stop()。
		for (auto& scanner : idleScanners_)
		{
			scanner->shutdown();
		}
	}

	Router& HttpServer::router()
	{
		if (started_)
		{
			throw std::logic_error("HttpServer: cannot modify router after start()");
		}
		return router_;
	}

	void HttpServer::use(MiddlewareHandler middleware)
	{
		if (started_)
		{
			throw std::logic_error("HttpServer: cannot add middleware after start()");
		}
		middlewarePipeline_.use(std::move(middleware));
	}

	void HttpServer::use(const std::string& name, MiddlewareHandler middleware)
	{
		if (started_)
		{
			throw std::logic_error("HttpServer: cannot add middleware after start()");
		}
		middlewarePipeline_.use(name, std::move(middleware));
	}

#ifdef HICAL_ENABLE_MIDDLEWARE_PROFILING
	std::vector<MiddlewarePipeline::TimingSnapshot> HttpServer::middlewareStats() const
	{
		return middlewarePipeline_.getTimingStats();
	}
#endif

	void HttpServer::setMaxBodySize(size_t bytes)
	{
		maxBodySize_ = bytes;
	}

	void HttpServer::setMaxHeaderSize(size_t bytes)
	{
		maxHeaderSize_ = bytes;
	}

	void HttpServer::setMaxConnections(size_t maxConns)
	{
		// relaxed 足够：这只是个阈值，不需要和别的内存操作建立 happens-before
		maxConnections_.store(maxConns, std::memory_order_relaxed);
	}

	size_t HttpServer::recommendedMaxConnections(size_t availableMemoryMB)
	{
		// 每连接内存粗估：readBuf 借还 + PmrBuffer 懒分配后空闲连接约 8KB，
		// 再加上活跃时的临时缓冲和 socket 开销，按 16KB 估。
		constexpr size_t kBytesPerConnection = 16 * 1024;
		// 给推荐值封个顶，免得有人传进来个离谱的 availableMemoryMB 算出天文数字。
		// 封顶 100 万够用了。注意这只管「自动推荐值」，setMaxConnections() 不受这限制，想设更高自己传。
		constexpr size_t kMaxRecommendedConnections = 1'000'000;

		size_t usableBytes = availableMemoryMB * 1024 * 1024 * 7 / 10;
		size_t recommended = usableBytes / kBytesPerConnection;
		return std::min(recommended, kMaxRecommendedConnections);
	}

	void HttpServer::setIdleTimeout(double seconds)
	{
		idleTimeout_ = seconds;
	}

	void HttpServer::setGcInterval(double seconds)
	{
		gcInterval_ = seconds;
	}

	void HttpServer::setShutdownTimeout(double seconds)
	{
		shutdownTimeout_ = seconds;
	}

	void HttpServer::setErrorHandler(ErrorHandler handler)
	{
		if (started_)
		{
			throw std::logic_error("HttpServer: cannot set error handler after start()");
		}
		errorHandler_ = std::move(handler);
	}

	void HttpServer::enableSsl(const std::string& certFile, const std::string& keyFile)
	{
		sslCtx_ = std::make_shared<SslContext>(boost::asio::ssl::context::tls_server);
		sslCtx_->loadCertificate(certFile);
		sslCtx_->loadPrivateKey(keyFile);
	}

	void HttpServer::start()
	{
		running_.store(true);
		started_ = true;

		// 中间件链预构建
		if (middlewarePipeline_.size() > 0)
		{
			middlewarePipeline_.build(
				[this](HttpRequest& req) -> Awaitable<HttpResponse>
				{
					co_return co_await router_.dispatch(req);
				});

			// WS 升级也走中间件，这里预构建好链避免每次动态分配
			wsMiddlewareChain_ = middlewarePipeline_.buildFor(
				[](HttpRequest&) -> Awaitable<HttpResponse>
				{
					co_return HttpResponse::ok("");
				});
		}

		// 创建 IO 线程池（SO_REUSEPORT 需要所有 loop 先跑起来）
		if (ioThreads_ > 1)
		{
			ioPool_ = std::make_unique<EventLoopPool>(ioThreads_ - 1);
			ioPool_->start();
		}

		// 收集所有 loop（baseLoop + workers）
		std::vector<AsioEventLoop*> allLoops;
		allLoops.push_back(&baseLoop_);
		if (ioPool_)
		{
			for (auto* loop : ioPool_->getAllLoops())
			{
				allLoops.push_back(loop);
			}
		}

		auto endpoint = tcp::endpoint(tcp::v4(), port_.load());

		// SO_REUSEPORT：每个 loop 各有 acceptor，内核负载均衡，省掉跨线程分发。
		// Windows 不支持，走下面的 fallback。
#if defined(__linux__) || defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
		{
			bool allOk = true;
			std::vector<std::unique_ptr<tcp::acceptor>> tempAcceptors;

			for (auto* loop : allLoops)
			{
				auto acc = std::make_unique<tcp::acceptor>(loop->getIoContext());
				acc->open(endpoint.protocol());
				acc->set_option(boost::asio::socket_base::reuse_address(true));

				boost::system::error_code ec;
				using reuse_port = boost::asio::detail::socket_option::boolean<SOL_SOCKET, SO_REUSEPORT>;
				acc->set_option(reuse_port(true), ec);
				if (ec)
				{
					allOk = false;
					break;
				}

				acc->bind(endpoint);
				acc->listen();
				tempAcceptors.push_back(std::move(acc));
			}

			if (allOk && tempAcceptors.size() == allLoops.size())
			{
				reusePortEnabled_ = true;
				acceptors_ = std::move(tempAcceptors);
			}
		}
#endif

		// 回退：单 acceptor（Windows / SO_REUSEPORT 不可用）
		if (!reusePortEnabled_)
		{
			auto acc = std::make_unique<tcp::acceptor>(baseLoop_.getIoContext());
			acc->open(endpoint.protocol());
			acc->set_option(boost::asio::socket_base::reuse_address(true));
			acc->bind(endpoint);
			acc->listen();
			acceptors_.push_back(std::move(acc));
		}

		// 端口 0 时由系统分配，更新实际端口
		port_.store(acceptors_[0]->local_endpoint().port());

		// 每个 acceptor 配独立 IdleFd（EMFILE 保护）
		for (size_t i = 0; i < acceptors_.size(); ++i)
		{
			idleFds_.push_back(std::make_unique<IdleFd>());
		}

		auto& baseIoCtx = baseLoop_.getIoContext();

		// 信号处理（Ctrl+C / kill → 优雅关机）
		// Windows 下 signal_set 有个 static mutex 的坑，同一进程反复创建销毁会炸，
		// 但生产环境进程只起一次所以没事
		signals_.emplace(baseIoCtx, SIGINT, SIGTERM);
		signals_->async_wait(
			[this](const boost::system::error_code& ec, int)
			{
				if (!ec)
				{
					gracefulStop();
				}
			});

		// 给每个 io_context 跑一个空闲扫描器（干掉 per-connection timer 协程）
		if (idleTimeout_ > 0)
		{
			auto timeoutMs = static_cast<int64_t>(idleTimeout_ * 1000);
			for (auto* loop : allLoops)
			{
				auto scanner = std::make_unique<IdleScanner>(loop->getIoContext().get_executor(), timeoutMs);
				coSpawn(loop->getIoContext(), scanner->run());
				idleScanners_.push_back(std::move(scanner));
			}
		}

		// 在每个 loop 上启动独立 acceptLoop
		if (reusePortEnabled_)
		{
			for (size_t i = 0; i < allLoops.size(); ++i)
			{
				coSpawn(allLoops[i]->getIoContext(), acceptLoop(*acceptors_[i], *idleFds_[i]));
			}
		}
		else
		{
			coSpawn(baseIoCtx, acceptLoop(*acceptors_[0], *idleFds_[0]));
		}

		// 启动内存池 GC 定时器协程
		if (gcInterval_ > 0)
		{
			coSpawn(baseIoCtx, gcLoop());
		}

		// 主线程阻塞在这。work_guard 保证不会因为"暂时没活"就提前返回。
		// stop() 把所有 pending op cancel 掉再 releaseWork()，协程走完 run() 就退了。
		// 这样退出时 IOCP queue 是空的，不会触发两阶段 destroy 那个 SegFault。
		baseLoop_.run();

		// 趁 io_context 还活着把 signal_set 干掉——Windows 下这玩意有 static mutex，
		// 不提前清理的话同一进程里下次创建可能踩到脏状态
		signals_.reset();

		// worker loop 同理——socket 已经被 closeAll 关了，放掉 work_guard 等它们退
		if (ioPool_)
		{
			ioPool_->stop();
		}

		// 趁 io_context 还活着，把 scanner 里的 timer 先干掉。
		// 不然等 io_context 析构完 timer_service 没了，scanner 析构时 timer 就踩野内存
		for (auto& scanner : idleScanners_)
		{
			scanner->shutdown();
		}

		running_.store(false);
	}

	void HttpServer::stop()
	{
		if (!running_.exchange(false))
		{
			return;
		}

		draining_.store(true);

		// timer/socket 操作不是线程安全的，stop() 可能从外面调进来，
		// 所以把真正的 shutdown 动作 post 到 io_context 线程里做
		boost::asio::post(baseLoop_.getIoContext(),
						  [this]()
						  {
							  // 关 acceptor → acceptLoop 自然退出
							  for (auto& acc : acceptors_)
							  {
								  boost::system::error_code ec;
								  acc->close(ec);
							  }

							  if (signals_.has_value())
							  {
								  signals_->cancel();
							  }

							  if (gcTimer_.has_value())
							  {
								  gcTimer_->cancel();
							  }

							  // 把所有还活着的连接 socket 关掉，协程自然收到错误退出
							  for (auto& scanner : idleScanners_)
							  {
								  scanner->closeAll();
							  }

							  // scanner 的 scan timer 也 cancel 掉
							  for (auto& scanner : idleScanners_)
							  {
								  if (scanner->getExecutor() == baseLoop_.getIoContext().get_executor())
								  {
									  scanner->stop();
									  scanner->shutdown();
								  }
							  }
						  });

		// worker loop 上的 scanner 同理
		if (ioPool_)
		{
			for (size_t i = 1; i < idleScanners_.size(); ++i)
			{
				auto& scanner = idleScanners_[i];
				boost::asio::post(scanner->getExecutor(),
								  [s = scanner.get()]()
								  {
									  s->closeAll();
									  s->stop();
									  s->shutdown();
								  });
			}
		}

		// 放掉 work_guard，等协程全走完 run() 就自然退出了。
		// 不调 io_context::stop()——那玩意会留一堆没 dispatch 的 completion 在 IOCP queue 里，
		// 然后 ~io_context 暴力 destroy 协程帧，GCC MinGW 就炸了。
		baseLoop_.releaseWork();
	}

	bool HttpServer::isRunning() const
	{
		return running_.load();
	}

	uint16_t HttpServer::port() const
	{
		return port_.load();
	}

	boost::asio::io_context& HttpServer::ioContext()
	{
		return baseLoop_.getIoContext();
	}

	Awaitable<void> HttpServer::acceptLoop(tcp::acceptor& acceptor, IdleFd& idleFd)
	{
		while (running_.load())
		{
			bool needSleep = false;

			try
			{
				if (reusePortEnabled_)
				{
					// SO_REUSEPORT 路径：socket 已在当前 loop 上，零跨线程调度
					auto socket = co_await acceptor.async_accept(boost::asio::use_awaitable);

					if (!running_.load())
					{
						break;
					}

					// 先占位再校验：原子地把计数 +1。绝不能像以前那样「先 load 判断、再到
					// handleSession 里 fetch_add」——handleSession 是 coSpawn 异步投递的，
					// 高速 burst 建连时大批连接已 accept 但协程还没被调度执行 fetch_add，
					// load() 读到的计数严重滞后，限流直接形同虚设。占位即计数才是硬上限。
					size_t maxConns = maxConnections_.load(std::memory_order_relaxed);
					size_t prev = activeConnections_.fetch_add(1, std::memory_order_acq_rel);
					if (maxConns > 0 && prev >= maxConns)
					{
						activeConnections_.fetch_sub(1, std::memory_order_acq_rel);
						boost::system::error_code ec;
						socket.close(ec);
						continue;
					}

					// 占位成功，计数所有权移交 handleSession（由它析构时 fetch_sub）。
					// committed 之前任何提前返回/抛出都要回退占位，否则计数泄漏。
					bool committed = false;

					struct AcceptGuard
					{
						std::atomic<size_t>& count;
						bool& committed;

						~AcceptGuard()
						{
							if (!committed)
							{
								count.fetch_sub(1, std::memory_order_acq_rel);
							}
						}
					} acceptGuard {activeConnections_, committed};

					boost::system::error_code optEc;
					socket.set_option(boost::asio::ip::tcp::no_delay(true), optEc); // 失败不致命，忽略

					coSpawn(co_await boost::asio::this_coro::executor, handleSession(std::move(socket)));
					committed = true; // 移交成功
				}
				else
				{
					// 回退路径（Windows 等无 SO_REUSEPORT 平台）：
					// 借鉴 Cinatra 策略——socket 直接创建在目标 worker 的 io_context 上，
					// 使 socket 的 IOCP/epoll 关联从一开始就在正确的线程，减少迁移开销。
					auto& targetIoCtx = ioPool_ ? ioPool_->getNextLoop()->getIoContext() : baseLoop_.getIoContext();
					tcp::socket socket(targetIoCtx.get_executor());

					co_await acceptor.async_accept(socket, boost::asio::use_awaitable);

					if (!running_.load())
					{
						break;
					}

					// 同 SO_REUSEPORT 路径：先占位再校验，占位即计数（硬上限）
					size_t maxConns = maxConnections_.load(std::memory_order_relaxed);
					size_t prev = activeConnections_.fetch_add(1, std::memory_order_acq_rel);
					if (maxConns > 0 && prev >= maxConns)
					{
						activeConnections_.fetch_sub(1, std::memory_order_acq_rel);
						boost::system::error_code ec;
						socket.close(ec);
						continue;
					}

					bool committed = false;

					struct AcceptGuard
					{
						std::atomic<size_t>& count;
						bool& committed;

						~AcceptGuard()
						{
							if (!committed)
							{
								count.fetch_sub(1, std::memory_order_acq_rel);
							}
						}
					} acceptGuard {activeConnections_, committed};

					boost::system::error_code optEc;
					socket.set_option(boost::asio::ip::tcp::no_delay(true), optEc); // 失败不致命，忽略

					coSpawn(targetIoCtx.get_executor(), handleSession(std::move(socket)));
					committed = true; // 移交成功
				}
			}
			catch (const boost::system::system_error& e)
			{
				if (e.code() == boost::asio::error::operation_aborted || e.code() == boost::asio::error::bad_descriptor)
				{
					break;
				}

				// fd 耗尽处理：释放预留 fd → accept 并关闭 → 重新预留
				if (e.code() == boost::asio::error::no_descriptors)
				{
					idleFd.temporaryRelease();
					{
						boost::system::error_code acceptEc;
						boost::asio::ip::tcp::socket tmpSocket(acceptor.get_executor());
						acceptor.accept(tmpSocket, acceptEc);
					}
					idleFd.reacquire();
				}

				needSleep = true;
			}

			// MSVC 不允许在 catch 块内 co_await，延迟到块外
			if (needSleep)
			{
				co_await hical::sleep(0.05);
			}
		}
	}

	Awaitable<void> HttpServer::gcLoop()
	{
		auto executor = co_await boost::asio::this_coro::executor;
		gcTimer_.emplace(executor);

		while (running_.load())
		{
			gcTimer_->expires_after(std::chrono::milliseconds(static_cast<int64_t>(gcInterval_ * 1000)));
			boost::system::error_code ec;
			co_await gcTimer_->async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, ec));

			if (ec || !running_.load())
			{
				break;
			}
			MemoryPool::instance().gc();
		}

		gcTimer_.reset();
	}

	void HttpServer::gracefulStop()
	{
		if (draining_.exchange(true))
		{
			return;
		}

		closeAllAcceptors();

		// 超时后强制停止（防止活跃连接迟迟不退出）
		auto& baseIoCtx = baseLoop_.getIoContext();
		auto timer = std::make_shared<boost::asio::steady_timer>(baseIoCtx);
		timer->expires_after(std::chrono::milliseconds(static_cast<int64_t>(shutdownTimeout_ * 1000)));
		timer->async_wait(
			[this, timer](const boost::system::error_code& ec)
			{
				if (!ec)
				{
					stop();
				}
			});
	}

	void HttpServer::closeAllAcceptors()
	{
		// 将每个 acceptor 的关闭调度到其所在 loop 线程内，与 acceptLoop 串行执行，消除竞态
		for (auto& acc : acceptors_)
		{
			boost::asio::post(acc->get_executor(),
							  [&acc]()
							  {
								  boost::system::error_code ec;
								  acc->close(ec);
							  });
		}
	}

	void HttpServer::stopAllLoops()
	{
		// baseLoop_.stop() 和 AsioEventLoop::stop() 都是幂等的（内部 atomic + ioContext_.stop()），
		// 多线程并发调用此函数是安全的（ConnectionCounter 析构 / gracefulStop 超时可能同时触发）。
		// ioPool_ 生命周期由 start() 保证：ioPool_->stop() 在 baseLoop_.run() 返回后才执行，
		// 而 baseLoop_.run() 返回前 ioPool_ 必然存活。
		baseLoop_.stop();
		if (ioPool_)
		{
			for (auto* loop : ioPool_->getAllLoops())
			{
				loop->stop();
			}
		}
	}

} // namespace hical
