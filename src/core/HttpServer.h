/**
 * @file HttpServer.h
 * @brief HTTP 服务器顶层门面
 */

#pragma once

#include "Router.h"
#include "Middleware.h"
#include "HttpRequest.h"
#include "HttpResponse.h"
#include "Coroutine.h"
#include "SslContext.h"
#include "IdleFd.h"
#include "IdleScanner.h"
#include "../asio/AsioEventLoop.h"
#include "../asio/EventLoopPool.h"
#include <boost/asio.hpp>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace hical
{

	/**
	 * @brief HTTP 服务器
	 * 高层封装：整合 TcpServer + Router + 中间件管道。
	 * 提供简洁的 API 配置路由、中间件，一键启动。
	 * 用法：
	 * ```cpp
	 * hical::HttpServer server(8080);
	 * server.router().get("/", handler);
	 * server.use(logMiddleware);
	 * server.start();  // 阻塞
	 * ```
	 * @note 线程模型
	 * 采用 1 Thread : 1 io_context 架构。baseLoop_ 运行 accept/signal/GC，
	 * ioPool_ 中的 worker loop 处理连接 I/O。每个 worker loop 单线程运行，
	 * 同一 loop 上的协程天然串行，无需 strand。当 ioThreads > 1 时，
	 * 不同 loop 上的路由 handler 仍可能并发调用。
	 */
	class HttpServer
	{
	public:
		/**
		 * @brief 构造 HTTP 服务器
		 * @param port 监听端口
		 * @param ioThreads IO 线程数（默认 1，即单线程）
		 */
		explicit HttpServer(uint16_t port, size_t ioThreads = 1);

		~HttpServer();

		/**
		 * @brief 获取路由器引用（用于注册路由）
		 * @return 路由器引用
		 */
		[[nodiscard]] Router& router();

		/**
		 * @brief 添加中间件
		 * @param middleware 中间件处理器
		 */
		void use(MiddlewareHandler middleware);

		/**
		 * @brief 添加命名中间件（启用 profiling 时记录名称用于统计）
		 * @param name 中间件名称
		 * @param middleware 中间件处理器
		 */
		void use(const std::string& name, MiddlewareHandler middleware);

#ifdef HICAL_ENABLE_MIDDLEWARE_PROFILING
		/**
		 * @brief 获取中间件计时统计快照
		 * @return 各层中间件的统计数据
		 */
		[[nodiscard]] std::vector<MiddlewarePipeline::TimingSnapshot> middlewareStats() const;
#endif

		/**
		 * @brief 启用 SSL/TLS
		 * @param certFile 证书文件路径
		 * @param keyFile 私钥文件路径
		 */
		void enableSsl(const std::string& certFile, const std::string& keyFile);

		/**
		 * @brief 设置最大请求体大小
		 * @param bytes 最大字节数（默认 1MB）
		 */
		void setMaxBodySize(size_t bytes);

		/**
		 * @brief 设置最大请求头大小
		 * @param bytes 最大字节数（默认 8KB）
		 */
		void setMaxHeaderSize(size_t bytes);

		/**
		 * @brief 设置最大并发连接数
		 * @param maxConns 最大连接数（0 表示不限制，默认 10000）
		 * @note 过大的值可能在低配服务器上被连接洪水攻击导致 OOM。
		 * 可使用 recommendedMaxConnections() 根据可用内存计算推荐值。
		 */
		void setMaxConnections(size_t maxConns);

		/**
		 * @brief 根据可用内存计算推荐的最大连接数
		 * 按每连接约 16KB 估算（readBuf 借还 + PmrBuffer 懒分配后空闲约 8KB，
		 * 再加活跃连接临时缓冲和 socket 开销），
		 * 预留 30% 内存给业务逻辑和系统开销。
		 * @param availableMemoryMB 可用内存（MB）
		 * @return 推荐的最大连接数（最多 100 万；只管推荐值，setMaxConnections 不受此限）
		 */
		[[nodiscard]] static size_t recommendedMaxConnections(size_t availableMemoryMB);

		/**
		 * @brief 设置空闲连接超时时间
		 * @param seconds 超时秒数（0 表示不超时，默认 60 秒）
		 */
		void setIdleTimeout(double seconds);

		/**
		 * @brief 设置内存池 GC 间隔
		 * @param seconds GC 间隔秒数（0 表示关闭自动 GC，默认 60 秒）
		 */
		void setGcInterval(double seconds);

		/**
		 * @brief 设置优雅关机超时时间
		 * @param seconds 超时秒数（默认 30 秒，超时后强制关闭）
		 */
		void setShutdownTimeout(double seconds);

		/**
		 * @brief 错误处理器类型
		 * 当路由 handler 或中间件抛出异常时调用，返回自定义错误响应。
		 * @param e 捕获的异常
		 * @param req 当前请求
		 * @return 错误响应
		 */
		using ErrorHandler = std::function<HttpResponse(const std::exception& e, const HttpRequest& req)>;

		/**
		 * @brief 设置全局错误处理器
		 * @param handler 错误处理回调
		 * @note 必须在 start() 之前调用
		 */
		void setErrorHandler(ErrorHandler handler);

		/**
		 * @brief 启动服务器（阻塞）
		 * 调用后阻塞当前线程，直到 stop() 被调用。
		 * @warning 当 ioThreads > 1 时，路由 handler 可能被多个线程并发调用，
		 * handler 中访问的任何共享状态必须自行保证线程安全。
		 */
		void start();

		/**
		 * @brief 停止服务器
		 */
		void stop();

		/**
		 * @brief 服务器是否正在运行
		 * @return true 如果正在运行
		 */
		[[nodiscard]] bool isRunning() const;

		/**
		 * @brief 获取监听端口
		 * @return 端口号
		 */
		[[nodiscard]] uint16_t port() const;

		/**
		 * @brief 获取底层 io_context 引用
		 * 用于创建需要 io_context 的外部组件（如数据库连接池）。
		 * @warning 不要在 start() 之后手动调用 ioCtx.run()
		 * @return io_context 引用
		 */
		[[nodiscard]] boost::asio::io_context& ioContext();

	private:
		// 协程式连接监听（每个 acceptor 独立运行）
		Awaitable<void> acceptLoop(boost::asio::ip::tcp::acceptor& acceptor, IdleFd& idleFd);

		// 协程式 HTTP 会话处理
		Awaitable<void> handleSession(boost::asio::ip::tcp::socket socket);

		// 协程式 WebSocket 会话处理（headers 已从 readBuf 拷贝为 owned，调用前已 release readBuf）
		Awaitable<void> handleWebSocket(boost::asio::ip::tcp::socket socket,
										std::string wsKey,
										std::string wsExtensions,
										std::string wsProtocol,
										const Router::WsRoute& wsRoute);

		// 协程式 SSE 会话处理（接管 socket，发送响应头后持续推送）
		Awaitable<void> handleSseSession(boost::asio::ip::tcp::socket socket, const Router::SseRoute& sseRoute);

		// 内存池定期 GC 协程
		Awaitable<void> gcLoop();

		// 优雅关机：停止接受新连接，等待活跃连接处理完毕
		void gracefulStop();

		// 停止所有 loop（baseLoop + ioPool）
		void stopAllLoops();

		// 关闭所有 acceptor（stop/gracefulStop 共用）
		void closeAllAcceptors();

		// 这俩 atomic 必须在 io_context 之前声明（析构顺序），协程帧里会引用它们
		std::atomic<size_t> activeConnections_ {0};
		std::atomic<bool> draining_ {false};

		std::atomic<uint16_t> port_;
		size_t ioThreads_;
		AsioEventLoop baseLoop_;                // 主 loop（accept + signal + GC）
		std::unique_ptr<EventLoopPool> ioPool_; // IO 线程池（ioThreads-1 个 worker loop）

		// idleScanners_ 在 baseLoop_/ioPool_ 之后声明 → 先析构。
		// 让 ~IdleScanner 的 timer 在 io_context 还活着时自然销毁。
		// 原注释担心 Guard::~Guard 回调 scanner->unregisterEntry() 时 scanner 已死
		// 的问题，在当前 releaseWork 路径下不成立——stop() 的 closeAll 关闭所有 socket，
		// 协程在 baseLoop_.run() 返回前已退出，Guard 析构在 scanner 析构前已完成。
		std::vector<std::unique_ptr<IdleScanner>> idleScanners_;

		// SO_REUSEPORT 多 acceptor，不支持时回退单 acceptor
		std::vector<std::unique_ptr<boost::asio::ip::tcp::acceptor>> acceptors_;
		std::vector<std::unique_ptr<IdleFd>> idleFds_;
		bool reusePortEnabled_ {false};

		std::atomic<bool> running_ {false};

		Router router_;
		MiddlewarePipeline middlewarePipeline_;

		std::shared_ptr<SslContext> sslCtx_;

		// start() 后配置不可改
		std::atomic<bool> started_ {false};

		// WebSocket 升级专用的预构建中间件链（finalHandler 返回 200 占位）
		MiddlewareNext wsMiddlewareChain_;

		// 请求大小限制
		size_t maxBodySize_ {1024 * 1024}; // 1MB
		size_t maxHeaderSize_ {8192};      // 8KB

		// 连接数限制（0 表示不限制，默认 10000）
		// atomic：accept 协程在多个 acceptor 线程并发读它，运行时也允许动态调整
		std::atomic<size_t> maxConnections_ {10000};

		// 空闲连接超时（秒，0 表示不超时）
		double idleTimeout_ {60.0};

		// fd 耗尽处理：已移至 idleFds_（每个 acceptor 配独立 IdleFd）

		// 内存池 GC 间隔（秒，0 表示关闭自动 GC）
		double gcInterval_ {60.0};

		// 优雅关机
		double shutdownTimeout_ {30.0};

		// 全局错误处理器
		ErrorHandler errorHandler_;

		// gcLoop 的 timer，放成员上是为了 stop 时能从外面 cancel 掉
		std::optional<boost::asio::steady_timer> gcTimer_;

		// signal_set 放成员上——stop() 得能 cancel 它，不然 async_wait 会卡住 run() 不退
		std::optional<boost::asio::signal_set> signals_;
	};

} // namespace hical
