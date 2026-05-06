#include "HttpServer.h"
#include "MemoryPool.h"
#include "core/Version.h"
#include "WebSocket.h"
#include <boost/beast/websocket.hpp>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <thread>
#include <vector>

namespace hical
{

	namespace beast = boost::beast;
	namespace http = beast::http;
	namespace ws = beast::websocket;
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

		acceptor_ = std::make_unique<tcp::acceptor>(ioContext_);
		auto endpoint = tcp::endpoint(tcp::v4(), port_.load());
		acceptor_->open(endpoint.protocol());
		acceptor_->set_option(boost::asio::socket_base::reuse_address(true));
		acceptor_->bind(endpoint);
		acceptor_->listen();

		// 端口 0 时由系统分配，更新实际端口
		port_.store(acceptor_->local_endpoint().port());

		// 注册信号处理（SIGINT/SIGTERM → 优雅关机）
		boost::asio::signal_set signals(ioContext_, SIGINT, SIGTERM);
		signals.async_wait(
			[this](const boost::system::error_code& ec, int)
			{
				if (!ec)
				{
					gracefulStop();
				}
			});

		coSpawn(ioContext_, acceptLoop());

		// 启动内存池 GC 定时器协程
		if (gcInterval_ > 0)
		{
			coSpawn(ioContext_, gcLoop());
		}

		// 多线程运行 io_context
		std::vector<std::thread> threads;

		// RAII 守卫：确保工作线程在任何退出路径（含异常）都被 join
		struct ThreadJoiner
		{
			std::vector<std::thread>& threads;

			~ThreadJoiner()
			{
				for (auto& t : threads)
				{
					if (t.joinable())
					{
						t.join();
					}
				}
			}
		} joiner {threads};

		for (size_t i = 1; i < ioThreads_; ++i)
		{
			threads.emplace_back(
				[this]()
				{
					ioContext_.run();
				});
		}

		// 主线程也参与运行（阻塞）
		ioContext_.run();

		// joiner 析构时 join 所有线程
		running_.store(false);
	}

	void HttpServer::stop()
	{
		if (!running_.exchange(false))
		{
			return;
		}

		draining_.store(true);

		// 将 acceptor 关闭调度到 io_context 线程内，与 acceptLoop 串行执行，消除竞态
		boost::asio::post(ioContext_,
						  [this]()
						  {
							  if (acceptor_)
							  {
								  boost::system::error_code ec;
								  acceptor_->close(ec);
							  }
						  });

		ioContext_.stop();
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
		return ioContext_;
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

				boost::asio::co_spawn(boost::asio::make_strand(ioContext_.get_executor()),
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
						boost::asio::ip::tcp::socket tmpSocket(ioContext_);
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

	Awaitable<void> HttpServer::handleSession(tcp::socket socket)
	{
		// 连接计数 RAII 守卫
		activeConnections_.fetch_add(1);

		struct ConnectionCounter
		{
			std::atomic<size_t>& count;
			std::atomic<bool>& draining;
			boost::asio::io_context& ioCtx;

			~ConnectionCounter()
			{
				// fetch_sub 返回旧值；旧值为 1 表示减到 0（最后一个连接）
				if (count.fetch_sub(1) == 1 && draining.load())
				{
					ioCtx.stop();
				}
			}
		} connCounter {activeConnections_, draining_, ioContext_};

		// RAII 守卫：确保 socket 在任何退出路径（含异常）都被正确关闭
		// transferred 标志：当 socket 被 move 给 WebSocket 会话后，跳过析构
		struct SocketGuard
		{
			tcp::socket& sock;
			bool transferred {false};

			~SocketGuard()
			{
				if (!transferred && sock.is_open())
				{
					boost::system::error_code ec;
					sock.shutdown(tcp::socket::shutdown_send, ec);
					sock.close(ec);
				}
			}
		} guard {socket};

		// 空闲超时 timer 在 try 外声明，catch 块需要访问以取消 timer 防竞态
		std::optional<boost::asio::steady_timer> deadline;
		if (idleTimeout_ > 0)
		{
			deadline.emplace(socket.get_executor());
		}

		try
		{
			// 使用请求级 pmr 单调池，整个连接生命周期内复用
			auto requestPool = MemoryPool::instance().createRequestPool();
			std::pmr::polymorphic_allocator<std::byte> alloc(requestPool.get());
			beast::basic_flat_buffer<std::pmr::polymorphic_allocator<std::byte>> buffer(alloc);

			// 使用 alive 标志防止 timer 回调在 socket 销毁后访问悬空引用
			auto socketAlive = std::make_shared<std::atomic<bool>>(true);

			// RAII：确保协程退出时标记 socket 已失效，timer 回调不再操作 socket
			struct AliveGuard
			{
				std::shared_ptr<std::atomic<bool>> alive;

				~AliveGuard()
				{
					alive->store(false);
				}
			} aliveGuard {socketAlive};

			for (;;)
			{
				// 使用 parser 并设置请求大小限制，防止 OOM 攻击
				http::request_parser<http::string_body> parser;
				parser.body_limit(maxBodySize_);
				parser.header_limit(static_cast<std::uint32_t>(maxHeaderSize_));

				// 空闲超时：防止 Slowloris 类攻击，客户端不发数据时自动断开
				if (deadline)
				{
					deadline->expires_after(std::chrono::milliseconds(static_cast<int64_t>(idleTimeout_ * 1000)));
					deadline->async_wait(
						[&socket, aliveFlag = socketAlive](const boost::system::error_code& ec)
						{
							if (!ec && aliveFlag->load())
							{
								// dispatch 到 socket 的 executor（strand）上序列化执行，避免与 async_read 竞态
								boost::asio::dispatch(socket.get_executor(),
													  [&socket, aliveFlag]()
													  {
														  if (aliveFlag->load())
														  {
															  boost::system::error_code closeEc;
															  socket.close(closeEc);
														  }
													  });
							}
						});

					co_await http::async_read(socket, buffer, parser, boost::asio::use_awaitable);

					deadline->cancel();
				}
				else
				{
					co_await http::async_read(socket, buffer, parser, boost::asio::use_awaitable);
				}

				auto beastReq = parser.release();

				// 统一构造 HttpRequest（WS 和 HTTP 路径共用）
				HttpRequest req(std::move(beastReq));

				// 检查 WebSocket 升级请求
				if (ws::is_upgrade(req.native()))
				{
					auto reqPath = req.path();

					auto* wsRoute = router_.findWsRoute(reqPath);
					if (wsRoute)
					{
						// Origin 白名单校验（CSWSH 防护）
						if (!wsRoute->allowedOrigins.empty())
						{
							auto origin = std::string(req.header("Origin"));
							if (wsRoute->allowedOrigins.count(origin) == 0)
							{
								HttpResponse forbiddenRes;
								forbiddenRes.setStatus(HttpStatusCode::hForbidden);
								forbiddenRes.setBody("403 Forbidden: Origin not allowed");
								auto& nativeRes = forbiddenRes.native();
								nativeRes.version(11);
								nativeRes.set(http::field::connection, "close");
								nativeRes.prepare_payload();
								co_await http::async_write(socket, nativeRes, boost::asio::use_awaitable);
								break;
							}
						}

						// WebSocket 升级也走中间件管道（认证/限流/日志等）
						if (wsMiddlewareChain_)
						{
							auto wsAuthRes = co_await wsMiddlewareChain_(req);

							auto wsAuthCode = wsAuthRes.statusCode();
							if (wsAuthCode != HttpStatusCode::hOk)
							{
								// 中间件拦截（如 401/403），返回 HTTP 响应拒绝升级
								auto& nativeRes = wsAuthRes.native();
								nativeRes.version(11);
								nativeRes.set(http::field::connection, "close");
								nativeRes.prepare_payload();
								co_await http::async_write(socket, nativeRes, boost::asio::use_awaitable);
								break;
							}
						}

						// socket 所有权转移给 WebSocket 会话，标记 guard 跳过析构
						guard.transferred = true;
						co_await handleWebSocket(std::move(socket), std::move(req.native()), *wsRoute);
						co_return;
					}
				}

				// 通过中间件管道 + 路由器分发（带全局错误处理）
				HttpResponse res;
				try
				{
					if (middlewarePipeline_.size() > 0)
					{
						// build() 已在 start() 中调用，使用无参版本避免每请求构造 std::function
						res = co_await middlewarePipeline_.execute(req);
					}
					else
					{
						res = co_await router_.dispatch(req);
					}
				}
				catch (const std::exception& e)
				{
					if (errorHandler_)
					{
						try
						{
							res = errorHandler_(e, req);
						}
						catch (...)
						{
							// errorHandler 自身抛异常时 fallback
							res = HttpResponse::serverError();
						}
					}
					else
					{
						res = HttpResponse::serverError();
					}
				}
				catch (...)
				{
					res = HttpResponse::serverError();
				}

				// 设置通用头部
				auto& nativeRes = res.native();
				nativeRes.version(11);
				nativeRes.set(http::field::server, HICAL_VERSION_STRING);
				nativeRes.keep_alive(req.native().keep_alive() && !draining_.load());
				nativeRes.prepare_payload();

				// 发送响应
				co_await http::async_write(socket, nativeRes, boost::asio::use_awaitable);

				if (!nativeRes.keep_alive())
				{
					break;
				}
			}
		}
		catch (const beast::system_error& e)
		{
			// 异常路径先取消 timer，防止 timer 回调的 socket.close() 与下方 http::write 竞态
			if (deadline)
			{
				deadline->cancel();
			}

			if (e.code() == http::error::body_limit)
			{
				// 请求体过大：返回 413 Payload Too Large
				http::response<http::string_body> res {http::status::payload_too_large, 11};
				res.set(http::field::server, HICAL_VERSION_STRING);
				res.set(http::field::connection, "close");
				res.body() = "Request body too large";
				res.prepare_payload();
				boost::system::error_code writeEc;
				http::write(socket, res, writeEc);
			}
			else if (e.code() != beast::errc::not_connected && e.code() != boost::asio::error::eof)
			{
				// 忽略正常的连接关闭
			}
		}
		// SocketGuard 析构时自动关闭 socket
	}

	Awaitable<void> HttpServer::handleWebSocket(tcp::socket socket,
												http::request<http::string_body> req,
												const Router::WsRoute& wsRoute)
	{
		std::unique_ptr<WebSocketSession> session;

		// WebSocket 空闲超时 timer 在 try 外声明，catch 块需要访问以取消 timer 防竞态
		std::optional<boost::asio::steady_timer> wsDeadline;
		auto wsTimeoutDuration = std::chrono::milliseconds(static_cast<int64_t>(idleTimeout_ * 1000));
		if (idleTimeout_ > 0)
		{
			wsDeadline.emplace(socket.get_executor());
		}

		try
		{
			ws::stream<tcp::socket> wsStream(std::move(socket));

			// 配置 permessage-deflate 压缩（在 accept 前设置）
			if (wsRoute.enableCompression)
			{
				ws::permessage_deflate opt;
				opt.server_enable = true;
				opt.client_enable = true;
				opt.server_max_window_bits = wsRoute.serverMaxWindowBits;
				opt.client_max_window_bits = wsRoute.clientMaxWindowBits;
				opt.server_no_context_takeover = wsRoute.serverNoContextTakeover;
				opt.compLevel = 6;
				opt.memLevel = 4;
				wsStream.set_option(opt);
			}

			// 接受 WebSocket 升级
			co_await wsStream.async_accept(req, boost::asio::use_awaitable);

			WsCompressionConfig compressionCfg;
			compressionCfg.enabled = wsRoute.enableCompression;
			compressionCfg.serverMaxWindowBits = wsRoute.serverMaxWindowBits;
			compressionCfg.clientMaxWindowBits = wsRoute.clientMaxWindowBits;
			compressionCfg.serverNoContextTakeover = wsRoute.serverNoContextTakeover;

			session = std::make_unique<WebSocketSession>(std::move(wsStream),
														 WebSocketSession::hDefaultMaxMessageSize,
														 compressionCfg);

			// 使用 alive 标志防止 timer 回调在 session 销毁后访问悬空引用
			auto wsAlive = std::make_shared<std::atomic<bool>>(true);

			// RAII：确保协程退出时标记 session 已失效，timer 回调不再操作 session
			struct WsAliveGuard
			{
				std::shared_ptr<std::atomic<bool>> alive;

				~WsAliveGuard()
				{
					alive->store(false);
				}
			} wsAliveGuard {wsAlive};

			// 调用连接回调
			if (wsRoute.onConnect)
			{
				co_await wsRoute.onConnect(*session);
			}

			// 消息循环
			while (session->isOpen())
			{
				// 设置空闲超时（每次读取前重置）
				if (wsDeadline)
				{
					wsDeadline->expires_after(wsTimeoutDuration);
					wsDeadline->async_wait(
						[&session, aliveFlag = wsAlive](const boost::system::error_code& ec)
						{
							if (!ec && aliveFlag->load() && session && session->isOpen())
							{
								// 超时：关闭底层 socket 以中断 async_read
								boost::system::error_code closeEc;
								session->native().next_layer().close(closeEc);
							}
						});
				}

				auto msg = co_await session->receive();

				// 收到消息后取消超时
				if (wsDeadline)
				{
					wsDeadline->cancel();
				}

				if (!msg.has_value())
				{
					break;
				}

				if (wsRoute.onMessage)
				{
					co_await wsRoute.onMessage(*msg, *session);
				}
			}
		}
		catch (const beast::system_error& e)
		{
			// 异常路径先取消 timer，防止 timer 回调在 session 析构后访问悬空引用
			if (wsDeadline)
			{
				wsDeadline->cancel();
			}

			if (e.code() != ws::error::closed && e.code() != boost::asio::error::eof)
			{
				// 忽略正常关闭
			}
		}

		// 连接断开回调（正常退出和异常退出都会触发）
		if (session && wsRoute.onDisconnect)
		{
			try
			{
				co_await wsRoute.onDisconnect(*session);
			}
			catch (...)
			{
				// 忽略断开回调中的异常
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

		// 关闭 acceptor，不再接受新连接
		boost::asio::post(ioContext_,
						  [this]()
						  {
							  if (acceptor_)
							  {
								  boost::system::error_code ec;
								  acceptor_->close(ec);
							  }
						  });

		// 超时后强制停止（防止活跃连接迟迟不退出）
		auto timer = std::make_shared<boost::asio::steady_timer>(ioContext_);
		timer->expires_after(std::chrono::milliseconds(static_cast<int64_t>(shutdownTimeout_ * 1000)));
		timer->async_wait(
			[this, timer](const boost::system::error_code& ec)
			{
				if (!ec)
				{
					running_.store(false);
					ioContext_.stop();
				}
			});
	}

} // namespace hical
