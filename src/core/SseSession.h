/**
 * @file SseSession.h
 * @brief Server-Sent Events (SSE) 会话封装
 * 提供协程化的 sendEvent/sendComment 接口，
 * 基于 HTTP chunked transfer-encoding 实现流式推送（RFC 8895）。
 */

#pragma once

#include "Coroutine.h"
#include <boost/asio.hpp>
#include <atomic>
#include <memory>
#include <string>
#include <string_view>

namespace hical
{

	/**
	 * @brief SSE 事件结构
	 * 对应 SSE 协议中的 event/data/id/retry 字段，
	 * 调用 send() 后序列化为 wire format 并经由 chunked encoding 发送。
	 */
	struct SseEvent
	{
		std::string_view data;  ///< 事件数据（必填）
		std::string_view event; ///< 事件类型（可选，默认 "message"）
		std::string_view id;    ///< 事件 ID（可选，Last-Event-Id 断线重连用）
		std::string_view retry; ///< 重连间隔（可选，毫秒）
	};

	/**
	 * @brief SSE 会话封装
	 * 基于 TCP socket + chunked transfer-encoding 的 Server-Sent Events 实现。
	 * 线程安全性：非线程安全，所有操作须在同一个 executor 上串行调用。
	 * 用法：
	 * ```cpp
	 * auto sse = std::make_shared<SseSession>(std::move(socket));
	 * co_await sse->sendResponseHead();
	 * co_await sse->sendData("hello");
	 * co_await sse->close();
	 * ```
	 */
	class SseSession : public std::enable_shared_from_this<SseSession>
	{
	public:
		/**
		 * @brief 构造 SSE 会话
		 * @param socket 已建立连接的 TCP socket
		 */
		explicit SseSession(boost::asio::ip::tcp::socket socket);

		~SseSession();

		SseSession(const SseSession&) = delete;
		SseSession& operator=(const SseSession&) = delete;

		/**
		 * @brief 发送 HTTP 响应头（必须在 sendEvent 前调用，仅一次）
		 * 发送：200 OK + Content-Type: text/event-stream + Transfer-Encoding: chunked
		 * + Cache-Control: no-cache + Connection: keep-alive
		 * 用 FixedBuffer<512> 栈上构建，零堆分配。
		 */
		Awaitable<void> sendResponseHead();

		/**
		 * @brief 发送 SSE 事件
		 * 序列化为 SSE wire format（data/event/id/retry），
		 * 再包装为 chunked transfer-encoding 帧后写入 socket。
		 * @param event SSE 事件（data 必填，其余可选）
		 */
		Awaitable<void> sendEvent(const SseEvent& event);

		/**
		 * @brief 发送纯文本 SSE 事件（无 event/id/retry 字段）
		 * @param data 事件数据
		 * 等价于 sendEvent({.data = data})
		 * 常见场景的快捷接口（不需要 event/id/retry 时直接用这个），
		 * 但不处理 data 中含 \n 需拆行的情况（由 sendEvent 处理）。
		 */
		Awaitable<void> sendData(std::string_view data);

		/**
		 * @brief 发送 SSE 注释（冒号开头，客户端忽略，用于 keepalive）
		 * @param comment 注释内容（不含前导冒号）
		 * wire: : comment\n\n
		 */
		Awaitable<void> sendComment(std::string_view comment);

		/**
		 * @brief 发送 chunked 终止帧并关闭连接
		 * 先发送 0\r\n\r\n 表示 chunked body 结束，然后关闭 socket。
		 */
		Awaitable<void> close();

		/**
		 * @brief 连接是否仍处于打开状态
		 * @return true 如果 socket 未关闭且未标记为已关闭
		 */
		bool isOpen() const
		{
			return alive_.load(std::memory_order_acquire);
		}

		/**
		 * @brief 获取底层 TCP socket 引用
		 * @return socket 引用
		 */
		boost::asio::ip::tcp::socket& socket()
		{
			return socket_;
		}

	private:
		boost::asio::ip::tcp::socket socket_;
		std::atomic<bool> alive_ {true};
		bool headSent_ {false}; ///< sendResponseHead 是否已调用

		/**
		 * @brief 跨 sendEvent/sendComment 复用的序列化缓冲区
		 * 避免每次事件推送时重复堆分配，热路径下提升性能。
		 * 在 sendEvent 中 clear() 后重用，保留已分配的容量。
		 */
		mutable std::string sendBuf_;
	};

} // namespace hical
