/**
 * @file HttpSessionImpl.cpp
 * @brief HttpServer 的会话处理实现（编译防火墙）
 *
 * 将 Beast HTTP 解析器/序列化器和 WebSocket 的重模板代码隔离在此翻译单元，
 * 修改 HttpServer 配置逻辑时不会触发 Beast 模板重编译。
 */

#include "HttpServer.h"
#include "MemoryPool.h"
#include "core/Version.h"
#include "WebSocket.h"
#include <boost/beast/websocket.hpp>
#include <optional>

namespace hical
{

	namespace beast = boost::beast;
	namespace http = beast::http;
	namespace ws = beast::websocket;
	using boost::asio::ip::tcp;

	Awaitable<void> HttpServer::handleSession(tcp::socket socket)
	{
		// 连接计数 RAII 守卫
		activeConnections_.fetch_add(1);

		struct ConnectionCounter
		{
			std::atomic<size_t>& count;
			std::atomic<bool>& draining;
			HttpServer& server;

			~ConnectionCounter()
			{
				// fetch_sub 返回旧值；旧值为 1 表示减到 0（最后一个连接）
				if (count.fetch_sub(1) == 1 && draining.load())
				{
					server.stopAllLoops();
				}
			}
		} connCounter {activeConnections_, draining_, *this};

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
			auto* tlPool = MemoryPool::instance().threadLocalPool();

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
				// 每请求独立的 monotonic 池，避免 Keep-Alive 连接上的内存只增不减
				// monotonic_buffer_resource::deallocate() 是空操作，flat_buffer 扩容释放的旧块不会回收
				// 将池放在循环内，每轮请求结束时析构，统一归还所有内存到 thread-local upstream
				std::pmr::monotonic_buffer_resource requestPool(4096, tlPool);
				std::pmr::polymorphic_allocator<std::byte> alloc(&requestPool);
				beast::basic_flat_buffer<std::pmr::polymorphic_allocator<std::byte>> buffer(alloc);

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
						// 同步快速路径：handler 是同步注册时直接调用，跳过协程帧分配
						auto syncResult = router_.dispatchSync(req);
						if (syncResult)
						{
							res = std::move(*syncResult);
						}
						else
						{
							res = co_await router_.dispatch(req);
						}
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

				// 发送响应（prepare_payload 已由 HttpResponse::setBody/setJsonBody 调用）
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

} // namespace hical
