#include "TcpServer.h"
#include "GenericConnection.h"

namespace hical
{

TcpServer::TcpServer(AsioEventLoop* baseLoop,
                     const InetAddress& listenAddr,
                     const std::string& name)
    : baseLoop_(baseLoop),
      listenAddr_(listenAddr),
      name_(name),
      acceptor_(baseLoop->getIoContext())
{
}

TcpServer::~TcpServer()
{
    if (running_.load())
    {
        stop();
    }
}

void TcpServer::setIoLoopNum(size_t num)
{
    ioLoopNum_ = num;
}

void TcpServer::start()
{
    if (running_.exchange(true))
    {
        return;
    }

    // 创建并启动 IO 线程池
    if (ioLoopNum_ > 0)
    {
        ioPool_ = std::make_unique<EventLoopPool>(ioLoopNum_);
        ioPool_->start();
    }

    // 打开 acceptor
    using boost::asio::ip::tcp;
    auto endpoint = tcp::endpoint(
        listenAddr_.isIpV6() ? tcp::v6() : tcp::v4(),
        listenAddr_.port());

    acceptor_.open(endpoint.protocol());
    acceptor_.set_option(boost::asio::socket_base::reuse_address(true));
    acceptor_.bind(endpoint);
    acceptor_.listen();

    // 获取实际监听端口（端口 0 时由系统分配）
    auto actualEndpoint = acceptor_.local_endpoint();
    listenAddr_ = InetAddress(
        actualEndpoint.address().to_string(),
        actualEndpoint.port());

    // 启动协程式 accept 循环
    boost::asio::co_spawn(
        baseLoop_->getIoContext(),
        [this]() -> Awaitable<void> {
            co_await acceptLoop();
        },
        boost::asio::detached);
}

void TcpServer::stop()
{
    if (!running_.exchange(false))
    {
        return;
    }

    // 关闭 acceptor
    boost::system::error_code ec;
    acceptor_.close(ec);

    // 关闭所有连接
    {
        std::lock_guard<std::mutex> lock(connectionsMutex_);
        for (auto& conn : connections_)
        {
            conn->close();
        }
        connections_.clear();
    }

    // 停止 IO 线程池
    if (ioPool_)
    {
        ioPool_->stop();
    }
}

void TcpServer::onNewConnection(NewConnectionCallback cb)
{
    newConnectionCallback_ = std::move(cb);
}

void TcpServer::onMessage(TcpConnection::MessageCallback cb)
{
    messageCallback_ = std::move(cb);
}

void TcpServer::onClose(TcpConnection::CloseCallback cb)
{
    closeCallback_ = std::move(cb);
}

void TcpServer::enableSsl(std::shared_ptr<SslContext> ctx)
{
    sslCtx_ = std::move(ctx);
}

const std::string& TcpServer::name() const
{
    return name_;
}

const InetAddress& TcpServer::listenAddr() const
{
    return listenAddr_;
}

size_t TcpServer::connectionCount() const
{
    std::lock_guard<std::mutex> lock(connectionsMutex_);
    return connections_.size();
}

bool TcpServer::isRunning() const
{
    return running_.load();
}

AsioEventLoop* TcpServer::getNextIoLoop()
{
    if (ioPool_ && ioPool_->size() > 0)
    {
        return ioPool_->getNextLoop();
    }
    return baseLoop_;
}

void TcpServer::addConnection(const TcpConnection::Ptr& conn)
{
    std::lock_guard<std::mutex> lock(connectionsMutex_);
    connections_.insert(conn);
}

void TcpServer::removeConnection(const TcpConnection::Ptr& conn)
{
    std::lock_guard<std::mutex> lock(connectionsMutex_);
    connections_.erase(conn);
}

Awaitable<void> TcpServer::acceptLoop()
{
    using boost::asio::ip::tcp;

    while (running_.load())
    {
        try
        {
            tcp::socket socket = co_await acceptor_.async_accept(
                boost::asio::use_awaitable);

            // 获取地址信息
            auto remoteEp = socket.remote_endpoint();
            auto localEp = socket.local_endpoint();

            InetAddress peerAddr(remoteEp.address().to_string(),
                                remoteEp.port());
            InetAddress localAddr(localEp.address().to_string(),
                                 localEp.port());

            // 获取目标 IO 循环
            auto* ioLoop = getNextIoLoop();

            // 创建连接
            TcpConnection::Ptr conn;

            if (sslCtx_)
            {
                boost::asio::ssl::stream<tcp::socket> sslStream(
                    std::move(socket), sslCtx_->native());
                conn = std::make_shared<SslConnection>(
                    ioLoop, std::move(sslStream), localAddr, peerAddr);
            }
            else
            {
                conn = std::make_shared<PlainConnection>(
                    ioLoop, std::move(socket), localAddr, peerAddr);
            }

            // 设置回调
            if (messageCallback_)
            {
                conn->onMessage(messageCallback_);
            }

            // 设置关闭回调（在原始回调之上添加连接移除逻辑）
            auto weakThis = this;
            conn->onClose([weakThis, userCb = closeCallback_](
                              const TcpConnection::Ptr& c) {
                if (userCb)
                {
                    userCb(c);
                }
                weakThis->removeConnection(c);
            });

            addConnection(conn);

            // 通知新连接
            if (newConnectionCallback_)
            {
                newConnectionCallback_(conn);
            }

            // 建立连接（SSL 会触发握手）
            if (auto* plain =
                    dynamic_cast<PlainConnection*>(conn.get()))
            {
                plain->connectEstablished();
            }
            else if (auto* ssl =
                         dynamic_cast<SslConnection*>(conn.get()))
            {
                ssl->connectEstablished();
            }
        }
        catch (const boost::system::system_error& e)
        {
            if (e.code() == boost::asio::error::operation_aborted)
            {
                break;  // acceptor 被关闭
            }
        }
    }
}

}  // namespace hical
