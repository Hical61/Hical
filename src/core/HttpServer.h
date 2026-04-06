#pragma once

#include "Router.h"
#include "Middleware.h"
#include "HttpRequest.h"
#include "HttpResponse.h"
#include "Coroutine.h"
#include "SslContext.h"
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
 *
 * 高层封装：整合 TcpServer + Router + 中间件管道。
 * 提供简洁的 API 配置路由、中间件，一键启动。
 *
 * 用法：
 * ```cpp
 * hical::HttpServer server(8080);
 * server.router().get("/", handler);
 * server.use(logMiddleware);
 * server.start();  // 阻塞
 * ```
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
     * @brief 启用 SSL/TLS
     * @param certFile 证书文件路径
     * @param keyFile 私钥文件路径
     */
    void enableSsl(const std::string& certFile, const std::string& keyFile);

    /**
     * @brief 启动服务器（阻塞）
     *
     * 调用后阻塞当前线程，直到 stop() 被调用。
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

  private:
    // 协程式连接监听
    Awaitable<void> acceptLoop(boost::asio::ip::tcp::acceptor acceptor);

    // 协程式 HTTP 会话处理
    Awaitable<void> handleSession(boost::asio::ip::tcp::socket socket);

    // 协程式 WebSocket 会话处理
    Awaitable<void> handleWebSocket(
        boost::asio::ip::tcp::socket socket,
        boost::beast::http::request<boost::beast::http::string_body> req,
        const Router::WsRoute& wsRoute);

    uint16_t port_;
    size_t ioThreads_;
    boost::asio::io_context ioContext_;
    std::atomic<bool> running_{false};

    Router router_;
    MiddlewarePipeline middlewarePipeline_;

    std::shared_ptr<SslContext> sslCtx_;
};

}  // namespace hical
