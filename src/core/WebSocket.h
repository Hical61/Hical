#pragma once

#include "Coroutine.h"
#include "WsDeflate.h"
#include "WsFrame.h"
#include <boost/asio.hpp>
#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
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
		 * @brief 接收消息
		 * @return 接收到的消息内容，连接关闭时返回 std::nullopt
		 */
		Awaitable<std::optional<std::string>> receive();

		/**
		 * @brief 协程式关闭 WebSocket 连接（推荐）
		 * 在协程上下文中安全关闭，发送 close 帧后关闭 TCP 连接。
		 */
		Awaitable<void> closeAsync();

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
		bool isOpen() const;

		/**
		 * @brief 获取底层 TCP socket 引用
		 * 用于超时场景关闭底层 socket 以中断 async_read。
		 * @return TCP socket 引用
		 */
		boost::asio::ip::tcp::socket& socket();

		/**
		 * @brief 获取压缩配置
		 */
		const WsCompressionConfig& compressionConfig() const;

	private:
		/// 确保读缓冲区中至少有 n 字节可用数据
		Awaitable<void> ensureBytes(size_t n);

		/// 丢弃读缓冲区前 n 字节（memmove 剩余数据）
		void consumeBytes(size_t n);

		/// 构造并发送 WebSocket 帧
		Awaitable<void> sendFrame(WsOpcode opcode, std::string_view payload, bool fin = true, bool rsv1 = false);

		/// 发送 close 帧的便捷方法
		Awaitable<void> sendCloseFrame(WsCloseCode code, std::string_view reason = {});

		boost::asio::ip::tcp::socket m_socket;
		std::atomic<bool> m_open {true};
		WsCompressionConfig m_compression;
		size_t m_maxMessageSize;

		// 连接级读缓冲区（跨 receive() 调用复用）
		std::vector<uint8_t> m_readBuf;
		size_t m_readBufUsed = 0;

		// 分片消息重组
		std::string m_fragmentBuf;
		WsOpcode m_fragmentOpcode = WsOpcode::hText;
		bool m_fragmentCompressed = false; ///< 当前分片消息的首帧是否带 RSV1（压缩）

		// permessage-deflate 上下文（仅压缩启用时构造）
		std::unique_ptr<WsDeflateContext> m_deflateCtx;
	};

} // namespace hical
