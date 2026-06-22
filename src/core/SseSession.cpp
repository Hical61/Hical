/**
 * @file SseSession.cpp
 * @brief Server-Sent Events (SSE) 会话实现
 */

#include "SseSession.h"
#include "FixedBuffer.h"
#include <charconv>

namespace hical
{

	SseSession::SseSession(boost::asio::ip::tcp::socket socket) : socket_(std::move(socket))
	{
	}

	SseSession::~SseSession()
	{
		alive_.store(false, std::memory_order_release);
		boost::system::error_code ec;
		socket_.close(ec);
	}

	Awaitable<void> SseSession::sendResponseHead()
	{
		FixedBuffer<512> headBuf;
		headBuf << "HTTP/1.1 200 OK\r\n"
				   "Content-Type: text/event-stream\r\n"
				   "Cache-Control: no-cache\r\n"
				   "Connection: keep-alive\r\n"
				   "X-Content-Type-Options: nosniff\r\n"
				   "Transfer-Encoding: chunked\r\n"
				   "\r\n";

		co_await boost::asio::async_write(socket_,
										  boost::asio::buffer(headBuf.data(), headBuf.size()),
										  boost::asio::use_awaitable);

		headSent_ = true;
	}

	Awaitable<void> SseSession::sendEvent(const SseEvent& event)
	{
		if (!alive_.load(std::memory_order_acquire))
		{
			co_return;
		}

		// 复用 sendBuf_ 避免每次堆分配
		sendBuf_.clear();
		size_t estimate = event.data.size() + 64;
		sendBuf_.reserve(estimate);

		// 构建 SSE wire body（不含 chunked 帧头，最后用 scatter-gather 发送）
		if (!event.event.empty())
		{
			sendBuf_ += "event: ";
			sendBuf_.append(event.event.data(), event.event.size());
			sendBuf_ += '\n';
		}

		if (!event.data.empty())
		{
			size_t pos = 0;
			while (pos < event.data.size())
			{
				auto newline = event.data.find('\n', pos);
				sendBuf_ += "data: ";
				if (newline == std::string_view::npos)
				{
					sendBuf_.append(event.data.data() + pos, event.data.size() - pos);
					pos = event.data.size();
				}
				else
				{
					sendBuf_.append(event.data.data() + pos, newline - pos);
					pos = newline + 1;
				}
				sendBuf_ += '\n';
			}
		}
		else
		{
			sendBuf_ += "data:\n";
		}

		if (!event.id.empty())
		{
			sendBuf_ += "id: ";
			sendBuf_.append(event.id.data(), event.id.size());
			sendBuf_ += '\n';
		}

		if (!event.retry.empty())
		{
			sendBuf_ += "retry: ";
			sendBuf_.append(event.retry.data(), event.retry.size());
			sendBuf_ += '\n';
		}

		sendBuf_ += '\n'; // SSE 事件终结空行

		// chunk-size 在栈上计算
		char chunkSizeBuf[20];
		auto [ptr, ec] = std::to_chars(chunkSizeBuf, chunkSizeBuf + sizeof(chunkSizeBuf), sendBuf_.size(), 16);
		auto sizeLen = static_cast<size_t>(ptr - chunkSizeBuf);

		// scatter-gather 零拷贝发送：chunk-size\r\n + body + \r\n
		static constexpr std::string_view kCrLf = "\r\n";
		static constexpr std::string_view kTermCrLf = "\r\n";
		std::array<boost::asio::const_buffer, 4> bufs = {{
			boost::asio::buffer(chunkSizeBuf, sizeLen),
			boost::asio::buffer(kCrLf.data(), kCrLf.size()),
			boost::asio::buffer(sendBuf_.data(), sendBuf_.size()),
			boost::asio::buffer(kTermCrLf.data(), kTermCrLf.size()),
		}};
		co_await boost::asio::async_write(socket_, bufs, boost::asio::use_awaitable);
	}

	Awaitable<void> SseSession::sendData(std::string_view data)
	{
		SseEvent evt;
		evt.data = data;
		co_await sendEvent(evt);
	}

	Awaitable<void> SseSession::sendComment(std::string_view comment)
	{
		if (!alive_.load(std::memory_order_acquire))
		{
			co_return;
		}

		// 复用 sendBuf_ 构建 : comment\n\n → chunked frame
		sendBuf_.clear();
		sendBuf_ += ": ";
		sendBuf_.append(comment.data(), comment.size());
		sendBuf_ += "\n\n";

		char chunkSizeBuf[20];
		auto [ptr, ec] = std::to_chars(chunkSizeBuf, chunkSizeBuf + sizeof(chunkSizeBuf), sendBuf_.size(), 16);
		auto sizeLen = static_cast<size_t>(ptr - chunkSizeBuf);

		// scatter-gather 零拷贝发送
		static constexpr std::string_view kCrLf = "\r\n";
		static constexpr std::string_view kTermCrLf = "\r\n";
		std::array<boost::asio::const_buffer, 4> bufs = {{
			boost::asio::buffer(chunkSizeBuf, sizeLen),
			boost::asio::buffer(kCrLf.data(), kCrLf.size()),
			boost::asio::buffer(sendBuf_.data(), sendBuf_.size()),
			boost::asio::buffer(kTermCrLf.data(), kTermCrLf.size()),
		}};
		co_await boost::asio::async_write(socket_, bufs, boost::asio::use_awaitable);
	}

	Awaitable<void> SseSession::close()
	{
		if (!alive_.load(std::memory_order_acquire))
		{
			co_return;
		}
		alive_.store(false, std::memory_order_release);

		// 发送 chunked 终止帧：0\r\n\r\n
		static constexpr std::string_view kTrailer = "0\r\n\r\n";
		boost::system::error_code ec;
		co_await boost::asio::async_write(socket_,
										  boost::asio::buffer(kTrailer.data(), kTrailer.size()),
										  boost::asio::redirect_error(boost::asio::use_awaitable, ec));

		// 关闭 socket
		boost::system::error_code closeEc;
		socket_.close(closeEc);
	}

} // namespace hical
