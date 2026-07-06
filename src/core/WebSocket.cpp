/**
 * @file WebSocket.cpp
 * @brief WebSocket 会话实现
 */

#include "WebSocket.h"
#include "WsHandshake.h"
#include <cstring>

namespace hical
{

	// ============ WebSocketSession ============

	WebSocketSession::WebSocketSession(boost::asio::ip::tcp::socket socket,
									   size_t maxMessageSize,
									   WsCompressionConfig compression,
									   const WsDeflateNegotiation* deflateNeg)
		: socket_(std::move(socket)), compression_(compression), maxMessageSize_(maxMessageSize)
	{
		// 初始 8KB 读缓冲区
		readBuf_.resize(8192);

		// 写互斥 timer（初始为"就绪"状态，过期时间在过去 = 不会阻塞）
		writeReady_ = std::make_unique<boost::asio::steady_timer>(socket_.get_executor());
		writeReady_->expires_at(std::chrono::steady_clock::time_point::min());

		// 构造 permessage-deflate 上下文（仅在协商成功时）
		if (compression.enabled && deflateNeg != nullptr && deflateNeg->accepted)
		{
			WsDeflateContext::Config cfg;
			cfg.serverMaxWindowBits = deflateNeg->serverMaxWindowBits;
			cfg.clientMaxWindowBits = deflateNeg->clientMaxWindowBits;
			cfg.serverNoContextTakeover = deflateNeg->serverNoContextTakeover;
			cfg.clientNoContextTakeover = deflateNeg->clientNoContextTakeover;
			cfg.compLevel = 6;
			cfg.memLevel = 4;
			deflateCtx_ = std::make_unique<WsDeflateContext>(cfg);
		}
	}

	Awaitable<void> WebSocketSession::send(const std::string& msg)
	{
		if (deflateCtx_)
		{
			auto compressed = deflateCtx_->compress(msg);
			co_await sendFrame(WsOpcode::hText, compressed, true, true);
		}
		else
		{
			co_await sendFrame(WsOpcode::hText, msg);
		}
	}

	Awaitable<void> WebSocketSession::sendBinary(std::string_view data)
	{
		if (deflateCtx_)
		{
			auto compressed = deflateCtx_->compress(data);
			co_await sendFrame(WsOpcode::hBinary, compressed, true, true);
		}
		else
		{
			co_await sendFrame(WsOpcode::hBinary, data);
		}
	}

	Awaitable<void> WebSocketSession::sendPing(std::string_view payload)
	{
		// RFC 6455 §5.5: 控制帧载荷不得超过 125 字节
		if (payload.size() > 125)
		{
			payload = payload.substr(0, 125);
		}
		co_await sendFrame(WsOpcode::hPing, payload);
	}

	Awaitable<std::optional<std::string>> WebSocketSession::receive()
	{
		auto msg = co_await receiveInternal();
		if (!msg)
		{
			co_return std::nullopt;
		}
		co_return std::move(msg->data);
	}

	Awaitable<void> WebSocketSession::closeAsync()
	{
		bool expected = true;
		if (open_.compare_exchange_strong(expected, false))
		{
			try
			{
				co_await sendCloseFrame(WsCloseCode::hNormal);
			}
			catch (...)
			{
				// 忽略发送 close 帧的错误（对端可能已关闭）
			}

			boost::system::error_code ec;
			socket_.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
			socket_.close(ec);
		}
	}

	Awaitable<void> WebSocketSession::closeAsync(WsCloseCode code, std::string_view reason)
	{
		bool expected = true;
		if (open_.compare_exchange_strong(expected, false))
		{
			try
			{
				co_await sendCloseFrame(code, reason);
			}
			catch (...)
			{
				// 忽略发送 close 帧的错误（对端可能已关闭）
			}

			boost::system::error_code ec;
			socket_.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
			socket_.close(ec);
		}
	}

	void WebSocketSession::close()
	{
		bool expected = true;
		if (open_.compare_exchange_strong(expected, false))
		{
			boost::system::error_code ec;
			socket_.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
			socket_.close(ec);
		}
	}

	bool WebSocketSession::isOpen() const
	{
		return open_.load() && socket_.is_open();
	}

	boost::asio::ip::tcp::socket& WebSocketSession::socket()
	{
		return socket_;
	}

	const WsCompressionConfig& WebSocketSession::compressionConfig() const
	{
		return compression_;
	}

	Awaitable<void> WebSocketSession::ensureBytes(size_t n)
	{
		while (readBufUsed_ < n)
		{
			// 按需扩容
			if (readBuf_.size() < n)
			{
				readBuf_.resize(std::max(n, readBuf_.size() * 2));
			}

			auto bytesRead = co_await socket_.async_read_some(
				boost::asio::buffer(readBuf_.data() + readBufUsed_, readBuf_.size() - readBufUsed_),
				boost::asio::use_awaitable);
			readBufUsed_ += bytesRead;
		}
	}

	void WebSocketSession::consumeBytes(size_t n)
	{
		if (n >= readBufUsed_)
		{
			readBufUsed_ = 0;
		}
		else
		{
			std::memmove(readBuf_.data(), readBuf_.data() + n, readBufUsed_ - n);
			readBufUsed_ -= n;
		}
	}

	Awaitable<void> WebSocketSession::acquireWrite()
	{
		while (writePending_)
		{
			// 等待前一个写完成（timer 过期 = 就绪信号）
			boost::system::error_code ec;
			co_await writeReady_->async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, ec));
			// ec 可能是 operation_aborted（被 releaseWrite cancel），这正是我们要的唤醒信号
		}
		writePending_ = true;
	}

	void WebSocketSession::releaseWrite()
	{
		writePending_ = false;
		// 唤醒等待的协程：cancel 使 async_wait 返回 operation_aborted
		writeReady_->cancel_one();
	}

	Awaitable<void> WebSocketSession::sendFrame(WsOpcode opcode, std::string_view payload, bool fin, bool rsv1)
	{
		co_await acquireWrite();

		// RAII 写锁守卫：async_write 抛异常时也能释放写权限，防止写锁永久卡死
		struct WriteGuard
		{
			WebSocketSession& self;

			~WriteGuard()
			{
				self.releaseWrite();
			}
		} guard {*this};

		// 直接写到复用缓冲 frameBuf_，省掉 buildWsFrame() 的每次 std::string 堆分配
		const size_t payloadLen = payload.size();
		size_t headerSize = 2;
		if (payloadLen > 65535)
		{
			headerSize += 8;
		}
		else if (payloadLen > 125)
		{
			headerSize += 2;
		}

		frameBuf_.resize(headerSize + payloadLen);
		auto* out = reinterpret_cast<uint8_t*>(frameBuf_.data());

		// Byte 0: FIN + RSV1 + opcode
		out[0] = static_cast<uint8_t>((fin ? 0x80U : 0x00U) | (rsv1 ? 0x40U : 0x00U)
									  | (static_cast<uint8_t>(opcode) & 0x0FU));

		// Byte 1 + extended payload length（big-endian，MASK=0）
		if (payloadLen <= 125)
		{
			out[1] = static_cast<uint8_t>(payloadLen);
		}
		else if (payloadLen <= 65535)
		{
			out[1] = 126;
			const auto len16 = static_cast<uint16_t>(payloadLen);
			out[2] = static_cast<uint8_t>(len16 >> 8);
			out[3] = static_cast<uint8_t>(len16 & 0xFF);
		}
		else
		{
			out[1] = 127;
			out[2] = static_cast<uint8_t>(payloadLen >> 56);
			out[3] = static_cast<uint8_t>((payloadLen >> 48) & 0xFF);
			out[4] = static_cast<uint8_t>((payloadLen >> 40) & 0xFF);
			out[5] = static_cast<uint8_t>((payloadLen >> 32) & 0xFF);
			out[6] = static_cast<uint8_t>((payloadLen >> 24) & 0xFF);
			out[7] = static_cast<uint8_t>((payloadLen >> 16) & 0xFF);
			out[8] = static_cast<uint8_t>((payloadLen >> 8) & 0xFF);
			out[9] = static_cast<uint8_t>(payloadLen & 0xFF);
		}

		if (payloadLen > 0)
		{
			std::memcpy(out + headerSize, payload.data(), payloadLen);
		}

		co_await boost::asio::async_write(socket_, boost::asio::buffer(frameBuf_), boost::asio::use_awaitable);
	}

	Awaitable<void> WebSocketSession::sendCloseFrame(WsCloseCode code, std::string_view reason)
	{
		auto closePayload = buildClosePayload(code, reason);
		co_await sendFrame(WsOpcode::hClose, closePayload);
	}

	Awaitable<std::optional<WsMessage>> WebSocketSession::receiveInternal()
	{
		try
		{
			for (;;)
			{
				// 1. 确保至少 2 字节（帧头最小长度）
				co_await ensureBytes(2);

				// 2. 尝试解析帧头
				auto hdr = parseWsFrameHeader(readBuf_.data(), readBufUsed_);
				if (!hdr)
				{
					// 帧头不完整，多读一些
					co_await ensureBytes(readBufUsed_ + 1);
					continue;
				}

				// 3. 协议校验
				if (hdr->rsv2 || hdr->rsv3)
				{
					co_await sendCloseFrame(WsCloseCode::hProtocolError, "Unexpected RSV2/RSV3");
					open_ = false;
					co_return std::nullopt;
				}
				if (hdr->rsv1 && !deflateCtx_)
				{
					co_await sendCloseFrame(WsCloseCode::hProtocolError, "Unexpected RSV1 without deflate");
					open_ = false;
					co_return std::nullopt;
				}
				if (!hdr->masked)
				{
					co_await sendCloseFrame(WsCloseCode::hProtocolError, "Client frames must be masked");
					open_ = false;
					co_return std::nullopt;
				}
				bool isControl = (static_cast<uint8_t>(hdr->opcode) >= 0x08);
				if (isControl && hdr->payloadLength > 125)
				{
					co_await sendCloseFrame(WsCloseCode::hProtocolError, "Control frame payload too large");
					open_ = false;
					co_return std::nullopt;
				}
				if (hdr->payloadLength > maxMessageSize_)
				{
					co_await sendCloseFrame(WsCloseCode::hMessageTooBig);
					open_ = false;
					co_return std::nullopt;
				}

				// 4. 确保完整帧数据可用
				size_t frameSize = hdr->headerSize + static_cast<size_t>(hdr->payloadLength);
				co_await ensureBytes(frameSize);

				// 5. 解除 mask
				uint8_t* payloadPtr = readBuf_.data() + hdr->headerSize;
				size_t payloadLen = static_cast<size_t>(hdr->payloadLength);
				unmaskPayload(payloadPtr, payloadLen, hdr->maskKey);

				// 6. 控制帧处理（RFC 6455 §5.5：控制帧可穿插在数据帧分片之间）
				if (hdr->opcode == WsOpcode::hClose)
				{
					// 回复 close 帧（直接引用 readBuf 中的载荷，零拷贝）
					std::string_view closeSv(reinterpret_cast<const char*>(payloadPtr), payloadLen);
					co_await sendFrame(WsOpcode::hClose, (payloadLen >= 2) ? closeSv : std::string_view {});
					consumeBytes(frameSize);
					open_ = false;
					co_return std::nullopt;
				}
				if (hdr->opcode == WsOpcode::hPing)
				{
					// 回复 Pong，载荷与 Ping 相同
					std::string_view pongPayload(reinterpret_cast<const char*>(payloadPtr), payloadLen);
					co_await sendFrame(WsOpcode::hPong, pongPayload);
					consumeBytes(frameSize);
					continue;
				}
				if (hdr->opcode == WsOpcode::hPong)
				{
					// 记录 Pong 接收时间（心跳检测用）
					recordPongReceived();
					consumeBytes(frameSize);
					continue;
				}

				// 7. 数据帧处理（Text/Binary/Continuation）
				if (hdr->opcode == WsOpcode::hText || hdr->opcode == WsOpcode::hBinary)
				{
					// 新消息的首帧
					fragmentBuf_.clear();
					fragmentOpcode_ = hdr->opcode;
					fragmentCompressed_ = hdr->rsv1;
				}
				else if (hdr->opcode != WsOpcode::hContinuation)
				{
					// 未知 opcode
					co_await sendCloseFrame(WsCloseCode::hProtocolError, "Unknown opcode");
					open_ = false;
					co_return std::nullopt;
				}

				// 追加载荷到分片缓冲
				fragmentBuf_.append(reinterpret_cast<const char*>(payloadPtr), payloadLen);

				// 总大小检查
				if (fragmentBuf_.size() > maxMessageSize_)
				{
					co_await sendCloseFrame(WsCloseCode::hMessageTooBig);
					open_ = false;
					co_return std::nullopt;
				}

				consumeBytes(frameSize);

				if (hdr->fin)
				{
					// 消息完成：构造 WsMessage 返回
					WsMessage msg;
					msg.type = fragmentOpcode_;
					if (fragmentCompressed_ && deflateCtx_)
					{
						msg.data = deflateCtx_->decompress(fragmentBuf_, maxMessageSize_);
					}
					else
					{
						msg.data = std::move(fragmentBuf_);
					}
					co_return msg;
				}
				// FIN=0: 继续等待后续分片
			}
		}
		catch (const boost::system::system_error& e)
		{
			open_ = false;
			if (e.code() == boost::asio::error::eof || e.code() == boost::asio::error::connection_reset
				|| e.code() == boost::asio::error::operation_aborted)
			{
				co_return std::nullopt;
			}
			throw;
		}
	}

	Awaitable<std::optional<WsMessage>> WebSocketSession::receiveMessage()
	{
		co_return co_await receiveInternal();
	}

	// ============ Context ============

	void WebSocketSession::setContext(std::shared_ptr<void> ctx)
	{
		context_ = std::move(ctx);
	}

	bool WebSocketSession::hasContext() const
	{
		return context_ != nullptr;
	}

	void WebSocketSession::clearContext()
	{
		context_.reset();
	}

	// ============ Heartbeat ============

	void WebSocketSession::recordPongReceived()
	{
		lastPongTime_ = std::chrono::steady_clock::now();
	}

	std::chrono::steady_clock::time_point WebSocketSession::lastPongTime() const
	{
		return lastPongTime_;
	}

	// ============ Subprotocol ============

	std::string_view WebSocketSession::subprotocol() const
	{
		return subprotocol_;
	}

	void WebSocketSession::setSubprotocol(std::string proto)
	{
		subprotocol_ = std::move(proto);
	}

} // namespace hical
