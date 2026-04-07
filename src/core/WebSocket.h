#pragma once

#include "Coroutine.h"
#include <boost/beast/websocket.hpp>
#include <boost/asio.hpp>
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
    using WsStream =
        boost::beast::websocket::stream<boost::asio::ip::tcp::socket>;

    /**
     * @brief 从已升级的 WebSocket stream 构造
     * @param stream WebSocket 流
     */
    explicit WebSocketSession(WsStream stream);

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
     * @brief 关闭 WebSocket 连接
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
    bool open_{true};
};

}  // namespace hical
