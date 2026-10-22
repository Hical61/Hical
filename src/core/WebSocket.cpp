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
				// 关 socket 前先把合批缓冲刷干净，攒着的响应不能跟着一起丢
				co_await flushBatch();
				batching_ = false; // close 帧直写，别等批末没人来刷
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
				// 同上：先落盘再发 close 帧
				co_await flushBatch();
				batching_ = false;
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
		// 走到这儿说明缓冲里的完整帧已经解完了，马上要挂起等对端。
		// 解帧期间攒下的 pong 回帧得先落地——不然对端光等着我们的回帧，
		// 我们又等着对端发下一条消息，两头对着干
		co_await flushBatch();

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
			++waiters_;
			boost::system::error_code ec;
			co_await writeReady_->async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, ec));
			--waiters_;
			// ec 可能是 operation_aborted（被 releaseWrite cancel），这正是我们要的唤醒信号
		}
		writePending_ = true;
	}

	void WebSocketSession::releaseWrite()
	{
		writePending_ = false;
		// 有人排队才需要主动唤醒：单连接串行发送时 writePending_ 基本恒假，
		// 没等待者还去 cancel_one 就是白锁一次 timer 队列
		if (waiters_ > 0)
		{
			writeReady_->cancel_one();
		}
	}

	void WebSocketSession::encodeFrame(std::string& dst, WsOpcode opcode, std::string_view payload, bool fin, bool rsv1)
	{
		// 追加编码，不覆盖 dst 里已有的内容（合批时一帧接一帧往里塞）
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

		const size_t base = dst.size();
		dst.resize(base + headerSize + payloadLen);
		auto* out = reinterpret_cast<uint8_t*>(dst.data()) + base;

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
	}

	Awaitable<void> WebSocketSession::sendFrame(WsOpcode opcode, std::string_view payload, bool fin, bool rsv1)
	{
		co_await acquireWrite();

		// RAII 写锁守卫：async_write 抛异常时也能释放写权限，防止写锁永久卡死。
		// 合批模式下同样要拿这把锁——出站缓冲是连接级共用的，不拿锁就可能和
		// 心跳 Ping 协程或者 WsHub 广播撞到一起。
		WriteLock guard {*this};

		if (batching_)
		{
			// 合批模式：只入队不出网，真正落盘交给 flushBatch()/endBatch()
			co_await queueFrameLocked(opcode, payload, fin, rsv1);
			co_return;
		}

		frameBuf_.clear();
		encodeFrame(frameBuf_, opcode, payload, fin, rsv1);
		++writeCount_;
		co_await boost::asio::async_write(socket_, boost::asio::buffer(frameBuf_), boost::asio::use_awaitable);
	}

	Awaitable<void> WebSocketSession::queueFrameLocked(WsOpcode opcode, std::string_view payload, bool fin, bool rsv1)
	{
		encodeFrame(writeBatch_, opcode, payload, fin, rsv1);
		if (writeBatch_.size() >= kWriteBatchBytes)
		{
			// 攒到上限就提前落一次，客户端狂灌时分不出无限内存
			co_await writeBatchLocked();
		}
	}

	// payload 是视图不是拷贝，而且本函数第一件事 co_await acquireWrite() 就是个真挂起点：
	// 这块内存必须活到本协程返回为止，调用方不能传一块别的协程会改写或复用的缓冲区。
	// 眼下唯一的调用方是解帧循环，传的是 readBuf_ 里的载荷；readBuf_ 只有读循环自己动，
	// 所以现在是安全的。以后往外加调用方时别随手踩这条。
	//
	// 还有个已知的例外：解帧阶段收到的 ping 走这里无条件进出站缓冲，而批窗口外的
	// send()（心跳 Ping、WsHub 广播）是直写 frameBuf_ 的。两路交错时广播帧会先到、
	// pong 后到。RFC 6455 §5.5.3 只要求 pong 原样带回 payload，没要求跟数据帧保序，
	// 所以这不算 bug，别当成乱序问题去「修」。
	Awaitable<void> WebSocketSession::queueFrame(WsOpcode opcode, std::string_view payload)
	{
		co_await acquireWrite();
		WriteLock guard {*this};
		co_await queueFrameLocked(opcode, payload, true, false);
	}

	Awaitable<void> WebSocketSession::writeBatchLocked()
	{
		if (writeBatch_.empty())
		{
			co_return;
		}

		++writeCount_;
		try
		{
			co_await boost::asio::async_write(socket_, boost::asio::buffer(writeBatch_), boost::asio::use_awaitable);
			writeBatch_.clear();
		}
		catch (...)
		{
			// 写到一半出错：缓冲里剩下的没地方送了，清掉；连接标记关闭，
			// 读循环下一轮自己就退出去，不用往上层抛
			writeBatch_.clear();
			open_ = false;
		}
	}

	Awaitable<void> WebSocketSession::flushBatch()
	{
		if (writeBatch_.empty())
		{
			co_return;
		}

		co_await acquireWrite();
		WriteLock guard {*this};
		co_await writeBatchLocked();
	}

	void WebSocketSession::beginBatch()
	{
		batching_ = true;
	}

	Awaitable<void> WebSocketSession::endBatch()
	{
		batching_ = false;

		// 批内响应先落地，close 回帧排在它们后面——对端得先收到数据再收到关闭
		co_await flushBatch();

		if (closeReplyPending_)
		{
			closeReplyPending_ = false;
			try
			{
				co_await sendFrame(WsOpcode::hClose, closeReplyBuf_);
			}
			catch (...)
			{
				// 多半是对端已经没了，close 回帧发不出去就算了
			}
			closeReplyBuf_.clear();
		}
	}

	Awaitable<void> WebSocketSession::replyClose(std::string_view payload)
	{
		// close 回帧不能插到批内响应前面，先记下来，endBatch() 里补发。
		// 单条 receive() 路径也会在解完一批后调 endBatch()，所以这里不用分情况
		closeReplyBuf_.assign(payload);
		closeReplyPending_ = true;
		co_return;
	}

	Awaitable<void> WebSocketSession::sendCloseFrame(WsCloseCode code, std::string_view reason)
	{
		auto closePayload = buildClosePayload(code, reason);
		co_await sendFrame(WsOpcode::hClose, closePayload);
	}

	Awaitable<bool> WebSocketSession::receiveBatch(std::vector<WsMessage>& out)
	{
		out.clear();

		// 这一批已经吃完几帧（控制帧也算），到上限就让出去
		size_t framesInBatch = 0;

		try
		{
			for (;;)
			{
				// 一帧完整处理完之后才判上限，绝不会把帧劈成两半。剩下的字节都还在
				// readBuf_ 里，下一轮循环接着解；连接照旧是活的，所以返回 true
				if (framesInBatch >= kMaxFramesPerBatch)
				{
					co_return true;
				}

				// 1. 帧头最小 2 字节。手里已经解出消息就先交出去，
				// 别为了半个帧头把整批消息扣在这儿
				if (readBufUsed_ < 2)
				{
					if (!out.empty())
					{
						co_return true;
					}
					co_await ensureBytes(2);
				}

				// 2. 尝试解析帧头
				auto hdr = parseWsFrameHeader(readBuf_.data(), readBufUsed_);
				if (!hdr)
				{
					// 帧头没收全（扩展长度 / mask key 还没到）
					if (!out.empty())
					{
						co_return true;
					}
					co_await ensureBytes(readBufUsed_ + 1);
					continue;
				}

				// 3. 协议校验
				if (hdr->rsv2 || hdr->rsv3)
				{
					auto closePayload = buildClosePayload(WsCloseCode::hProtocolError, "Unexpected RSV2/RSV3");
					co_await replyClose(closePayload);
					open_ = false;
					co_return false;
				}
				if (hdr->rsv1 && !deflateCtx_)
				{
					auto closePayload =
						buildClosePayload(WsCloseCode::hProtocolError, "Unexpected RSV1 without deflate");
					co_await replyClose(closePayload);
					open_ = false;
					co_return false;
				}
				if (!hdr->masked)
				{
					auto closePayload = buildClosePayload(WsCloseCode::hProtocolError, "Client frames must be masked");
					co_await replyClose(closePayload);
					open_ = false;
					co_return false;
				}
				bool isControl = (static_cast<uint8_t>(hdr->opcode) >= 0x08);
				if (isControl && hdr->payloadLength > 125)
				{
					auto closePayload =
						buildClosePayload(WsCloseCode::hProtocolError, "Control frame payload too large");
					co_await replyClose(closePayload);
					open_ = false;
					co_return false;
				}
				if (hdr->payloadLength > maxMessageSize_)
				{
					auto closePayload = buildClosePayload(WsCloseCode::hMessageTooBig);
					co_await replyClose(closePayload);
					open_ = false;
					co_return false;
				}

				// 4. 半帧：先交已有的消息，剩下的留在缓冲里等下一批。
				//    缓冲里只有这半帧、没别的东西可交，才去读 socket 补齐。
				const size_t frameSize = hdr->headerSize + static_cast<size_t>(hdr->payloadLength);
				if (readBufUsed_ < frameSize)
				{
					if (!out.empty())
					{
						co_return true;
					}
					co_await ensureBytes(frameSize);
					continue;
				}

				// 5. 解除 mask
				uint8_t* payloadPtr = readBuf_.data() + hdr->headerSize;
				const size_t payloadLen = static_cast<size_t>(hdr->payloadLength);
				unmaskPayload(payloadPtr, payloadLen, hdr->maskKey);

				// 6. 控制帧就地处理（RFC 6455 §5.5：控制帧可穿插在数据帧分片之间）
				if (hdr->opcode == WsOpcode::hClose)
				{
					// 回 close 帧（载荷引用 readBuf，replyClose 里立刻拷走）
					std::string_view closeSv(reinterpret_cast<const char*>(payloadPtr), payloadLen);
					co_await replyClose((payloadLen >= 2) ? closeSv : std::string_view {});
					consumeBytes(frameSize);
					open_ = false;
					co_return false;
				}
				if (hdr->opcode == WsOpcode::hPing)
				{
					// 回复 Pong，载荷与 Ping 相同。攒进出站缓冲而不是立刻写：
					// 一批 ping 进来合成一次写，碰到要挂起等数据时 ensureBytes 会先刷出去
					std::string_view pongPayload(reinterpret_cast<const char*>(payloadPtr), payloadLen);
					co_await queueFrame(WsOpcode::hPong, pongPayload);
					consumeBytes(frameSize);
					++framesInBatch; // 控制帧也是实打实吃掉了一帧，同样计入上限
					continue;
				}
				if (hdr->opcode == WsOpcode::hPong)
				{
					// 记录 Pong 接收时间（心跳检测用）
					recordPongReceived();
					consumeBytes(frameSize);
					++framesInBatch;
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
					auto closePayload = buildClosePayload(WsCloseCode::hProtocolError, "Unknown opcode");
					co_await replyClose(closePayload);
					open_ = false;
					co_return false;
				}

				// 追加载荷到分片缓冲
				fragmentBuf_.append(reinterpret_cast<const char*>(payloadPtr), payloadLen);

				// 总大小检查
				if (fragmentBuf_.size() > maxMessageSize_)
				{
					auto closePayload = buildClosePayload(WsCloseCode::hMessageTooBig);
					co_await replyClose(closePayload);
					open_ = false;
					co_return false;
				}

				consumeBytes(frameSize);
				++framesInBatch; // 整帧吃掉了（分片消息没拼齐也算），上限计数就在这儿

				if (hdr->fin)
				{
					// 消息完成，塞进结果里接着解下一帧——一批能解几条就解几条
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
					out.push_back(std::move(msg));
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
				co_return false;
			}
			throw;
		}
	}

	Awaitable<std::optional<WsMessage>> WebSocketSession::receiveInternal()
	{
		if (pendingPos_ >= pending_.size())
		{
			// 上一批解出来的剩帧交完了，连接也没了
			if (!open_.load())
			{
				co_return std::nullopt;
			}

			pending_.clear();
			pendingPos_ = 0;
			(void)co_await receiveBatch(pending_);
			// 单条 API 自己就是一批的边界：攒下的 pong、待补的 close 回帧在这刷掉。
			// 没活就别调——endBatch 是协程，调一次就得分一次协程帧，
			// 空判断写进函数体里也拦不住这次分配，只能放在调用点
			if (hasPendingBatchWork())
			{
				co_await endBatch();
			}
			if (pending_.empty())
			{
				co_return std::nullopt;
			}
		}

		WsMessage msg = std::move(pending_[pendingPos_++]);
		if (pendingPos_ >= pending_.size())
		{
			// 交完了就把容器清掉，别把上一批的字符串挂到下一批
			pending_.clear();
			pendingPos_ = 0;
		}
		co_return msg;
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

	size_t WebSocketSession::writeCount() const
	{
		return writeCount_;
	}

} // namespace hical
