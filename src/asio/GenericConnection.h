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

	// ============ 模板实现（header-only 部分） ============

	template <typename SocketType>
	GenericConnection<SocketType>::GenericConnection(AsioEventLoop* loop,
													 SocketType socket,
													 const InetAddress& localAddr,
													 const InetAddress& peerAddr)
		: loop_(loop)
		, socket_(std::move(socket))
		, localAddr_(localAddr)
		, peerAddr_(peerAddr)
		, inputBuffer_(MemoryPool::instance().threadLocalAllocator())
	{
	}

	template <typename SocketType>
	GenericConnection<SocketType>::~GenericConnection()
	{
		// 不调用 close()，因为析构时 shared_from_this() 无效（引用计数已为 0）
		// 直接操作底层 socket 关闭
		auto& sock = lowestLayerSocket();
		if (sock.is_open())
		{
			boost::system::error_code ec;
			sock.close(ec);
		}
	}

	// ============ 辅助方法 ============

	template <typename SocketType>
	auto& GenericConnection<SocketType>::lowestLayerSocket()
	{
		if constexpr (hIsSslStream<SocketType>)
		{
			return socket_.lowest_layer();
		}
		else
		{
			return socket_;
		}
	}

	template <typename SocketType>
	auto GenericConnection<SocketType>::socketExecutor()
	{
		if constexpr (hIsSslStream<SocketType>)
		{
			return socket_.lowest_layer().get_executor();
		}
		else
		{
			return socket_.get_executor();
		}
	}

	// ============ 数据发送 ============

	template <typename SocketType>
	void GenericConnection<SocketType>::send(const char* data, size_t len)
	{
		if (state_.load() != State::hConnected)
		{
			return;
		}

		enqueueNode(std::make_shared<MemoryWriteNode>(std::make_shared<std::string>(data, len)));
	}

	template <typename SocketType>
	void GenericConnection<SocketType>::send(const std::string& msg)
	{
		send(msg.data(), msg.size());
	}

	template <typename SocketType>
	void GenericConnection<SocketType>::send(std::string&& msg)
	{
		if (state_.load() != State::hConnected)
		{
			return;
		}

		enqueueNode(std::make_shared<MemoryWriteNode>(std::move(msg)));
	}

	template <typename SocketType>
	void GenericConnection<SocketType>::send(const PmrBuffer& buffer)
	{
		send(buffer.peek(), buffer.readableBytes());
	}

	template <typename SocketType>
	void GenericConnection<SocketType>::send(PmrBuffer&& buffer)
	{
		if (state_.load() != State::hConnected)
		{
			return;
		}

		enqueueNode(std::make_shared<PmrBufferWriteNode>(std::move(buffer)));
	}

	template <typename SocketType>
	void GenericConnection<SocketType>::send(const std::shared_ptr<std::string>& msgPtr)
	{
		if (state_.load() != State::hConnected)
		{
			return;
		}

		enqueueNode(std::make_shared<MemoryWriteNode>(msgPtr));
	}

	template <typename SocketType>
	void GenericConnection<SocketType>::send(const std::shared_ptr<PmrBuffer>& msgPtr)
	{
		if (state_.load() != State::hConnected)
		{
			return;
		}

		enqueueNode(std::make_shared<MemoryWriteNode>(msgPtr->readAll()));
	}

	/**
	 * @brief 将写节点入队并触发写操作（统一的入队路径）
	 * 线程安全：可从任意线程调用，内部通过 loop_->post 保证线程安全。
	 */
	template <typename SocketType>
	void GenericConnection<SocketType>::enqueueNode(std::shared_ptr<WriteNode> node)
	{
		auto enqueue = [this, node = std::move(node)]()
		{
			{
				std::lock_guard<std::mutex> lock(writeMutex_);
				queuedBytes_ += node->size();
				writeQueue_.push_back(std::move(node));

				// O(1) 高水位检查
				if (highWaterMarkCallback_ && queuedBytes_ >= highWaterMark_)
				{
					size_t currentQueuedBytes = queuedBytes_;
					auto self = sharedThis();
					loop_->post(
						[this, currentQueuedBytes, self]()
						{
							highWaterMarkCallback_(std::static_pointer_cast<TcpConnection>(self), currentQueuedBytes);
						});
				}
			}

			tryStartWrite();
		};

		if (loop_->isInLoopThread())
		{
			enqueue();
		}
		else
		{
			auto self = sharedThis();
			loop_->post(
				[self, enqueue = std::move(enqueue)]() mutable
				{
					enqueue();
				});
		}
	}

	template <typename SocketType>
	void GenericConnection<SocketType>::tryStartWrite()
	{
		// 如果没有在写，启动写协程
		bool expected = false;
		if (writing_.compare_exchange_strong(expected, true))
		{
			auto self = std::static_pointer_cast<GenericConnection<SocketType>>(sharedThis());
			boost::asio::co_spawn(
				socketExecutor(),
				[self]() -> boost::asio::awaitable<void>
				{
					co_await self->writeLoop();
				},
				boost::asio::detached);
		}
	}

	// ============ 连接控制 ============

	template <typename SocketType>
	void GenericConnection<SocketType>::shutdown()
	{
		if (state_.load() == State::hConnected)
		{
			state_.store(State::hDisconnecting);
			auto self = sharedThis();
			loop_->dispatch(
				[this, self]()
				{
					shutdownInLoop();
				});
		}
	}

	template <typename SocketType>
	void GenericConnection<SocketType>::shutdownInLoop()
	{
		if constexpr (hIsSslStream<SocketType>)
		{
			// SSL 连接：先执行 TLS close_notify，再做 TCP shutdown
			auto self = sharedThis();
			boost::asio::co_spawn(
				socketExecutor(),
				[self]() -> boost::asio::awaitable<void>
				{
					try
					{
						co_await self->socket_.async_shutdown(boost::asio::use_awaitable);
					}
					catch (const boost::system::system_error&)
					{
						// 忽略 shutdown 错误（对端可能已关闭）
					}
					boost::system::error_code ec;
					auto& sock = self->lowestLayerSocket();
					if (sock.is_open())
					{
						sock.shutdown(boost::asio::ip::tcp::socket::shutdown_send, ec);
					}
				},
				boost::asio::detached);
		}
		else
		{
			auto& sock = lowestLayerSocket();
			if (!sock.is_open())
			{
				return;
			}

			boost::system::error_code ec;
			sock.shutdown(boost::asio::ip::tcp::socket::shutdown_send, ec);
		}
	}

	template <typename SocketType>
	void GenericConnection<SocketType>::close()
	{
		// 仅从 hConnected 转为 hDisconnecting；实际 hDisconnected 在 closeInLoop 中设置
		auto expected = State::hConnected;
		if (state_.compare_exchange_strong(expected, State::hDisconnecting))
		{
			auto self = sharedThis();
			loop_->dispatch(
				[this, self]()
				{
					closeInLoop();
				});
			return;
		}
		// 已经是 hDisconnecting 状态也尝试关闭
		if (expected == State::hDisconnecting)
		{
			return;
		}
	}

	template <typename SocketType>
	void GenericConnection<SocketType>::closeInLoop()
	{
		auto& sock = lowestLayerSocket();
		if (sock.is_open())
		{
			boost::system::error_code ec;
			sock.close(ec);
		}
		handleClose();
	}

	template <typename SocketType>
	void GenericConnection<SocketType>::setTcpNoDelay(bool on)
	{
		boost::asio::ip::tcp::no_delay option(on);
		boost::system::error_code ec;
		lowestLayerSocket().set_option(option, ec);
	}

	template <typename SocketType>
	void GenericConnection<SocketType>::startRead()
	{
		auto self = sharedThis();
		loop_->dispatch(
			[this, self]()
			{
				if (!reading_ && state_.load() == State::hConnected)
				{
					reading_ = true;
					auto conn = std::static_pointer_cast<GenericConnection<SocketType>>(self);
					boost::asio::co_spawn(
						socketExecutor(),
						[conn]() -> boost::asio::awaitable<void>
						{
							co_await conn->readLoop();
						},
						boost::asio::detached);
				}
			});
	}

	template <typename SocketType>
	void GenericConnection<SocketType>::stopRead()
	{
		reading_ = false;
	}

	// ============ 状态查询 ============

	template <typename SocketType>
	bool GenericConnection<SocketType>::connected() const
	{
		return state_.load() == State::hConnected;
	}

	template <typename SocketType>
	bool GenericConnection<SocketType>::disconnected() const
	{
		return state_.load() == State::hDisconnected;
	}

	template <typename SocketType>
	const InetAddress& GenericConnection<SocketType>::localAddr() const
	{
		return localAddr_;
	}

	template <typename SocketType>
	const InetAddress& GenericConnection<SocketType>::peerAddr() const
	{
		return peerAddr_;
	}

	template <typename SocketType>
	EventLoop* GenericConnection<SocketType>::getLoop()
	{
		return loop_;
	}

	template <typename SocketType>
	size_t GenericConnection<SocketType>::bytesSent() const
	{
		return bytesSent_.load();
	}

	template <typename SocketType>
	size_t GenericConnection<SocketType>::bytesReceived() const
	{
		return bytesReceived_.load();
	}

	template <typename SocketType>
	std::chrono::steady_clock::time_point GenericConnection<SocketType>::lastActiveTime() const
	{
		auto ms = std::chrono::milliseconds(lastActiveTimeMs_.load(std::memory_order_relaxed));
		return std::chrono::steady_clock::time_point(ms);
	}

	template <typename SocketType>
	void GenericConnection<SocketType>::sendFile(const std::filesystem::path& path, int64_t offset, int64_t length)
	{
		if (state_.load() != State::hConnected)
		{
			return;
		}

		enqueueNode(std::make_shared<FileWriteNode>(path, offset, length));
	}

	// ============ 回调设置 ============

	template <typename SocketType>
	void GenericConnection<SocketType>::onMessage(MessageCallback cb)
	{
		messageCallback_ = std::move(cb);
	}

	template <typename SocketType>
	void GenericConnection<SocketType>::onConnection(ConnectionCallback cb)
	{
		connectionCallback_ = std::move(cb);
	}

	template <typename SocketType>
	void GenericConnection<SocketType>::onClose(CloseCallback cb)
	{
		closeCallback_ = std::move(cb);
	}

	template <typename SocketType>
	void GenericConnection<SocketType>::onWriteComplete(WriteCompleteCallback cb)
	{
		writeCompleteCallback_ = std::move(cb);
	}

	template <typename SocketType>
	void GenericConnection<SocketType>::onHighWaterMark(HighWaterMarkCallback cb, size_t markLen)
	{
		highWaterMarkCallback_ = std::move(cb);
		highWaterMark_ = markLen;
	}

	// ============ 用户上下文 ============

	template <typename SocketType>
	void GenericConnection<SocketType>::setContext(const std::shared_ptr<void>& context)
	{
		context_ = context;
	}

	template <typename SocketType>
	bool GenericConnection<SocketType>::hasContext() const
	{
		return context_ != nullptr;
	}

	template <typename SocketType>
	void GenericConnection<SocketType>::clearContext()
	{
		context_.reset();
	}

	template <typename SocketType>
	std::shared_ptr<void> GenericConnection<SocketType>::getContextInternal() const
	{
		return context_;
	}

	// ============ 内部实现 ============

	template <typename SocketType>
	void GenericConnection<SocketType>::connectEstablished()
	{
		auto self = sharedThis();
		loop_->dispatch(
			[this, self]()
			{
				state_.store(State::hConnected);
				updateLastActiveTime();

				if constexpr (hIsSslStream<SocketType>)
				{
					// SSL 连接：先执行握手，再触发回调和读取
					auto conn = std::static_pointer_cast<GenericConnection<SocketType>>(self);
					boost::asio::co_spawn(
						socketExecutor(),
						[conn]() -> boost::asio::awaitable<void>
						{
							co_await conn->sslHandshake();
						},
						boost::asio::detached);
				}
				else
				{
					if (connectionCallback_)
					{
						connectionCallback_(std::static_pointer_cast<TcpConnection>(self));
					}
					startRead();
				}
			});
	}

	template <typename SocketType>
	void GenericConnection<SocketType>::connectDestroyed()
	{
		auto self = sharedThis();
		loop_->dispatch(
			[this, self]()
			{
				if (state_.load() == State::hConnected)
				{
					state_.store(State::hDisconnected);
					reading_ = false;

					if (connectionCallback_)
					{
						connectionCallback_(std::static_pointer_cast<TcpConnection>(self));
					}
				}
			});
	}

	// ============ SSL 握手 ============

	template <typename SocketType>
	boost::asio::awaitable<void> GenericConnection<SocketType>::sslHandshake()
	{
		if constexpr (hIsSslStream<SocketType>)
		{
			try
			{
				co_await socket_.async_handshake(boost::asio::ssl::stream_base::server, boost::asio::use_awaitable);

				// 握手成功，触发连接回调并开始读取
				auto self = sharedThis();
				if (connectionCallback_)
				{
					connectionCallback_(std::static_pointer_cast<TcpConnection>(self));
				}
				startRead();
			}
			catch (const boost::system::system_error&)
			{
				handleClose();
			}
		}
		co_return;
	}

	// ============ 协程式异步 I/O ============

	template <typename SocketType>
	boost::asio::awaitable<void> GenericConnection<SocketType>::readLoop()
	{
		try
		{
			static constexpr size_t hMinReadSize = 4096;
			while (reading_ && state_.load() == State::hConnected)
			{
				// 零拷贝：直接读入 inputBuffer_ 的可写区域
				inputBuffer_.ensureWritableBytes(hMinReadSize);
				auto bytesRead = co_await socket_.async_read_some(
					boost::asio::buffer(inputBuffer_.beginWrite(), inputBuffer_.writableBytes()),
					boost::asio::use_awaitable);

				bytesReceived_ += bytesRead;
				updateLastActiveTime();
				inputBuffer_.hasWritten(bytesRead);

				if (messageCallback_)
				{
					messageCallback_(std::static_pointer_cast<TcpConnection>(sharedThis()), &inputBuffer_);
				}
			}
		}
		catch (const boost::system::system_error& e)
		{
			// operation_aborted 表示主动关闭，由 close/shutdown 路径处理
			// 其他所有错误（eof、connection_reset、broken_pipe 等）都需清理连接
			if (e.code() != boost::asio::error::operation_aborted)
			{
				handleClose();
			}
		}
	}

	template <typename SocketType>
	boost::asio::awaitable<void> GenericConnection<SocketType>::writeLoop()
	{
		try
		{
			while (true)
			{
				// 批量取出所有待发送节点
				std::deque<std::shared_ptr<WriteNode>> batch;

				{
					std::lock_guard<std::mutex> lock(writeMutex_);
					if (writeQueue_.empty())
					{
						writing_.store(false);

						// 在锁内重检队列，防止 set false 与 enqueue 之间的竞态丢消息
						if (!writeQueue_.empty())
						{
							writing_.store(true);
							batch.swap(writeQueue_);
							for (const auto& node : batch)
							{
								queuedBytes_ -= node->size();
							}
						}
						else
						{
							if (writeCompleteCallback_)
							{
								auto self = std::static_pointer_cast<TcpConnection>(sharedThis());
								loop_->post(
									[this, self]()
									{
										writeCompleteCallback_(self);
									});
							}

							co_return;
						}
					}
					else
					{
						batch.swap(writeQueue_);
						for (const auto& node : batch)
						{
							queuedBytes_ -= node->size();
						}
					}
				}

				// 发送逻辑：收集连续内存节点做 scatter-gather，遇到文件节点单独发送
				size_t idx = 0;
				while (idx < batch.size())
				{
					// 文件节点：单独发送
					if (batch[idx]->isFile())
					{
						auto& fileNode = static_cast<FileWriteNode&>(*batch[idx]);
						auto sent = co_await sendFileNode(fileNode);
						bytesSent_ += sent;
						updateLastActiveTime();
						++idx;
						continue;
					}

					// 收集连续的内存节点
					size_t memStart = idx;
					while (idx < batch.size() && !batch[idx]->isFile())
					{
						++idx;
					}
					size_t memCount = idx - memStart;

					if (memCount == 1)
					{
						auto bytesWritten = co_await boost::asio::async_write(socket_,
																			  batch[memStart]->asBuffer(),
																			  boost::asio::use_awaitable);
						bytesSent_ += bytesWritten;
						updateLastActiveTime();
					}
					else
					{
						// Scatter-Gather I/O
						static constexpr size_t hSmallBatchSize = 16;
						if (memCount <= hSmallBatchSize)
						{
							std::array<boost::asio::const_buffer, hSmallBatchSize> stackBufs;
							for (size_t i = 0; i < memCount; ++i)
							{
								stackBufs[i] = batch[memStart + i]->asBuffer();
							}
							auto bytesWritten = co_await boost::asio::async_write(
								socket_,
								std::span<const boost::asio::const_buffer>(stackBufs.data(), memCount),
								boost::asio::use_awaitable);
							bytesSent_ += bytesWritten;
							updateLastActiveTime();
						}
						else
						{
							std::vector<boost::asio::const_buffer> buffers;
							buffers.reserve(memCount);
							for (size_t i = 0; i < memCount; ++i)
							{
								buffers.emplace_back(batch[memStart + i]->asBuffer());
							}
							auto bytesWritten =
								co_await boost::asio::async_write(socket_, buffers, boost::asio::use_awaitable);
							bytesSent_ += bytesWritten;
							updateLastActiveTime();
						}
					}
				}
			}
		}
		catch (const boost::system::system_error&)
		{
			writing_.store(false);
			handleClose();
		}
	}

	/**
	 * @brief 协程式文件异步分块发送
	 * 使用 boost::asio::random_access_file 异步读取，不阻塞 io_context 线程。
	 * @return 发送的总字节数
	 */
	template <typename SocketType>
	boost::asio::awaitable<size_t> GenericConnection<SocketType>::sendFileNode(const FileWriteNode& node)
	{
		static constexpr size_t hChunkSize = 65536; // 64KB

#ifdef BOOST_ASIO_HAS_FILE
		boost::asio::random_access_file file(socketExecutor(),
											 node.path().string(),
											 boost::asio::random_access_file::read_only);

		int64_t remaining = node.length();
		size_t totalSent = 0;
		uint64_t offset = static_cast<uint64_t>(node.offset());
		std::string chunk((std::min)(static_cast<int64_t>(hChunkSize), remaining), '\0');

		while (remaining > 0)
		{
			auto toRead = (std::min)(static_cast<int64_t>(hChunkSize), remaining);
			auto bytesRead =
				co_await file.async_read_some_at(offset,
												 boost::asio::buffer(chunk.data(), static_cast<size_t>(toRead)),
												 boost::asio::use_awaitable);

			if (bytesRead == 0)
			{
				break;
			}

			auto written = co_await boost::asio::async_write(socket_,
															 boost::asio::buffer(chunk.data(), bytesRead),
															 boost::asio::use_awaitable);
			totalSent += written;
			offset += bytesRead;
			remaining -= static_cast<int64_t>(bytesRead);
		}

		co_return totalSent;
#else
		// 同步 ifstream 回退（macOS 等不支持 BOOST_ASIO_HAS_FILE 的平台）
		std::ifstream ifs(node.path(), std::ios::binary);
		if (!ifs)
		{
			co_return 0;
		}

		ifs.seekg(node.offset());
		if (!ifs)
		{
			co_return 0;
		}

		int64_t remaining = node.length();
		size_t totalSent = 0;
		std::string chunk((std::min)(static_cast<int64_t>(hChunkSize), remaining), '\0');

		while (remaining > 0)
		{
			auto toRead = (std::min)(static_cast<int64_t>(hChunkSize), remaining);
			ifs.read(chunk.data(), toRead);
			auto bytesRead = static_cast<size_t>(ifs.gcount());
			if (bytesRead == 0)
			{
				break;
			}

			auto written = co_await boost::asio::async_write(socket_,
															 boost::asio::buffer(chunk.data(), bytesRead),
															 boost::asio::use_awaitable);
			totalSent += written;
			remaining -= static_cast<int64_t>(bytesRead);
		}

		co_return totalSent;
#endif
	}

	template <typename SocketType>
	void GenericConnection<SocketType>::handleClose()
	{
		// CAS 保护：readLoop 和 writeLoop 可能并发调用 handleClose()，
		// 确保回调只触发一次
		auto expected = State::hConnected;
		if (!state_.compare_exchange_strong(expected, State::hDisconnected))
		{
			expected = State::hDisconnecting;
			if (!state_.compare_exchange_strong(expected, State::hDisconnected))
			{
				return; // 已经 hDisconnected，跳过
			}
		}

		reading_ = false;

		auto self = std::static_pointer_cast<TcpConnection>(sharedThis());

		if (connectionCallback_)
		{
			connectionCallback_(self);
		}

		if (closeCallback_)
		{
			closeCallback_(self);
		}
	}

} // namespace hical
