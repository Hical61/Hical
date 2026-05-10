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
		// 每连接约 25KB（PMR 缓冲 4KB + Beast parser ~8KB + socket 缓冲 ~8KB + 开销 ~5KB）
		// 预留 30% 内存给业务逻辑和系统开销
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

		// 预构建中间件调用链，避免每请求重建
		if (middlewarePipeline_.size() > 0)
		{
			middlewarePipeline_.build(
				[this](HttpRequest& req) -> Awaitable<HttpResponse>
				{
					co_return co_await router_.dispatch(req);
				});

			// 预构建 WebSocket 升级专用中间件链（finalHandler 返回 200 占位）
			// 避免每次 WS 升级都动态 buildChain 导致 N 次 std::function 堆分配
			wsMiddlewareChain_ = middlewarePipeline_.buildFor(
				[](HttpRequest&) -> Awaitable<HttpResponse>
				{
					co_return HttpResponse::ok("");
				});
		}

		auto& baseIoCtx = baseLoop_.getIoContext();

		acceptor_ = std::make_unique<tcp::acceptor>(baseIoCtx);
		auto endpoint = tcp::endpoint(tcp::v4(), port_.load());
		acceptor_->open(endpoint.protocol());
		acceptor_->set_option(boost::asio::socket_base::reuse_address(true));
		acceptor_->bind(endpoint);
		acceptor_->listen();

		// 端口 0 时由系统分配，更新实际端口
		port_.store(acceptor_->local_endpoint().port());

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

		coSpawn(baseIoCtx, acceptLoop());

		// 启动内存池 GC 定时器协程
		if (gcInterval_ > 0)
		{
			coSpawn(baseIoCtx, gcLoop());
		}

		// 创建 IO 线程池：worker loop 数量 = ioThreads_ - 1（baseLoop 占 1 个线程）
		if (ioThreads_ > 1)
		{
			ioPool_ = std::make_unique<EventLoopPool>(ioThreads_ - 1);
			ioPool_->start();
		}

		// 主线程运行 baseLoop（阻塞）
		baseLoop_.run();

		// baseLoop 退出后，停止并等待 ioPool 中的 worker 线程
		if (ioPool_)
		{
			ioPool_->stop();
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

		// 将 acceptor 关闭调度到 baseLoop 线程内，与 acceptLoop 串行执行，消除竞态
		auto& baseIoCtx = baseLoop_.getIoContext();
		boost::asio::post(baseIoCtx,
						  [this]()
						  {
							  if (acceptor_)
							  {
								  boost::system::error_code ec;
								  acceptor_->close(ec);
							  }
						  });

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

	Awaitable<void> HttpServer::acceptLoop()
	{
		while (running_.load())
		{
			bool needSleep = false;

			try
			{
				auto socket = co_await acceptor_->async_accept(boost::asio::use_awaitable);

				if (!running_.load())
				{
					break;
				}

				// 连接数限制：超过上限时立即关闭新连接
				if (maxConnections_ > 0 && activeConnections_.load() >= maxConnections_)
				{
					boost::system::error_code ec;
					socket.close(ec);
					continue;
				}

				// 减少 Nagle 延迟
				socket.set_option(boost::asio::ip::tcp::no_delay(true));

				// 将 socket 分发到 worker loop：每个 worker 单线程运行，无需 strand
				auto& targetIoCtx = ioPool_ ? ioPool_->getNextLoop()->getIoContext() : baseLoop_.getIoContext();
				boost::asio::co_spawn(targetIoCtx.get_executor(),
									  handleSession(std::move(socket)),
									  boost::asio::detached);
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
					idleFd_.temporaryRelease();
					{
						boost::system::error_code acceptEc;
						boost::asio::ip::tcp::socket tmpSocket(baseLoop_.getIoContext());
						acceptor_->accept(tmpSocket, acceptEc);
					}
					idleFd_.reacquire();
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
		while (running_.load())
		{
			co_await hical::sleep(gcInterval_);
			if (!running_.load())
			{
				break;
			}
			MemoryPool::instance().gc();
		}
	}

	void HttpServer::gracefulStop()
	{
		if (draining_.exchange(true))
		{
			return;
		}

		auto& baseIoCtx = baseLoop_.getIoContext();

		// 关闭 acceptor，不再接受新连接
		boost::asio::post(baseIoCtx,
						  [this]()
						  {
							  if (acceptor_)
							  {
								  boost::system::error_code ec;
								  acceptor_->close(ec);
							  }
						  });

		// 超时后强制停止（防止活跃连接迟迟不退出）
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
