#pragma once

#include "../core/TcpConnection.h"
#include "../core/InetAddress.h"
#include "../core/Coroutine.h"
#include "../core/SslContext.h"
#include "AsioEventLoop.h"
#include "EventLoopPool.h"
#include <boost/asio.hpp>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>

namespace hical
{

/**
 * @brief TCP 服务器
 *
 * 管理连接的监听、接受和分发。
 * 支持多线程 IO（通过 EventLoopPool）。
 * 支持 SSL/TLS 加密连接。
 * 采用协程式 accept 循环。
 */
class TcpServer
{
  public:
    using NewConnectionCallback =
        std::function<void(const TcpConnection::Ptr&)>;

    /**
     * @brief 构造 TCP 服务器
     * @param baseLoop 主事件循环（用于 accept）
     * @param listenAddr 监听地址
     * @param name 服务器名称
     */
    TcpServer(AsioEventLoop* baseLoop,
              const InetAddress& listenAddr,
              const std::string& name);

    ~TcpServer();

    /**
     * @brief 设置 IO 线程数
     * @param num 线程数（0 表示使用 baseLoop 处理 IO）
     * @note 必须在 start() 之前调用
     */
    void setIoLoopNum(size_t num);

    /**
     * @brief 启动服务器
     */
    void start();

    /**
     * @brief 停止服务器（优雅关闭）
     */
    void stop();

    /**
     * @brief 设置新连接回调
     * @param cb 当新连接建立时调用
     */
    void onNewConnection(NewConnectionCallback cb);

    /**
     * @brief 设置消息接收回调
     * @param cb 当收到数据时调用
     */
    void onMessage(TcpConnection::MessageCallback cb);

    /**
     * @brief 设置连接关闭回调
     * @param cb 当连接关闭时调用
     */
    void onClose(TcpConnection::CloseCallback cb);

    /**
     * @brief 启用 SSL/TLS
     * @param ctx SSL 上下文
     */
    void enableSsl(std::shared_ptr<SslContext> ctx);

    /**
     * @brief 获取服务器名称
     * @return 名称
     */
    const std::string& name() const;

    /**
     * @brief 获取监听地址
     * @return 地址
     */
    const InetAddress& listenAddr() const;

    /**
     * @brief 获取当前连接数
     * @return 连接数
     */
    size_t connectionCount() const;

    /**
     * @brief 服务器是否已启动
     * @return true 如果已启动
     */
    bool isRunning() const;

    // 禁止拷贝
    TcpServer(const TcpServer&) = delete;
    TcpServer& operator=(const TcpServer&) = delete;

  private:
    // 协程式 accept 循环
    Awaitable<void> acceptLoop();

    // 获取下一个 IO 事件循环
    AsioEventLoop* getNextIoLoop();

    // 连接管理
    void addConnection(const TcpConnection::Ptr& conn);
    void removeConnection(const TcpConnection::Ptr& conn);

    AsioEventLoop* baseLoop_;
    InetAddress listenAddr_;
    std::string name_;

    boost::asio::ip::tcp::acceptor acceptor_;
    std::atomic<bool> running_{false};

    // IO 线程池
    size_t ioLoopNum_{0};
    std::unique_ptr<EventLoopPool> ioPool_;

    // 连接集合
    std::set<TcpConnection::Ptr> connections_;
    mutable std::mutex connectionsMutex_;

    // 回调
    NewConnectionCallback newConnectionCallback_;
    TcpConnection::MessageCallback messageCallback_;
    TcpConnection::CloseCallback closeCallback_;

    // SSL
    std::shared_ptr<SslContext> sslCtx_;
};

}  // namespace hical
