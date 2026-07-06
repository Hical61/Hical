/**
 * @file WebSocket.h
 * @brief WebSocket 会话管理与消息收发
 */

#pragma once

#include "Coroutine.h"
#include "WsDeflate.h"
#include "WsFrame.h"
#include <boost/asio.hpp>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace hical
{

	/**
	 * @brief WebSocket 压缩配置
	 * 由 WsRoute 传入，用于配置 permessage-deflate。
	 */
	struct WsCompressionConfig
	{
		bool enabled = false;
		int serverMaxWindowBits = 15;
		int clientMaxWindowBits = 15;
		bool serverNoContextTakeover = false;
	};

	struct WsDeflateNegotiation; // 前向声明，定义在 WsHandshake.h

	/**
	 * @brief WebSocket 消息（含类型信息）
	 * 用于区分 Text/Binary 消息类型。
	 */
	struct WsMessage
	{
		WsOpcode type = WsOpcode::hText; ///< hText 或 hBinary
		std::string data;
	};

	/**
	 * @brief WebSocket 会话封装
	 * 对原始 TCP socket 的 WebSocket 协议封装（RFC 6455）。
	 * 提供协程化的 send/receive 接口，自研帧解析/构造。
	 */
	class WebSocketSession
	{
	public:
		// 默认最大消息大小 1MB，防止恶意客户端发送超大帧导致 OOM
		static constexpr size_t hDefaultMaxMessageSize = 1024 * 1024;

		/**
		 * @brief 从已完成 WebSocket 握手的 TCP socket 构造
		 * @param socket      已完成握手的 TCP socket
		 * @param maxMessageSize 最大消息大小（字节，默认 1MB）
		 * @param compression 压缩配置
		 * @param deflateNeg  permessage-deflate 协商结果（nullptr 时不启用压缩）
		 */
		explicit WebSocketSession(boost::asio::ip::tcp::socket socket,
								  size_t maxMessageSize = hDefaultMaxMessageSize,
								  WsCompressionConfig compression = {},
								  const WsDeflateNegotiation* deflateNeg = nullptr);

		/**
		 * @brief 发送文本消息
		 * @param msg 消息内容
		 */
		Awaitable<void> send(const std::string& msg);

		/**
		 * @brief 发送二进制消息（WsOpcode::hBinary）
		 * @param data 二进制数据
		 */
		Awaitable<void> sendBinary(std::string_view data);

		/**
		 * @brief 发送 Ping 帧
		 * @param payload Ping 载荷（最大 125 字节，默认空）
		 */
		Awaitable<void> sendPing(std::string_view payload = {});

		/**
		 * @brief 接收消息（仅返回文本内容，向后兼容）
		 * @return 接收到的消息内容，连接关闭时返回 std::nullopt
		 */
		Awaitable<std::optional<std::string>> receive();

		/**
		 * @brief 接收带类型的消息（区分 Text/Binary）
		 * @return WsMessage 含 type 和 data，连接关闭时返回 std::nullopt
		 */
		Awaitable<std::optional<WsMessage>> receiveMessage();

		/**
		 * @brief 协程式关闭 WebSocket 连接（默认 Normal Closure）
		 */
		Awaitable<void> closeAsync();

		/**
		 * @brief 协程式关闭 WebSocket 连接（自定义关闭码）
		 * @param code 关闭码（RFC 6455 §7.4.1）
		 * @param reason 关闭原因（最大 123 字节）
		 */
		Awaitable<void> closeAsync(WsCloseCode code, std::string_view reason = {});

		/**
		 * @brief 同步关闭 WebSocket 连接
		 * @warning 不发送 close 帧，直接关闭底层 socket。
		 * 推荐使用 closeAsync() 替代。
		 */
		void close();

		/**
		 * @brief 连接是否仍然打开
		 * @return true 如果连接打开
		 */
		[[nodiscard]] bool isOpen() const;

		/**
		 * @brief 获取底层 TCP socket 引用
		 * 用于超时场景关闭底层 socket 以中断 async_read。
		 * @return TCP socket 引用
		 */
		[[nodiscard]] boost::asio::ip::tcp::socket& socket();

		/**
		 * @brief 获取压缩配置
		 */
		[[nodiscard]] const WsCompressionConfig& compressionConfig() const;

		// ============ 连接上下文 ============

		/**
		 * @brief 设置用户自定义上下文（类型擦除）
		 * @param ctx 任意类型的 shared_ptr
		 */
		void setContext(std::shared_ptr<void> ctx);

		/**
		 * @brief 获取用户自定义上下文
		 * @tparam T 原始类型
		 * @return static_pointer_cast 后的 shared_ptr
		 */
		template <typename T>
		[[nodiscard]] std::shared_ptr<T> getContext() const
		{
			return std::static_pointer_cast<T>(context_);
		}

		/// 检查是否已设置上下文
		[[nodiscard]] bool hasContext() const;

		/// 清除上下文
		void clearContext();

		// ============ 心跳支持 ============

		/// 获取最后收到 Pong 的时间点
		[[nodiscard]] std::chrono::steady_clock::time_point lastPongTime() const;

		// ============ 子协议 ============

		/// 获取协商的子协议（空 = 未协商）
		[[nodiscard]] std::string_view subprotocol() const;

		/// 设置子协议（框架内部使用）
		void setSubprotocol(std::string proto);

	private:
		/// 接收消息的内核实现（receive/receiveMessage 共用）
		Awaitable<std::optional<WsMessage>> receiveInternal();

		/// 记录收到 Pong（由 receiveInternal() 内部调用）
		void recordPongReceived();

		/// 确保读缓冲区中至少有 n 字节可用数据
		Awaitable<void> ensureBytes(size_t n);

		/// 丢弃读缓冲区前 n 字节（memmove 剩余数据）
		void consumeBytes(size_t n);

		/// 构造并发送 WebSocket 帧
		Awaitable<void> sendFrame(WsOpcode opcode, std::string_view payload, bool fin = true, bool rsv1 = false);

		/// 发送 close 帧的便捷方法
		Awaitable<void> sendCloseFrame(WsCloseCode code, std::string_view reason = {});

		boost::asio::ip::tcp::socket socket_;
		std::atomic<bool> open_ {true};
		WsCompressionConfig compression_;
		size_t maxMessageSize_;

		// 连接级读缓冲区（跨 receive() 调用复用）
		std::vector<uint8_t> readBuf_;
		size_t readBufUsed_ = 0;

		// 分片消息重组
		std::string fragmentBuf_;
		WsOpcode fragmentOpcode_ = WsOpcode::hText;
		bool fragmentCompressed_ = false; ///< 当前分片消息的首帧是否带 RSV1（压缩）

		// 帧发送复用缓冲，省掉每帧一次的 std::string 堆分配
		std::string frameBuf_;

		// permessage-deflate 上下文（仅压缩启用时构造）
		std::unique_ptr<WsDeflateContext> deflateCtx_;

		// 写串行化：防止心跳 Ping 与消息发送并发 async_write
		bool writePending_ = false;
		std::unique_ptr<boost::asio::steady_timer> writeReady_;

		/// 获取写权限（等待前一个 async_write 完成）
		Awaitable<void> acquireWrite();

		/// 释放写权限（唤醒等待的协程）
		void releaseWrite();

		// 连接上下文（Feature 6）
		std::shared_ptr<void> context_;

		// 心跳 Pong 时间戳（Feature 1）
		std::chrono::steady_clock::time_point lastPongTime_ {std::chrono::steady_clock::now()};

		// 协商的子协议（Feature 5）
		std::string subprotocol_;
	};

} // namespace hical
