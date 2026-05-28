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
		maxConnections_ = maxConns;
	}

	size_t HttpServer::recommendedMaxConnections(size_t availableMemoryMB)
	{
		// 粗估每连接 ~25KB，留 30% 给业务和系统
		constexpr size_t hBytesPerConnection = 25 * 1024;
		size_t usableBytes = availableMemoryMB * 1024 * 1024 * 7 / 10;
		size_t recommended = usableBytes / hBytesPerConnection;
		return std::min(recommended, size_t(65535));
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

		// 注册信号处理（SIGINT/SIGTERM → 优雅关机）
		boost::asio::signal_set signals(baseIoCtx, SIGINT, SIGTERM);
		signals.async_wait(
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

		// 主线程运行 baseLoop（阻塞）
		baseLoop_.run();

		// ── drain 阶段：确保所有协程正常退出，帧自然释放 ──
		//
		// stop() 关 acceptors/timers → abort completions 入 IOCP queue，
		// 但 io_context.stop() 让 run() 立即返回，completions 还没被 dispatch。
		// 如果留到 ~io_context() 才 destroy 协程帧，Windows MinGW GCC 上偶发 SegFault
		// （~awaitable_thread 两阶段 post 销毁 + coroutine edge case）。
		//
		// 解法：先 cancel 所有 pending operations（signal_set / gcTimer / socket），
		// 让它们产生 abort completions，再循环 poll 处理。
		// "double-confirm" 策略：首次 poll()=0 后 sleep 1ms 再 poll 确认 IOCP 无延迟入队。
		// Linux 下 abort completions 同步入队，额外开销仅 1ms sleep。

		// 1. cancel 所有 pending async operations，让它们产生 abort completions
		signals.cancel();
		if (gcTimer_.has_value())
		{
			gcTimer_->cancel();
		}
		for (auto& scanner : idleScanners_)
		{
			scanner->closeAll();
		}

		// 2. drain：循环 poll 处理 abort completions。
		//    多轮是因为：close socket/cancel timer → abort completion 入 IOCP queue → poll 取出 →
		//    协程 resume → 可能产生新的 completions（如 ConnectionCounter 析构触发 stopAllLoops）。
		//    最后一轮 poll()=0 后，针对 Windows IOCP completion 入队的微秒级延迟，
		//    sleep 1ms 再 poll 一次确认确实没有遗漏的 completions。
		{
			bool drained = false;
			for (int i = 0; i < 32; ++i)
			{
				baseLoop_.getIoContext().restart();
				if (baseLoop_.getIoContext().poll() == 0)
				{
					if (drained)
					{
						break; // 连续两次 poll()=0，确认干净了
					}
					// 首次 poll()=0：IOCP completions 可能有微秒级入队延迟，等一下再确认
					std::this_thread::sleep_for(std::chrono::milliseconds(1));
					drained = true;
				}
				else
				{
					drained = false;
				}
			}
		}

		// baseLoop 退出后，停止并等待 ioPool 中的 worker 线程
		if (ioPool_)
		{
			ioPool_->stop();

			// worker 的 io_context 也需要 drain（同理）
			auto workerLoops = ioPool_->getAllLoops();
			for (size_t w = 0; w < workerLoops.size(); ++w)
			{
				if (w + 1 < idleScanners_.size())
				{
					idleScanners_[w + 1]->closeAll();
				}

				bool drained = false;
				for (int i = 0; i < 32; ++i)
				{
					workerLoops[w]->getIoContext().restart();
					if (workerLoops[w]->getIoContext().poll() == 0)
					{
						if (drained)
						{
							break;
						}
						std::this_thread::sleep_for(std::chrono::milliseconds(1));
						drained = true;
					}
					else
					{
						drained = false;
					}
				}
			}
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
		closeAllAcceptors();

		for (auto& scanner : idleScanners_)
		{
			scanner->stop();
		}

		stopAllLoops();
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

					if (maxConnections_ > 0 && activeConnections_.load() >= maxConnections_)
					{
						boost::system::error_code ec;
						socket.close(ec);
						continue;
					}

					socket.set_option(boost::asio::ip::tcp::no_delay(true));

					coSpawn(co_await boost::asio::this_coro::executor, handleSession(std::move(socket)));
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

					if (maxConnections_ > 0 && activeConnections_.load() >= maxConnections_)
					{
						boost::system::error_code ec;
						socket.close(ec);
						continue;
					}

					socket.set_option(boost::asio::ip::tcp::no_delay(true));

					coSpawn(targetIoCtx.get_executor(), handleSession(std::move(socket)));
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
					running_.store(false);
					stopAllLoops();
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
