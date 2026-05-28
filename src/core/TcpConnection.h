/**
 * @file TcpConnection.h
 * @brief TCP 连接抽象接口
 */

#pragma once

#include "PmrBuffer.h"
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace hical
{

	class EventLoop;
	class InetAddress;

	/**
	 * @brief TCP 连接抽象接口（hical 风格）
	 * 与旧版 ITcpConnection 的区别：
	 * - 回调命名：recvMsgCallback_ -> onMessage
	 * - 回调命名：closeCallback_ -> onClose
	 * - 回调命名：writeCompleteCallback_ -> onWriteComplete
	 * - 使用 PmrBuffer 替代 MsgBuffer
	 * - 新增协程支持接口
	 */
	class TcpConnection : public std::enable_shared_from_this<TcpConnection>
	{
	public:
		using Ptr = std::shared_ptr<TcpConnection>;

		// 回调类型定义（hical 风格命名）
		using MessageCallback = std::function<void(const Ptr&, PmrBuffer*)>;
		using ConnectionCallback = std::function<void(const Ptr&)>;
		using CloseCallback = std::function<void(const Ptr&)>;
		using WriteCompleteCallback = std::function<void(const Ptr&)>;
		using HighWaterMarkCallback = std::function<void(const Ptr&, size_t)>;

		virtual ~TcpConnection() = default;

		// ============ 数据发送 ============

		/**
		 * @brief 发送数据
		 * @param data 数据指针
		 * @param len 数据长度
		 */
		virtual void send(const char* data, size_t len) = 0;

		/**
		 * @brief 发送字符串
		 * @param msg 字符串
		 */
		virtual void send(const std::string& msg) = 0;

		/**
		 * @brief 发送字符串（移动语义）
		 * @param msg 字符串
		 */
		virtual void send(std::string&& msg) = 0;

		/**
		 * @brief 发送缓冲区数据
		 * @param buffer 消息缓冲区
		 */
		virtual void send(const PmrBuffer& buffer) = 0;

		/**
		 * @brief 发送缓冲区数据（移动语义）
		 * @param buffer 消息缓冲区
		 */
		virtual void send(PmrBuffer&& buffer) = 0;

		/**
		 * @brief 发送共享字符串
		 * @param msgPtr 字符串智能指针
		 */
		virtual void send(const std::shared_ptr<std::string>& msgPtr) = 0;

		/**
		 * @brief 发送共享缓冲区
		 * @param msgPtr 缓冲区智能指针
		 */
		virtual void send(const std::shared_ptr<PmrBuffer>& msgPtr) = 0;

		// ============ 连接控制 ============

		/**
		 * @brief 关闭连接（半关闭，只关闭写端）
		 */
		virtual void shutdown() = 0;

		/**
		 * @brief 强制关闭连接
		 */
		virtual void close() = 0;

		/**
		 * @brief 设置 TCP_NODELAY 选项
		 * @param on 是否启用
		 */
		virtual void setTcpNoDelay(bool on) = 0;

		/**
		 * @brief 启动读取（开始接收数据）
		 */
		virtual void startRead() = 0;

		/**
		 * @brief 停止读取
		 */
		virtual void stopRead() = 0;

		// ============ 连接生命周期（由 TcpServer 调用） ============

		/**
		 * @brief 连接建立后调用（SSL 连接会自动执行握手）
		 */
		virtual void connectEstablished() = 0;

		/**
		 * @brief 连接销毁前调用
		 */
		virtual void connectDestroyed() = 0;

		// ============ 状态查询 ============

		/**
		 * @brief 连接是否已建立
		 * @return true 如果已连接
		 */
		virtual bool connected() const = 0;

		/**
		 * @brief 连接是否已断开
		 * @return true 如果已断开
		 */
		virtual bool disconnected() const = 0;

		/**
		 * @brief 获取本地地址
		 * @return 本地地址
		 */
		virtual const InetAddress& localAddr() const = 0;

		/**
		 * @brief 获取远端地址
		 * @return 远端地址
		 */
		virtual const InetAddress& peerAddr() const = 0;

		/**
		 * @brief 获取所属事件循环
		 * @return 事件循环指针
		 */
		virtual EventLoop* getLoop() = 0;

		/**
		 * @brief 获取已发送字节数
		 * @return 字节数
		 */
		virtual size_t bytesSent() const = 0;

		/**
		 * @brief 获取已接收字节数
		 * @return 字节数
		 */
		virtual size_t bytesReceived() const = 0;

		/**
		 * @brief 获取最后活跃时间
		 * @return 最后一次读写数据的时间点
		 */
		virtual std::chrono::steady_clock::time_point lastActiveTime() const = 0;

		// ============ 文件发送 ============

		/**
		 * @brief 发送文件（支持零拷贝优化）
		 * @param path 文件路径
		 * @param offset 起始偏移量（默认 0）
		 * @param length 发送长度（-1 表示到文件末尾）
		 */
		virtual void sendFile(const std::filesystem::path& path, int64_t offset = 0, int64_t length = -1) = 0;

		// ============ 回调设置 ============
		// lambda 里别按值捕获 conn 的 shared_ptr，会循环引用。用参数 c 或 weak_from_this()。

		/**
		 * @brief 设置消息接收回调
		 * @param cb 回调函数
		 */
		virtual void onMessage(MessageCallback cb) = 0;

		/**
		 * @brief 设置连接建立/断开回调
		 * @param cb 回调函数
		 */
		virtual void onConnection(ConnectionCallback cb) = 0;

		/**
		 * @brief 设置连接关闭回调
		 * @param cb 回调函数
		 */
		virtual void onClose(CloseCallback cb) = 0;

		/**
		 * @brief 设置写完成回调
		 * @param cb 回调函数
		 */
		virtual void onWriteComplete(WriteCompleteCallback cb) = 0;

		/**
		 * @brief 设置高水位回调
		 * @param cb 回调函数
		 * @param markLen 高水位字节数
		 */
		virtual void onHighWaterMark(HighWaterMarkCallback cb, size_t markLen) = 0;

		// ============ 用户上下文 ============

		/**
		 * @brief 设置用户上下文
		 * @param context 上下文对象
		 */
		virtual void setContext(const std::shared_ptr<void>& context) = 0;

		/**
		 * @brief 获取用户上下文
		 * @tparam T 上下文类型
		 * @return 上下文对象
		 */
		template <typename T>
		std::shared_ptr<T> getContext() const
		{
			return std::static_pointer_cast<T>(getContextInternal());
		}

		/**
		 * @brief 是否设置了用户上下文
		 * @return true 如果已设置
		 */
		virtual bool hasContext() const = 0;

		/**
		 * @brief 清除用户上下文
		 */
		virtual void clearContext() = 0;

	protected:
		/**
		 * @brief 获取用户上下文（内部实现）
		 * @return 上下文对象
		 */
		virtual std::shared_ptr<void> getContextInternal() const = 0;
	};

} // namespace hical
