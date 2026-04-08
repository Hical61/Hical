#pragma once

#include "Coroutine.h"
#include <boost/beast/websocket.hpp>
#include <boost/asio.hpp>
#include <atomic>
#include <optional>
#include <string>

namespace hical
{

	/**
 * @brief WebSocket 会话封装
 *
 * 对 Boost.Beast websocket::stream 的 hical 风格封装。
 * 提供协程化的 send/receive 接口。
 */
	class WebSocketSession
	{
	public:
		using WsStream = boost::beast::websocket::stream<boost::asio::ip::tcp::socket>;

		// 默认最大消息大小 1MB，防止恶意客户端发送超大帧导致 OOM
		static constexpr size_t hDefaultMaxMessageSize = 1024 * 1024;

		/**
     * @brief 从已升级的 WebSocket stream 构造
     * @param stream WebSocket 流
     * @param maxMessageSize 最大消息大小（字节，默认 1MB）
     */
		explicit WebSocketSession(WsStream stream, size_t maxMessageSize = hDefaultMaxMessageSize);

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
     *
     * 在协程上下文中安全关闭，不会与 async_read 竞争。
     */
		Awaitable<void> closeAsync();

		/**
     * @brief 同步关闭 WebSocket 连接
     *
     * @warning 必须在 io_context 线程中调用，否则与 async_read 竞争。
     * 推荐使用 closeAsync() 替代。
     */
		void close();

		/**
     * @brief 连接是否仍然打开
     * @return true 如果连接打开
     */
		bool isOpen() const;

		/**
     * @brief 获取底层 WsStream 引用
     * @return WsStream 引用
     */
		WsStream& native();

	private:
		WsStream stream_;
		std::atomic<bool> open_ {true};
	};

} // namespace hical
