#pragma once

#include "Router.h"
#include "Middleware.h"
#include "HttpRequest.h"
#include "HttpResponse.h"
#include "Coroutine.h"
#include "SslContext.h"
#include "IdleFd.h"
#include "../asio/AsioEventLoop.h"
#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <cstdint>
#include <memory>
#include <string>

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
	 * @note 线程安全
	 * 当 ioThreads > 1 时，多个 IO 线程共享同一个 io_context，
	 * 路由 handler 和中间件可能被并发调用。此时用户必须保证：
	 * - 路由 handler 内访问的共享数据需自行加锁或使用无锁结构
	 * - 如果不需要多线程并发，使用默认的 ioThreads=1（单线程模式）即可
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
		Router& router();

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
		std::vector<MiddlewarePipeline::TimingSnapshot> middlewareStats() const;
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
		 */
		void setMaxConnections(size_t maxConns);

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
		bool isRunning() const;

		/**
		 * @brief 获取监听端口
		 * @return 端口号
		 */
		uint16_t port() const;

		/**
		 * @brief 获取底层 io_context 引用
		 * 用于创建需要 io_context 的外部组件（如数据库连接池）。
		 * @warning 不要在 start() 之后手动调用 ioCtx.run()
		 * @return io_context 引用
		 */
		boost::asio::io_context& ioContext();

	private:
		// 协程式连接监听
		Awaitable<void> acceptLoop();

		// 协程式 HTTP 会话处理
		Awaitable<void> handleSession(boost::asio::ip::tcp::socket socket);

		// 协程式 WebSocket 会话处理
		Awaitable<void> handleWebSocket(boost::asio::ip::tcp::socket socket,
										boost::beast::http::request<boost::beast::http::string_body> req,
										const Router::WsRoute& wsRoute);

		// 内存池定期 GC 协程
		Awaitable<void> gcLoop();

		// 优雅关机：停止接受新连接，等待活跃连接处理完毕
		void gracefulStop();

		std::atomic<uint16_t> port_;
		size_t ioThreads_;
		boost::asio::io_context ioContext_;
		std::unique_ptr<boost::asio::ip::tcp::acceptor> acceptor_;
		std::atomic<bool> running_ {false};

		Router router_;
		MiddlewarePipeline middlewarePipeline_;

		std::shared_ptr<SslContext> sslCtx_;

		// 启动后锁定配置，防止运行期修改路由/中间件导致数据竞争
		// 使用 atomic 确保多线程环境下可见性（start() 后 IO 线程需读取此标志）
		std::atomic<bool> started_ {false};

		// WebSocket 升级专用的预构建中间件链（finalHandler 返回 200 占位）
		MiddlewareNext wsMiddlewareChain_;

		// 请求大小限制
		size_t maxBodySize_ {1024 * 1024}; // 1MB
		size_t maxHeaderSize_ {8192};      // 8KB

		// 连接数限制（0 表示不限制）
		size_t maxConnections_ {10000};
		std::atomic<size_t> activeConnections_ {0};

		// 空闲连接超时（秒，0 表示不超时）
		double idleTimeout_ {60.0};

		// fd 耗尽处理：预留一个 fd 防止 accept 循环空转
		IdleFd idleFd_;

		// 内存池 GC 间隔（秒，0 表示关闭自动 GC）
		double gcInterval_ {60.0};

		// 优雅关机
		double shutdownTimeout_ {30.0};
		std::atomic<bool> draining_ {false};

		// 全局错误处理器
		ErrorHandler errorHandler_;
	};

} // namespace hical
