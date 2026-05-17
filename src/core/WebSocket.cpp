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
		: m_socket(std::move(socket)), m_compression(compression), m_maxMessageSize(maxMessageSize)
	{
		// 初始 8KB 读缓冲区
		m_readBuf.resize(8192);

		// 写互斥 timer（初始为"就绪"状态，过期时间在过去 = 不会阻塞）
		m_writeReady = std::make_unique<boost::asio::steady_timer>(m_socket.get_executor());
		m_writeReady->expires_at(std::chrono::steady_clock::time_point::min());

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
			m_deflateCtx = std::make_unique<WsDeflateContext>(cfg);
		}
	}

	Awaitable<void> WebSocketSession::send(const std::string& msg)
	{
		if (m_deflateCtx)
		{
			auto compressed = m_deflateCtx->compress(msg);
			co_await sendFrame(WsOpcode::hText, compressed, true, true);
		}
		else
		{
			co_await sendFrame(WsOpcode::hText, msg);
		}
	}

	Awaitable<void> WebSocketSession::sendBinary(std::string_view data)
	{
		if (m_deflateCtx)
		{
			auto compressed = m_deflateCtx->compress(data);
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
		if (m_open.compare_exchange_strong(expected, false))
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
			m_socket.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
			m_socket.close(ec);
		}
	}

	Awaitable<void> WebSocketSession::closeAsync(WsCloseCode code, std::string_view reason)
	{
		bool expected = true;
		if (m_open.compare_exchange_strong(expected, false))
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
			m_socket.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
			m_socket.close(ec);
		}
	}

	void WebSocketSession::close()
	{
		bool expected = true;
		if (m_open.compare_exchange_strong(expected, false))
		{
			boost::system::error_code ec;
			m_socket.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
			m_socket.close(ec);
		}
	}

	bool WebSocketSession::isOpen() const
	{
		return m_open.load() && m_socket.is_open();
	}

	boost::asio::ip::tcp::socket& WebSocketSession::socket()
	{
		return m_socket;
	}

	const WsCompressionConfig& WebSocketSession::compressionConfig() const
	{
		return m_compression;
	}

	Awaitable<void> WebSocketSession::ensureBytes(size_t n)
	{
		while (m_readBufUsed < n)
		{
			// 按需扩容
			if (m_readBuf.size() < n)
			{
				m_readBuf.resize(std::max(n, m_readBuf.size() * 2));
			}

			auto bytesRead = co_await m_socket.async_read_some(
				boost::asio::buffer(m_readBuf.data() + m_readBufUsed, m_readBuf.size() - m_readBufUsed),
				boost::asio::use_awaitable);
			m_readBufUsed += bytesRead;
		}
	}

	void WebSocketSession::consumeBytes(size_t n)
	{
		if (n >= m_readBufUsed)
		{
			m_readBufUsed = 0;
		}
		else
		{
			std::memmove(m_readBuf.data(), m_readBuf.data() + n, m_readBufUsed - n);
			m_readBufUsed -= n;
		}
	}

	Awaitable<void> WebSocketSession::acquireWrite()
	{
		while (m_writePending)
		{
			// 等待前一个写完成（timer 过期 = 就绪信号）
			boost::system::error_code ec;
			co_await m_writeReady->async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, ec));
			// ec 可能是 operation_aborted（被 releaseWrite cancel），这正是我们要的唤醒信号
		}
		m_writePending = true;
	}

	void WebSocketSession::releaseWrite()
	{
		m_writePending = false;
		// 唤醒等待的协程：cancel 使 async_wait 返回 operation_aborted
		m_writeReady->cancel_one();
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

		auto frame = buildWsFrame(opcode, payload, fin, rsv1);
		co_await boost::asio::async_write(m_socket, boost::asio::buffer(frame), boost::asio::use_awaitable);
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
				auto hdr = parseWsFrameHeader(m_readBuf.data(), m_readBufUsed);
				if (!hdr)
				{
					// 帧头不完整，多读一些
					co_await ensureBytes(m_readBufUsed + 1);
					continue;
				}

				// 3. 协议校验
				if (hdr->rsv2 || hdr->rsv3)
				{
					co_await sendCloseFrame(WsCloseCode::hProtocolError, "Unexpected RSV2/RSV3");
					m_open = false;
					co_return std::nullopt;
				}
				if (hdr->rsv1 && !m_deflateCtx)
				{
					co_await sendCloseFrame(WsCloseCode::hProtocolError, "Unexpected RSV1 without deflate");
					m_open = false;
					co_return std::nullopt;
				}
				if (!hdr->masked)
				{
					co_await sendCloseFrame(WsCloseCode::hProtocolError, "Client frames must be masked");
					m_open = false;
					co_return std::nullopt;
				}
				bool isControl = (static_cast<uint8_t>(hdr->opcode) >= 0x08);
				if (isControl && hdr->payloadLength > 125)
				{
					co_await sendCloseFrame(WsCloseCode::hProtocolError, "Control frame payload too large");
					m_open = false;
					co_return std::nullopt;
				}
				if (hdr->payloadLength > m_maxMessageSize)
				{
					co_await sendCloseFrame(WsCloseCode::hMessageTooBig);
					m_open = false;
					co_return std::nullopt;
				}

				// 4. 确保完整帧数据可用
				size_t frameSize = hdr->headerSize + static_cast<size_t>(hdr->payloadLength);
				co_await ensureBytes(frameSize);

				// 5. 解除 mask
				uint8_t* payloadPtr = m_readBuf.data() + hdr->headerSize;
				size_t payloadLen = static_cast<size_t>(hdr->payloadLength);
				unmaskPayload(payloadPtr, payloadLen, hdr->maskKey);

				// 6. 控制帧处理（RFC 6455 §5.5：控制帧可穿插在数据帧分片之间）
				if (hdr->opcode == WsOpcode::hClose)
				{
					// 回复 close 帧（直接引用 readBuf 中的载荷，零拷贝）
					std::string_view closeSv(reinterpret_cast<const char*>(payloadPtr), payloadLen);
					co_await sendFrame(WsOpcode::hClose, (payloadLen >= 2) ? closeSv : std::string_view {});
					consumeBytes(frameSize);
					m_open = false;
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
					m_fragmentBuf.clear();
					m_fragmentOpcode = hdr->opcode;
					m_fragmentCompressed = hdr->rsv1;
				}
				else if (hdr->opcode != WsOpcode::hContinuation)
				{
					// 未知 opcode
					co_await sendCloseFrame(WsCloseCode::hProtocolError, "Unknown opcode");
					m_open = false;
					co_return std::nullopt;
				}

				// 追加载荷到分片缓冲
				m_fragmentBuf.append(reinterpret_cast<const char*>(payloadPtr), payloadLen);

				// 总大小检查
				if (m_fragmentBuf.size() > m_maxMessageSize)
				{
					co_await sendCloseFrame(WsCloseCode::hMessageTooBig);
					m_open = false;
					co_return std::nullopt;
				}

				consumeBytes(frameSize);

				if (hdr->fin)
				{
					// 消息完成：构造 WsMessage 返回
					WsMessage msg;
					msg.type = m_fragmentOpcode;
					if (m_fragmentCompressed && m_deflateCtx)
					{
						msg.data = m_deflateCtx->decompress(m_fragmentBuf, m_maxMessageSize);
					}
					else
					{
						msg.data = std::move(m_fragmentBuf);
					}
					co_return msg;
				}
				// FIN=0: 继续等待后续分片
			}
		}
		catch (const boost::system::system_error& e)
		{
			m_open = false;
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
		m_context = std::move(ctx);
	}

	bool WebSocketSession::hasContext() const
	{
		return m_context != nullptr;
	}

	void WebSocketSession::clearContext()
	{
		m_context.reset();
	}

	// ============ Heartbeat ============

	void WebSocketSession::recordPongReceived()
	{
		m_lastPongTime = std::chrono::steady_clock::now();
	}

	std::chrono::steady_clock::time_point WebSocketSession::lastPongTime() const
	{
		return m_lastPongTime;
	}

	// ============ Subprotocol ============

	std::string_view WebSocketSession::subprotocol() const
	{
		return m_subprotocol;
	}

	void WebSocketSession::setSubprotocol(std::string proto)
	{
		m_subprotocol = std::move(proto);
	}

} // namespace hical
