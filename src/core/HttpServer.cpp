#include "HttpServer.h"
#include "MemoryPool.h"
#include "WebSocket.h"
#include <boost/beast/websocket.hpp>
#include <iostream>
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
		}

		acceptor_ = std::make_unique<tcp::acceptor>(ioContext_);
		auto endpoint = tcp::endpoint(tcp::v4(), port_.load());
		acceptor_->open(endpoint.protocol());
		acceptor_->set_option(boost::asio::socket_base::reuse_address(true));
		acceptor_->bind(endpoint);
		acceptor_->listen();

		// 端口 0 时由系统分配，更新实际端口
		port_.store(acceptor_->local_endpoint().port());

		coSpawn(ioContext_, acceptLoop());

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

	Awaitable<void> HttpServer::acceptLoop()
	{
		while (running_.load())
		{
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

				boost::asio::co_spawn(ioContext_, handleSession(std::move(socket)), boost::asio::detached);
			}
			catch (const boost::system::system_error& e)
			{
				if (e.code() == boost::asio::error::operation_aborted || e.code() == boost::asio::error::bad_descriptor)
				{
					break;
				}
				// 瞬态错误（如 EMFILE）继续接受
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

			~ConnectionCounter()
			{
				count.fetch_sub(1);
			}
		} connCounter {activeConnections_};

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

		try
		{
			// 使用请求级 pmr 单调池，整个连接生命周期内复用
			auto requestPool = MemoryPool::instance().createRequestPool();
			std::pmr::polymorphic_allocator<std::byte> alloc(requestPool.get());
			beast::basic_flat_buffer<std::pmr::polymorphic_allocator<std::byte>> buffer(alloc);

			for (;;)
			{
				// 使用 parser 并设置请求大小限制，防止 OOM 攻击
				http::request_parser<http::string_body> parser;
				parser.body_limit(maxBodySize_);
				parser.header_limit(static_cast<std::uint32_t>(maxHeaderSize_));

				// 空闲超时：防止 Slowloris 类攻击，客户端不发数据时自动断开
				if (idleTimeout_ > 0)
				{
					boost::asio::steady_timer deadline(
						socket.get_executor(),
						std::chrono::milliseconds(static_cast<int64_t>(idleTimeout_ * 1000)));
					deadline.async_wait(
						[&socket](const boost::system::error_code& ec)
						{
							if (!ec)
							{
								boost::system::error_code closeEc;
								socket.close(closeEc);
							}
						});

					co_await http::async_read(socket, buffer, parser, boost::asio::use_awaitable);

					deadline.cancel();
				}
				else
				{
					co_await http::async_read(socket, buffer, parser, boost::asio::use_awaitable);
				}

				auto beastReq = parser.release();

				// 检查 WebSocket 升级请求
				if (ws::is_upgrade(beastReq))
				{
					auto reqPath = std::string(beastReq.target());
					auto pos = reqPath.find('?');
					if (pos != std::string::npos)
					{
						reqPath = reqPath.substr(0, pos);
					}

					auto* wsRoute = router_.findWsRoute(reqPath);
					if (wsRoute)
					{
						// socket 所有权转移给 WebSocket 会话，标记 guard 跳过析构
						guard.transferred = true;
						co_await handleWebSocket(std::move(socket), std::move(beastReq), *wsRoute);
						co_return;
					}
				}

				HttpRequest req(std::move(beastReq));

				// 通过中间件管道 + 路由器分发
				HttpResponse res;
				if (middlewarePipeline_.size() > 0)
				{
					res = co_await middlewarePipeline_.execute(req,
															   [this](HttpRequest& r) -> Awaitable<HttpResponse>
															   {
																   co_return co_await router_.dispatch(r);
															   });
				}
				else
				{
					res = co_await router_.dispatch(req);
				}

				// 设置通用头部
				auto& nativeRes = res.native();
				nativeRes.version(11);
				nativeRes.set(http::field::server, "hical/0.2.0");
				nativeRes.keep_alive(req.native().keep_alive());
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
			if (e.code() == http::error::body_limit)
			{
				// 请求体过大：返回 413 Payload Too Large
				http::response<http::string_body> res {http::status::payload_too_large, 11};
				res.set(http::field::server, "hical/0.2.0");
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
		try
		{
			ws::stream<tcp::socket> wsStream(std::move(socket));

			// 接受 WebSocket 升级
			co_await wsStream.async_accept(req, boost::asio::use_awaitable);

			WebSocketSession session(std::move(wsStream));

			// 调用连接回调
			if (wsRoute.onConnect)
			{
				co_await wsRoute.onConnect(session);
			}

			// 消息循环
			while (session.isOpen())
			{
				auto msg = co_await session.receive();
				if (!msg.has_value())
				{
					break;
				}

				if (wsRoute.onMessage)
				{
					co_await wsRoute.onMessage(*msg, session);
				}
			}
		}
		catch (const beast::system_error& e)
		{
			if (e.code() != ws::error::closed && e.code() != boost::asio::error::eof)
			{
				// 忽略正常关闭
			}
		}
	}

} // namespace hical
