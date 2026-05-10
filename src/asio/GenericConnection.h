#pragma once

#include "../core/TcpConnection.h"
#include "../core/EventLoop.h"
#include "../core/PmrBuffer.h"
#include "../core/InetAddress.h"
#include "../core/MemoryPool.h"
#include "../core/WriteNode.h"
#include "AsioEventLoop.h"
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#ifdef BOOST_ASIO_HAS_FILE
	#include <boost/asio/random_access_file.hpp>
#endif
#include <boost/asio/awaitable.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <array>
#include <atomic>
#include <chrono>
#include <deque>
#include <fstream>
#include <memory>
#include <mutex>
#include <span>
#include <type_traits>

namespace hical
{

	/**
	 * @brief 判断 SocketType 是否为 SSL 流
	 */
	template <typename T>
	struct IsSslStream : std::false_type
	{
	};

	template <typename T>
	struct IsSslStream<boost::asio::ssl::stream<T>> : std::true_type
	{
	};

	template <typename T>
	inline constexpr bool hIsSslStream = IsSslStream<T>::value;

	/**
	 * @brief 通用连接模板类（支持普通 TCP 和 SSL）
	 * 模板参数 SocketType：
	 * - boost::asio::ip::tcp::socket（普通连接）
	 * - boost::asio::ssl::stream<tcp::socket>（SSL 连接）
	 * 两种 socket 类型共享同一套协程读写逻辑。
	 * SSL 连接在 connectEstablished 时自动执行握手。
	 */
	template <typename SocketType>
	class GenericConnection : public TcpConnection
	{
	public:
		using Ptr = std::shared_ptr<GenericConnection<SocketType>>;

		/**
		 * @brief 构造普通 TCP 连接
		 * @param loop 所属事件循环
		 * @param socket 已连接的 socket
		 * @param localAddr 本地地址
		 * @param peerAddr 远端地址
		 */
		GenericConnection(AsioEventLoop* loop,
						  SocketType socket,
						  const InetAddress& localAddr,
						  const InetAddress& peerAddr);

		~GenericConnection() override;

		// ============ 数据发送 ============

		void send(const char* data, size_t len) override;
		void send(const std::string& msg) override;
		void send(std::string&& msg) override;
		void send(const PmrBuffer& buffer) override;
		void send(PmrBuffer&& buffer) override;
		void send(const std::shared_ptr<std::string>& msgPtr) override;
		void send(const std::shared_ptr<PmrBuffer>& msgPtr) override;

		// ============ 连接控制 ============

		void shutdown() override;
		void close() override;
		void setTcpNoDelay(bool on) override;
		void startRead() override;
		void stopRead() override;

		// ============ 状态查询 ============

		bool connected() const override;
		bool disconnected() const override;
		const InetAddress& localAddr() const override;
		const InetAddress& peerAddr() const override;
		EventLoop* getLoop() override;
		size_t bytesSent() const override;
		size_t bytesReceived() const override;
		std::chrono::steady_clock::time_point lastActiveTime() const override;

		// ============ 文件发送 ============

		void sendFile(const std::filesystem::path& path, int64_t offset = 0, int64_t length = -1) override;

		// ============ 回调设置（hical 风格命名）============
		// @warning 线程安全约束：所有回调必须在 connectEstablished() 调用前设置完毕。
		// 连接运行期间从其他线程修改回调会导致数据竞争。

		void onMessage(MessageCallback cb) override;
		void onConnection(ConnectionCallback cb) override;
		void onClose(CloseCallback cb) override;
		void onWriteComplete(WriteCompleteCallback cb) override;
		void onHighWaterMark(HighWaterMarkCallback cb, size_t markLen) override;

		// ============ 用户上下文 ============

		void setContext(const std::shared_ptr<void>& context) override;
		bool hasContext() const override;
		void clearContext() override;

		// ============ 内部接口 ============

		/**
		 * @brief 连接建立后调用（由 TcpServer 调用）
		 * @note SSL 连接会自动执行握手
		 */
		void connectEstablished() override;

		/**
		 * @brief 连接销毁前调用（由 TcpServer 调用）
		 */
		void connectDestroyed() override;

		/**
		 * @brief 是否为 SSL 连接
		 * @return true 如果是 SSL 连接
		 */
		static constexpr bool isSsl()
		{
			return hIsSslStream<SocketType>;
		}

	protected:
		std::shared_ptr<void> getContextInternal() const override;

	private:
		enum class State
		{
			hConnecting,
			hConnected,
			hDisconnecting,
			hDisconnected
		};

		/**
		 * @brief 获取指向自身的 shared_ptr（正确类型）
		 */
		std::shared_ptr<GenericConnection<SocketType>> sharedThis()
		{
			return std::static_pointer_cast<GenericConnection<SocketType>>(shared_from_this());
		}

		/**
		 * @brief 获取最底层 socket（用于设置 socket 选项、检查 is_open 等）
		 * @return 最底层 socket 引用
		 */
		auto& lowestLayerSocket();

		/**
		 * @brief 获取 executor（用于 co_spawn）
		 */
		auto socketExecutor();

		// 协程式异步操作
		boost::asio::awaitable<void> sslHandshake();
		boost::asio::awaitable<void> readLoop();
		boost::asio::awaitable<void> writeLoop();

		void handleClose();
		void shutdownInLoop();
		void closeInLoop();

		/**
		 * @brief 刷新最后活跃时间（readLoop/writeLoop 中调用）
		 */
		void updateLastActiveTime()
		{
			lastActiveTimeMs_.store(std::chrono::duration_cast<std::chrono::milliseconds>(
										std::chrono::steady_clock::now().time_since_epoch())
										.count(),
									std::memory_order_relaxed);
		}

		void enqueueNode(std::shared_ptr<WriteNode> node);
		void tryStartWrite();

		// 协程式文件发送（writeLoop 内部调用）
		boost::asio::awaitable<size_t> sendFileNode(const FileWriteNode& node);

		AsioEventLoop* loop_;
		SocketType socket_;
		std::atomic<State> state_ {State::hConnecting};
		std::atomic<bool> reading_ {false};

		InetAddress localAddr_;
		InetAddress peerAddr_;

		// 接收缓冲区（pmr 分配）
		PmrBuffer inputBuffer_;

		// 发送队列（多态写节点：内存数据 / 文件）
		std::deque<std::shared_ptr<WriteNode>> writeQueue_;
		size_t queuedBytes_ {0}; // 发送队列累计字节数（O(1) 高水位线检查）
		std::mutex writeMutex_;
		std::atomic<bool> writing_ {false};

		// 统计
		std::atomic<size_t> bytesSent_ {0};
		std::atomic<size_t> bytesReceived_ {0};

		// 最后活跃时间（用于空闲连接超时检测）
		std::atomic<int64_t> lastActiveTimeMs_ {
			std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
				.count()};

		// 回调（hical 风格）
		MessageCallback messageCallback_;
		ConnectionCallback connectionCallback_;
		CloseCallback closeCallback_;
		WriteCompleteCallback writeCompleteCallback_;
		HighWaterMarkCallback highWaterMarkCallback_;
		size_t highWaterMark_ {64 * 1024 * 1024}; // 64MB

		// 用户上下文
		std::shared_ptr<void> context_;
	};

	// ============ 类型别名 ============

	/** 普通 TCP 连接 */
	using PlainConnection = GenericConnection<boost::asio::ip::tcp::socket>;

	// ============ 显式实例化声明 ============
	// 抑制隐式实例化 — 方法实现在 GenericConnection.hci 中，
	// 显式实例化集中在 GenericConnection.cpp

	extern template class GenericConnection<boost::asio::ip::tcp::socket>;
	extern template class GenericConnection<boost::asio::ssl::stream<boost::asio::ip::tcp::socket>>;

} // namespace hical
