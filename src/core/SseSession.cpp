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

		// 直接在 frame 中构建 SSE wire + chunked 帧，避免中间 string 拷贝
		std::string frame;
		size_t estimate = event.data.size() + 64;
		frame.reserve(estimate + 24); // 多留 24 字节放 chunk-size+CRLF

		// 预留 chunk-size 位置（最大 16 字节十六进制 + 2 字节 \r\n）
		// 先把 SSE body 和尾部 \r\n 拼好，size 字段最后回填
		auto sizeFieldPos = frame.size();
		frame.append(18, '\0');
		// 记录 body 起始位置，用于后续计算正文长度
		auto bodyStart = frame.size();

		if (!event.event.empty())
		{
			frame.append("event: ");
			frame.append(event.event);
			frame += '\n';
		}

		if (!event.data.empty())
		{
			size_t pos = 0;
			while (pos < event.data.size())
			{
				auto newline = event.data.find('\n', pos);
				frame.append("data: ");
				if (newline == std::string_view::npos)
				{
					frame.append(event.data.substr(pos));
					pos = event.data.size();
				}
				else
				{
					frame.append(event.data.substr(pos, newline - pos));
					pos = newline + 1;
				}
				frame += '\n';
			}
		}
		else
		{
			frame.append("data:\n");
		}

		if (!event.id.empty())
		{
			frame.append("id: ");
			frame.append(event.id);
			frame += '\n';
		}

		if (!event.retry.empty())
		{
			frame.append("retry: ");
			frame.append(event.retry);
			frame += '\n';
		}

		frame += '\n'; // 事件终结空行

		// 尾部 \r\n（chunked frame trailing CRLF）
		frame.append("\r\n");

		// SSE 正文 = bodyStart 到 \r\n 之前
		size_t sseLen = frame.size() - bodyStart - 2;

		char chunkSizeBuf[20];
		auto [ptr, ec] = std::to_chars(chunkSizeBuf, chunkSizeBuf + sizeof(chunkSizeBuf), sseLen, 16);
		auto sizeLen = static_cast<size_t>(ptr - chunkSizeBuf);

		// 用 chunk-size + \r\n 覆盖占位
		frame.replace(sizeFieldPos, 18, chunkSizeBuf, sizeLen);
		frame[sizeFieldPos + sizeLen] = '\r';
		frame[sizeFieldPos + sizeLen + 1] = '\n';

		// 移除占位中未使用的部分（sizeLen+2 之后到 18 的字节）
		frame.erase(sizeFieldPos + sizeLen + 2, 18 - sizeLen - 2);

		co_await boost::asio::async_write(socket_, boost::asio::buffer(frame), boost::asio::use_awaitable);
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

		// : comment\n\n → chunked frame
		// 用 FixedBuffer 栈上构建（注释通常很短）
		FixedBuffer<1024> commentBuf;
		commentBuf << ": " << comment << "\n\n";

		char chunkSizeBuf[20];
		auto [ptr, ec] = std::to_chars(chunkSizeBuf, chunkSizeBuf + sizeof(chunkSizeBuf), commentBuf.size(), 16);
		auto sizeLen = static_cast<size_t>(ptr - chunkSizeBuf);

		// 拼完整帧
		std::string frame;
		frame.reserve(sizeLen + 2 + commentBuf.size() + 2);
		frame.append(chunkSizeBuf, sizeLen);
		frame.append("\r\n");
		frame.append(commentBuf.data(), commentBuf.size());
		frame.append("\r\n");

		co_await boost::asio::async_write(socket_, boost::asio::buffer(frame), boost::asio::use_awaitable);
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
