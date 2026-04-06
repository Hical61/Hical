#include "WebSocket.h"
#include <boost/beast/core/buffers_to_string.hpp>

namespace hical
{

namespace beast = boost::beast;

WebSocketSession::WebSocketSession(WsStream stream)
    : stream_(std::move(stream))
{
}

Awaitable<void> WebSocketSession::send(const std::string& msg)
{
    co_await stream_.async_write(
        boost::asio::buffer(msg),
        boost::asio::use_awaitable);
}

Awaitable<std::string> WebSocketSession::receive()
{
    beast::flat_buffer buffer;
    try
    {
        co_await stream_.async_read(buffer, boost::asio::use_awaitable);
        co_return beast::buffers_to_string(buffer.data());
    }
    catch (const beast::system_error& e)
    {
        open_ = false;
        if (e.code() == beast::websocket::error::closed ||
            e.code() == boost::asio::error::eof)
        {
            co_return "";
        }
        throw;
    }
}

void WebSocketSession::close()
{
    if (open_)
    {
        open_ = false;
        boost::system::error_code ec;
        stream_.close(beast::websocket::close_code::normal, ec);
    }
}

bool WebSocketSession::isOpen() const
{
    return open_ && stream_.is_open();
}

WebSocketSession::WsStream& WebSocketSession::native()
{
    return stream_;
}

}  // namespace hical
