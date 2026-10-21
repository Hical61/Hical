/**
 * @file HttpServer.cpp
 * @brief HTTP 服务器启动与生命周期实现
 */

#include "HttpServer.h"
#include "Log.h"
#include "MemoryPool.h"
#include "RequestDispatch.h"
#include "core/Version.h"
#include <iostream>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace hical
{
	template <typename Func>
	class ScopedGuard
	{
	public:
		ScopedGuard(Func func) : func_(std::move(func))
		{
		}

		~ScopedGuard()
		{
			if (!dismissed_)
			{
				func_();
			}
		}

		ScopedGuard(const ScopedGuard&) = delete;
		ScopedGuard& operator=(const ScopedGuard&) = delete;
		ScopedGuard(ScopedGuard&&) = delete;
		ScopedGuard& operator=(ScopedGuard&&) = delete;

		void dismiss()
		{
			dismissed_ = true;
		}

	private:
		Func func_;
		bool dismissed_ {false};
	};

	using boost::asio::ip::tcp;

	HttpServer::ConnectionReservation::ConnectionReservation(HttpServer& server,
															 std::atomic<size_t>& loopConnections) noexcept
		: server_(&server), loopConnections_(&loopConnections)
	{
		loopConnections_->fetch_add(1, std::memory_order_relaxed);
	}

	HttpServer::ConnectionReservation::ConnectionReservation(ConnectionReservation&& other) noexcept
		: server_(std::exchange(other.server_, nullptr)), loopConnections_(other.loopConnections_)
	{
	}

	HttpServer::ConnectionReservation::~ConnectionReservation()
	{
		if (server_ != nullptr)
		{
			// 先归还 worker 负载，再让最后一个连接触发关服。
			// io_context 销毁未启动帧时也走这里，两个计数都比 io_context 后析构。
			loopConnections_->fetch_sub(1, std::memory_order_relaxed);
			if (server_->activeConnections_.fetch_sub(1, std::memory_order_acq_rel) == 1 && server_->draining_.load())
			{
				server_->stop();
			}
		}
	}

	/**
	 * @brief 接管已 accept 的 socket，完成占位、TLS 包装和协程投递
	 * @param socket 已接受的 socket，按值接管所有权
	 * @param loopConnections 处理该会话的 loop 负载计数
	 * @param spawn 接收 socket 和计数守卫并投递会话的回调
	 * @return true 表示已投递，false 表示超过连接上限并已关闭 socket
	 */
	template <typename SpawnFn>
	bool HttpServer::commitAcceptedSocket(tcp::socket socket, std::atomic<size_t>& loopConnections, SpawnFn&& spawn)
	{
		// accept 后立即占位，不能等 worker 开始跑会话才记账，否则 burst 建连会绕过上限。
		size_t maxConns = maxConnections_.load(std::memory_order_relaxed);
		size_t prev = activeConnections_.fetch_add(1, std::memory_order_acq_rel);
		if (maxConns > 0 && prev >= maxConns)
		{
			activeConnections_.fetch_sub(1, std::memory_order_acq_rel);
			boost::system::error_code ec;
			socket.close(ec);
			return false;
		}

		// 同时占住目标 worker 的负载；下一次 getNextLoop() 必须看到本次分发。
		// 守卫随协程参数移动，TLS 构造、帧分配和 coSpawn 失败都只归还一次。
		ConnectionReservation reservation(*this, loopConnections);

		boost::system::error_code optEc;
		socket.set_option(boost::asio::ip::tcp::no_delay(true), optEc); // 失败不致命，忽略

		// 握手仍由会话内部异步执行，accept 路径只包装 TLS 流。
		if (sslCtx_)
		{
			boost::asio::ssl::stream<tcp::socket> sslStream(std::move(socket), sslCtx_->native());
			spawn(std::move(sslStream), std::move(reservation));
		}
		else
		{
			spawn(std::move(socket), std::move(reservation));
		}

		return true;
	}

	HttpServer::HttpServer(uint16_t port, size_t ioThreads) : port_(port), ioThreads_(ioThreads > 0 ? ioThreads : 1)
	{
	}

	HttpServer::HttpServer() : HttpServer(0, 1)
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

	void HttpServer::use(SyncBeforeHandler before)
	{
		if (started_)
		{
			throw std::logic_error("HttpServer: cannot add middleware after start()");
		}
		middlewarePipeline_.use(std::move(before));
	}

	void HttpServer::use(SyncBeforeHandler before, SyncAfterHandler after)
	{
		if (started_)
		{
			throw std::logic_error("HttpServer: cannot add middleware after start()");
		}
		middlewarePipeline_.use(std::move(before), std::move(after));
	}

	void HttpServer::use(const std::string& name, SyncBeforeHandler before, SyncAfterHandler after)
	{
		if (started_)
		{
			throw std::logic_error("HttpServer: cannot add middleware after start()");
		}
		middlewarePipeline_.use(name, std::move(before), std::move(after));
	}

	void HttpServer::use(SyncAfterHandler after)
	{
		if (started_)
		{
			throw std::logic_error("HttpServer: cannot add middleware after start()");
		}
		middlewarePipeline_.use(std::move(after));
	}

	void HttpServer::use(const std::string& name, SyncAfterHandler after)
	{
		if (started_)
		{
			throw std::logic_error("HttpServer: cannot add middleware after start()");
		}
		middlewarePipeline_.use(name, std::move(after));
	}

	void HttpServer::useBody(MiddlewareHandler middleware)
	{
		if (started_)
		{
			throw std::logic_error("HttpServer: cannot add middleware after start()");
		}
		middlewarePipeline_.useBody(std::move(middleware));
	}

	void HttpServer::useBody(const std::string& name, MiddlewareHandler middleware)
	{
		if (started_)
		{
			throw std::logic_error("HttpServer: cannot add middleware after start()");
		}
		middlewarePipeline_.useBody(name, std::move(middleware));
	}

	void HttpServer::useBody(SyncBeforeHandler before, SyncAfterHandler after)
	{
		if (started_)
		{
			throw std::logic_error("HttpServer: cannot add middleware after start()");
		}
		middlewarePipeline_.useBody(std::move(before), std::move(after));
	}

	void HttpServer::useBody(const std::string& name, SyncBeforeHandler before, SyncAfterHandler after)
	{
		if (started_)
		{
			throw std::logic_error("HttpServer: cannot add middleware after start()");
		}
		middlewarePipeline_.useBody(name, std::move(before), std::move(after));
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
		// 明文口径，转发给带 isTls 的重载，两处估算口径天然一致
		return recommendedMaxConnections(availableMemoryMB, false);
	}

	size_t HttpServer::recommendedMaxConnections(size_t availableMemoryMB, bool isTls)
	{
		// 每连接内存粗估：
		// 明文：readBuf 借还 + PmrBuffer 懒分配后空闲连接约 8KB，
		//      再加上活跃时的临时缓冲和 socket 开销，按 16KB 估。
		// TLS：明文那 16KB 之外，ssl::stream 构造时 eager 分配两个 17KB 的
		//      TLS 记录缓冲（Boost 1.90 boost/asio/ssl/detail/stream_core.hpp 的
		//      max_tls_record_size = 17 * 1024，output_buffer_space_ 和
		//      input_buffer_space_ 各一块），跟收发数据无关，空闲 TLS 连接
		//      光 SSL 层就多占 34KB，所以按 16 + 34 = 50KB 估。
		constexpr size_t kBytesPerConnectionPlain = 16 * 1024;
		constexpr size_t kBytesPerConnectionTls = 50 * 1024;
		// 给推荐值封个顶，免得有人传进来个离谱的 availableMemoryMB 算出天文数字。
		// 封顶 100 万够用了。注意这只管「自动推荐值」，setMaxConnections() 不受这限制，想设更高自己传。
		constexpr size_t kMaxRecommendedConnections = 1'000'000;

		size_t bytesPerConnection = isTls ? kBytesPerConnectionTls : kBytesPerConnectionPlain;
		size_t usableBytes = availableMemoryMB * 1024 * 1024 * 7 / 10;
		size_t recommended = usableBytes / bytesPerConnection;
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

	void HttpServer::addPeriodicTask(const std::string& name,
									 std::chrono::milliseconds interval,
									 std::function<Awaitable<void>()> callback)
	{
		if (started_)
		{
			throw std::logic_error("HttpServer: cannot add periodic task after start()");
		}

		// 0 或负间隔会让定时器立刻反复触发，等于把事件循环打满，直接拒掉
		if (interval <= std::chrono::milliseconds::zero())
		{
			throw std::invalid_argument("HttpServer: periodic task interval must be > 0");
		}

		if (!callback)
		{
			throw std::invalid_argument("HttpServer: periodic task callback must not be empty");
		}

		auto task = std::make_unique<PeriodicTask>();
		task->name = name;
		task->interval = interval;
		task->callback = std::move(callback);
		periodicTasks_.push_back(std::move(task));
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
		auto guardRunning = ScopedGuard(
			[this]()
			{
				running_.store(false);
			});

		// start_ 用于锁定配置，而重启现在不支持，因此一旦 start() 被调用，配置就不能再改了。
		started_.store(true);

		listenLock_.store(true);
		auto guardListenLock = ScopedGuard(
			[this]()
			{
				listenLock_.store(false);
			});

		// 中间件链在此锁定（build 后再 use 会抛异常）并初始化 profiling 统计。
		// build 的 finalHandler 是「终端骨架」：只从请求内部槽取每请求分发上下文并调
		// ctx->invoke(ctx->obj, r)（obj 指向 handleSession 每请求写在栈上的闭包，封装了
		// 「读 body + 用前置 resolveRoute 缓存结果 dispatch」，由它的 for 循环保活）。
		// 链本身只 build 一次，每请求零重建，resolve 全程只匹配一次，中间件后置仍拿到
		// 真实 handler 响应。
		if (middlewarePipeline_.size() > 0)
		{
			middlewarePipeline_.build(
				[](HttpRequest& r) -> Awaitable<HttpResponse>
				{
					return detail::runInternalDispatch(r);
				});

			// WS/SSE 升级也走中间件，这里预构建好链避免每次动态分配。
			// 注意：WS/SSE 不读 body，它的终端是占位 ok("")，与骨架链（读槽）无关——
			// 它们的分支不走 runInternalDispatch，req 也不塞槽。
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

		auto listenEndpoint = tcp::endpoint(listenAddr_, port_.load());

		// SO_REUSEPORT：每个 loop 各有 acceptor，内核负载均衡，省掉跨线程分发。
		// Windows 不支持，走下面的 fallback。
#if defined(__linux__) || defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
		{
			bool allOk = true;
			std::vector<std::unique_ptr<tcp::acceptor>> tempAcceptors;

			for (auto* loop : allLoops)
			{
				auto acc = std::make_unique<tcp::acceptor>(loop->getIoContext());
				acc->open(listenEndpoint.protocol());
				acc->set_option(boost::asio::socket_base::reuse_address(true));

				boost::system::error_code ec;
				using reuse_port = boost::asio::detail::socket_option::boolean<SOL_SOCKET, SO_REUSEPORT>;
				acc->set_option(reuse_port(true), ec);
				if (ec)
				{
					allOk = false;
					break;
				}

				acc->bind(listenEndpoint);
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
			acc->open(listenEndpoint.protocol());
			acc->set_option(boost::asio::socket_base::reuse_address(true));
			acc->bind(listenEndpoint);
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

		// 每个 loop 一个 TimerService，handler 里 co_await hical::sleep() 就挂到它上面，
		// 整条 io_context 共用一个 timerfd。绑定必须在跑那条 io_context 的线程上做：
		// baseLoop_ 的那个就在这儿绑（这线程待会儿就 baseLoop_.run() 了），worker 的交由
		// post 过去，排在下面 acceptLoop 的 coSpawn 之前。
		for (auto* loop : allLoops)
		{
			auto service = std::make_unique<TimerService>(loop->getIoContext().get_executor());
			if (loop == &baseLoop_)
			{
				service->bindToCurrentThread();
			}
			else
			{
				TimerService* rawService = service.get();
				boost::asio::post(loop->getIoContext(),
								  [rawService]()
								  {
									  rawService->bindToCurrentThread();
								  });
			}
			timerServices_.push_back(std::move(service));
		}

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
				coSpawn(allLoops[i]->getIoContext(),
						acceptLoop(*acceptors_[i], *idleFds_[i], allLoops[i]->connectionCount_));
			}
		}
		else
		{
			coSpawn(baseIoCtx, acceptLoop(*acceptors_[0], *idleFds_[0], baseLoop_.connectionCount_));
		}

		// 启动内存池 GC 定时器协程
		if (gcInterval_ > 0)
		{
			coSpawn(baseIoCtx, gcLoop());
		}

		// 启动周期性任务协程：每个任务一个循环，跑在 baseLoop_ 上。
		// 捕获的是 task.get()（裸指针）——periodicTasks_ 里存 unique_ptr，地址稳定，
		// 且 addPeriodicTask 在 start() 之后就抛异常，容器不会再扩容，指针一直有效。
		// 协程体本身隐式持有 this，但 HttpServer 生命周期长于协程：baseLoop_.run() 要等
		// 所有协程（包括这些周期任务）退出后才返回，而 run() 在 start() 里，start() 又在
		// ~HttpServer 之前——所以 this 和 periodicTasks_ 在协程活着期间一直有效。
		for (auto& task : periodicTasks_)
		{
			coSpawn(baseIoCtx, runPeriodicTask(task.get()));
		}

		// 主线程阻塞在这。work_guard 保证不会因为"暂时没活"就提前返回。
		// stop() 把所有 pending op cancel 掉再 releaseWork()，协程走完 run() 就退了。
		// 这样退出时 IOCP queue 是空的，不会触发两阶段 destroy 那个 SegFault。
		baseLoop_.run();

		// 趁 io_context 还活着把 signal_set 干掉——Windows 下这玩意有 static mutex，
		// 不提前清理的话同一进程里下次创建可能踩到脏状态
		signals_.reset();

		// 这条线程的 thread_local 还指着 baseLoop_ 的服务（timerServices_[0] 就是它）。
		// 服务本身要等析构才没，但它那条 io_context 已经没人跑了，这条线程后面要是再调
		// currentThreadTimerService() 拿到的就是个不会再触发的服务。跟 IdleScanner
		// 在 run() 收尾时把 tls 清掉一个道理。
		timerServices_[0]->unbindFromCurrentThread();

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

		// 走到这儿说明 start() 是正常收尾的（包括 stop() 之后返回），监听地址已经用掉一次，
		// 锁就保持住——重启本来也不支持。只有中途抛异常才会让 guardListenLock 把锁放回去，
		// 那种情况下用户改个端口重试是合理的。
		guardListenLock.dismiss();
	}

	void HttpServer::stop()
	{
		if (!running_.exchange(false))
		{
			return;
		}

		draining_.store(true);
		closeAllAcceptors();

		// 通知周期任务停机。置标志用 atomic，随便哪个线程写都行；协程在每轮
		// callback 前后都会看它，调用户回调时即使正卡在里面，回调一返回就会退出循环。
		for (auto& task : periodicTasks_)
		{
			task->stopRequested.store(true);
		}

		// timer/socket 操作不是线程安全的，stop() 可能从外面调进来，
		// 所以把真正的 shutdown 动作 post 到 io_context 线程里做
		boost::asio::post(baseLoop_.getIoContext(),
						  [this]()
						  {
							  if (signals_.has_value())
							  {
								  signals_->cancel();
							  }

							  if (gcTimer_.has_value())
							  {
								  gcTimer_->cancel();
							  }

							  // 周期任务定时器的 cancel 必须在 io_context 线程上做。
							  // 协程大多数时候挂在 async_wait 上等下一个 interval，如果从别的
							  // 线程直接 cancel，会和协程「重新 arm 定时器」的时序赛跑：万一
							  // cancel 落在两次 async_wait 之间的空档，就没人唤醒协程，run()
							  // 得干等满一个 interval 才退出。放到这儿和协程串行执行，cancel
							  // 必定命中挂起的 wait（timer 为 null 说明协程还没建/已建完退出）。
							  for (auto& task : periodicTasks_)
							  {
								  if (task->timer)
								  {
									  task->timer->cancel();
								  }
							  }

							  // 只操作本 loop 的链表；worker 会在各自的回调中关闭连接和 timer。
							  for (auto& scanner : idleScanners_)
							  {
								  if (scanner->getExecutor() == baseLoop_.getIoContext().get_executor())
								  {
									  scanner->closeAll();
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

	void HttpServer::listen(const tcp::endpoint& ep)
	{
		if (listenLock_.load())
		{
			throw std::logic_error("HttpServer: cannot change listen address after start()");
		}

		listenAddr_ = ep.address();
		port_.store(ep.port());
	}

	void HttpServer::listen(const std::string& ip, uint16_t port)
	{
		auto addr = boost::asio::ip::make_address(ip);
		listen(tcp::endpoint(addr, port));
	}

	void HttpServer::listenAny(uint16_t port)
	{
		listen(tcp::endpoint(tcp::v4(), port));
	}

	tcp::endpoint HttpServer::endpoint() const
	{
		return {listenAddr_, port_.load()};
	}

	uint16_t HttpServer::port() const
	{
		return port_.load();
	}

	boost::asio::io_context& HttpServer::ioContext()
	{
		return baseLoop_.getIoContext();
	}

	size_t HttpServer::responseWriteCount() const
	{
		return responseWriteCount_.load(std::memory_order_relaxed);
	}

	void HttpServer::enableResponseWriteCount()
	{
		if (started_.load(std::memory_order_relaxed))
		{
			throw std::logic_error("HttpServer: enableResponseWriteCount() must be called before start()");
		}
		writeCountEnabled_.store(true, std::memory_order_relaxed);
	}

	Awaitable<void> HttpServer::acceptLoop(tcp::acceptor& acceptor,
										   IdleFd& idleFd,
										   std::atomic<size_t>& acceptorConnections)
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

					// 收尾逻辑与回退路径共用，差异只有「socket 从哪来」和「用哪个 executor 投递」
					auto executor = co_await boost::asio::this_coro::executor;
					auto spawn = [this, executor](auto&& streamOrSocket, ConnectionReservation reservation)
					{
						coSpawn(executor,
								handleSession(std::forward<decltype(streamOrSocket)>(streamOrSocket),
											  std::move(reservation)));
					};
					if (!commitAcceptedSocket(std::move(socket), acceptorConnections, spawn))
					{
						continue;
					}
				}
				else
				{
					// 回退路径（Windows 等无 SO_REUSEPORT 平台）：
					// 借鉴 Cinatra 策略——socket 直接创建在目标 worker 的 io_context 上，
					// 使 socket 的 IOCP/epoll 关联从一开始就在正确的线程，减少迁移开销。
					auto* targetLoop = ioPool_ ? ioPool_->getNextLoop() : &baseLoop_;
					auto& targetIoCtx = targetLoop->getIoContext();
					tcp::socket socket(targetIoCtx.get_executor());

					co_await acceptor.async_accept(socket, boost::asio::use_awaitable);

					if (!running_.load())
					{
						break;
					}

					auto spawn = [this, exec = targetIoCtx.get_executor()](auto&& streamOrSocket,
																		   ConnectionReservation reservation)
					{
						coSpawn(exec,
								handleSession(std::forward<decltype(streamOrSocket)>(streamOrSocket),
											  std::move(reservation)));
					};
					if (!commitAcceptedSocket(std::move(socket), targetLoop->connectionCount_, spawn))
					{
						continue;
					}
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

	Awaitable<void> HttpServer::runPeriodicTask(PeriodicTask* task)
	{
		// timer 在协程体内建：executor 取当前协程的，自然绑在 baseLoop_ 上。
		// 用 shared_ptr 是为了 stop() 侧能拿到同一个对象去 cancel。
		auto executor = co_await boost::asio::this_coro::executor;
		task->timer = std::make_shared<boost::asio::steady_timer>(executor);

		// 周期语义：上一轮 callback 跑完，再等 interval 触发下一轮——不是固定频率。
		// 这样某轮 callback 偶尔跑超了，下一轮从「这次结束」重新计时，任务不会堆积。
		while (!task->stopRequested.load())
		{
			task->timer->expires_after(task->interval);
			boost::system::error_code ec;
			co_await task->timer->async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, ec));

			// cancel 会让 async_wait 带 operation_aborted 返回；stopRequested 是双保险
			if (ec || task->stopRequested.load())
			{
				break;
			}

			try
			{
				co_await task->callback();
			}
			catch (const std::exception& e)
			{
				// 一轮失败不能把整个任务带走，记日志，下一轮照跑
				HICAL_LOG_ERROR("periodic task [{}] callback failed: {}", task->name, e.what());
			}
			catch (...)
			{
				HICAL_LOG_ERROR("periodic task [{}] callback failed: unknown exception", task->name);
			}
		}

		// 协程收尾：把 timer 引用放掉。stop() 侧可能已经 cancel 过它，这里只是释放。
		task->timer.reset();
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
		// 多线程并发调用此函数是安全的（ConnectionReservation 析构 / gracefulStop 超时可能同时触发）。
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
