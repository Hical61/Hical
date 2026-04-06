#include <boost/asio.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <iostream>
#include <memory>

using boost::asio::ip::tcp;
using boost::asio::awaitable;
using boost::asio::co_spawn;
using boost::asio::detached;
using boost::asio::use_awaitable;

/**
 * @brief 协程式会话处理
 *
 * 使用 co_await 进行异步读写，替代旧版回调式实现。
 */
awaitable<void> handleSession(tcp::socket socket)
{
    try
    {
        char data[1024];
        for (;;)
        {
            // 协程式异步读取
            auto bytesRead = co_await socket.async_read_some(
                boost::asio::buffer(data), use_awaitable);

            // 协程式异步写入（Echo 回写）
            co_await boost::asio::async_write(
                socket,
                boost::asio::buffer(data, bytesRead),
                use_awaitable);
        }
    }
    catch (const std::exception&)
    {
        // 连接关闭或错误，会话结束
    }
}

/**
 * @brief 协程式连接接受器
 */
awaitable<void> listener(tcp::acceptor acceptor)
{
    for (;;)
    {
        // 协程式异步接受连接
        auto socket = co_await acceptor.async_accept(use_awaitable);

        // 为每个连接启动一个独立的协程
        co_spawn(
            acceptor.get_executor(),
            handleSession(std::move(socket)),
            detached);
    }
}

int main(int argc, char* argv[])
{
    try
    {
        if (argc != 2)
        {
            std::cerr << "用法: echo_server <端口>\n";
            return 1;
        }

        boost::asio::io_context ioContext;

        auto port = static_cast<unsigned short>(std::atoi(argv[1]));
        tcp::acceptor acceptor(ioContext, tcp::endpoint(tcp::v4(), port));

        // 启动监听协程
        co_spawn(ioContext, listener(std::move(acceptor)), detached);

        std::cout << "Echo Server（协程式）启动在端口 " << argv[1] << std::endl;
        ioContext.run();
    }
    catch (const std::exception& e)
    {
        std::cerr << "异常: " << e.what() << std::endl;
    }

    return 0;
}
