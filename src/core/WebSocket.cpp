#include "WebSocket.h"
#include <boost/beast/core/buffers_to_string.hpp>

namespace hical
{

	namespace beast = boost::beast;

	// ============ WebSocketSession ============

	WebSocketSession::WebSocketSession(WsStream stream, size_t maxMessageSize, WsCompressionConfig compression)
		: stream_(std::move(stream)), compression_(compression)
	{
		stream_.read_message_max(maxMessageSize);
	}

	Awaitable<void> WebSocketSession::send(const std::string& msg)
	{
		co_await stream_.async_write(boost::asio::buffer(msg), boost::asio::use_awaitable);
	}

	Awaitable<std::optional<std::string>> WebSocketSession::receive()
	{
		try
		{
			readBuffer_.consume(readBuffer_.size());
			co_await stream_.async_read(readBuffer_, boost::asio::use_awaitable);
			co_return beast::buffers_to_string(readBuffer_.data());
		}
		catch (const beast::system_error& e)
		{
			open_ = false;
			if (e.code() == beast::websocket::error::closed || e.code() == boost::asio::error::eof)
			{
				co_return std::nullopt;
			}
			throw;
		}
	}

	Awaitable<void> WebSocketSession::closeAsync()
	{
		bool expected = true;
		if (open_.compare_exchange_strong(expected, false))
		{
			try
			{
				co_await stream_.async_close(beast::websocket::close_code::normal, boost::asio::use_awaitable);
			}
			catch (const beast::system_error&)
			{
				// 忽略关闭错误（对端可能已关闭）
			}
		}
	}

	void WebSocketSession::close()
	{
		bool expected = true;
		if (open_.compare_exchange_strong(expected, false))
		{
			boost::system::error_code ec;
			stream_.close(beast::websocket::close_code::normal, ec);
		}
	}

	bool WebSocketSession::isOpen() const
	{
		return open_.load() && stream_.is_open();
	}

	WebSocketSession::WsStream& WebSocketSession::native()
	{
		return stream_;
	}

	const WsCompressionConfig& WebSocketSession::compressionConfig() const
	{
		return compression_;
	}

} // namespace hical
