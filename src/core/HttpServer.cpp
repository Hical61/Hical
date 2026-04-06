#include "HttpServer.h"
#include "MemoryPool.h"
#include "WebSocket.h"
#include <boost/beast/websocket.hpp>
#include <iostream>
#include <thread>
#include <vector>

namespace hical
{

namespace beast = boost::beast;
namespace http = beast::http;
namespace ws = beast::websocket;
using boost::asio::ip::tcp;

HttpServer::HttpServer(uint16_t port, size_t ioThreads)
    : port_(port),
      ioThreads_(ioThreads > 0 ? ioThreads : 1)
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
    return router_;
}

void HttpServer::use(MiddlewareHandler middleware)
{
    middlewarePipeline_.use(std::move(middleware));
}

void HttpServer::enableSsl(const std::string& certFile,
                           const std::string& keyFile)
{
    sslCtx_ = std::make_shared<SslContext>(
        boost::asio::ssl::context::tls_server);
    sslCtx_->loadCertificate(certFile);
    sslCtx_->loadPrivateKey(keyFile);
}

void HttpServer::start()
{
    running_.store(true);

    tcp::acceptor acceptor(
        ioContext_, tcp::endpoint(tcp::v4(), port_));

    coSpawn(ioContext_,
            acceptLoop(std::move(acceptor)));

    // 多线程运行 io_context
    std::vector<std::thread> threads;
    for (size_t i = 1; i < ioThreads_; ++i)
    {
        threads.emplace_back([this]() {
            ioContext_.run();
        });
    }

    // 主线程也参与运行（阻塞）
    ioContext_.run();

    // 等待工作线程结束
    for (auto& t : threads)
    {
        if (t.joinable())
        {
            t.join();
        }
    }

    running_.store(false);
}

void HttpServer::stop()
{
    running_.store(false);
    ioContext_.stop();
}

bool HttpServer::isRunning() const
{
    return running_.load();
}

uint16_t HttpServer::port() const
{
    return port_;
}

Awaitable<void> HttpServer::acceptLoop(tcp::acceptor acceptor)
{
    while (running_.load())
    {
        try
        {
            auto socket = co_await acceptor.async_accept(
                boost::asio::use_awaitable);

            boost::asio::co_spawn(
                ioContext_,
                handleSession(std::move(socket)),
                boost::asio::detached);
        }
        catch (const boost::system::system_error& e)
        {
            if (e.code() == boost::asio::error::operation_aborted)
            {
                break;
            }
        }
    }
}

Awaitable<void> HttpServer::handleSession(tcp::socket socket)
{
    try
    {
        // 使用请求级 pmr 单调池，整个连接生命周期内复用
        auto requestPool = MemoryPool::instance().createRequestPool();
        std::pmr::polymorphic_allocator<std::byte> alloc(requestPool.get());
        beast::basic_flat_buffer<std::pmr::polymorphic_allocator<std::byte>>
            buffer(alloc);

        for (;;)
        {
            // 读取 HTTP 请求
            HttpRequest::BeastRequest beastReq;
            co_await http::async_read(
                socket, buffer, beastReq,
                boost::asio::use_awaitable);

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
                    co_await handleWebSocket(
                        std::move(socket), std::move(beastReq), *wsRoute);
                    co_return;
                }
            }

            HttpRequest req(std::move(beastReq));

            // 通过中间件管道 + 路由器分发
            HttpResponse res;
            if (middlewarePipeline_.size() > 0)
            {
                res = co_await middlewarePipeline_.execute(
                    req,
                    [this](const HttpRequest& r) -> Awaitable<HttpResponse> {
                        // 这里需要非 const 引用，做一份拷贝
                        HttpRequest mutableReq = r;
                        co_return co_await router_.dispatch(mutableReq);
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
            co_await http::async_write(
                socket, nativeRes,
                boost::asio::use_awaitable);

            if (!nativeRes.keep_alive())
            {
                break;
            }
        }
    }
    catch (const beast::system_error& e)
    {
        if (e.code() != beast::errc::not_connected &&
            e.code() != boost::asio::error::eof)
        {
            // 忽略正常的连接关闭
        }
    }

    boost::system::error_code ec;
    socket.shutdown(tcp::socket::shutdown_send, ec);
}

Awaitable<void> HttpServer::handleWebSocket(
    tcp::socket socket,
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
            if (!session.isOpen())
            {
                break;
            }

            if (wsRoute.onMessage)
            {
                co_await wsRoute.onMessage(msg, session);
            }
        }
    }
    catch (const beast::system_error& e)
    {
        if (e.code() != ws::error::closed &&
            e.code() != boost::asio::error::eof)
        {
            // 忽略正常关闭
        }
    }
}

}  // namespace hical
