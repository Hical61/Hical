/**
 * @file GenericConnection.h
 * @brief 泛型 TCP/SSL 连接模板
 */

#pragma once

#include "../core/TcpConnection.h"
#include "../core/EventLoop.h"
#include "../core/PmrBuffer.h"
#include "../core/InetAddress.h"
#include "../core/MemoryPool.h"
#include "../core/WriteNode.h"
#include "../core/StringPool.h"
#include "../core/Coroutine.h"
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
#include <fstream>
#include <memory>
#include <optional>
#include <span>
#include <type_traits>
#include <vector>

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

		// ============ 回调设置 ============
		// 回调必须在 connectEstablished() 前设好，运行期改会 race

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

		/**
		 * @brief 写队列里的一个发送条目
		 * 内存数据直接持有 shared_ptr<string>，比原来的 WriteNode 少两层间接。
		 * 文件发送还是走 WriteNode 多态，不常见所以无所谓。
		 */
		struct WriteEntry
		{
			enum class Type : uint8_t
			{
				hMemory, // 内存数据（快速路径，零虚函数开销）
				hNode    // 多态节点（PmrBuffer/File，走虚函数慢路径）
			};

			Type type;
			std::shared_ptr<std::string> memData; // hMemory 时有效
			std::shared_ptr<WriteNode> node;      // hNode 时有效

			static WriteEntry fromMemory(std::shared_ptr<std::string> data)
			{
				return WriteEntry {Type::hMemory, std::move(data), nullptr};
			}

			static WriteEntry fromNode(std::shared_ptr<WriteNode> n)
			{
				return WriteEntry {Type::hNode, nullptr, std::move(n)};
			}

			size_t size() const
			{
				if (type == Type::hMemory)
				{
					return memData ? memData->size() : 0;
				}
				return node ? node->size() : 0;
			}

			bool isFile() const
			{
				return type == Type::hNode && node && node->isFile();
			}

			boost::asio::const_buffer asBuffer() const
			{
				if (type == Type::hMemory)
				{
					return boost::asio::buffer(*memData);
				}
				return node ? node->asBuffer() : boost::asio::const_buffer {};
			}
		};

		void enqueueEntry(WriteEntry entry);
		void tryStartWrite();

		/**
		 * @brief MPSC 队列节点（intrusive linked list）
		 */
		struct MpscNode
		{
			std::atomic<MpscNode*> next {nullptr};
			WriteEntry entry;

			explicit MpscNode(WriteEntry e) : entry(std::move(e))
			{
			}

			MpscNode() = default; // stub 节点用
		};

		/**
		 * @brief thread_local MpscNode 对象池
		 * 每个线程维护一个 free list（上限 128），send + writeLoop 同线程时
		 * 分配/回收都是纯用户态 O(1)，不走 malloc/free。
		 */
		struct MpscNodePool
		{
			static constexpr size_t kMaxFreeNodes = 128;

			struct FreeNode
			{
				FreeNode* next;
			};

			FreeNode* head = nullptr;
			size_t count = 0;

			~MpscNodePool()
			{
#ifndef __MINGW32__
				// MinGW 下 thread_local 析构在 DLL TLS 回调里跑，那时 CRT 堆
				// 可能已销毁，delete 下去就崩。进程退出 OS 会帮我们收。
				while (head)
				{
					auto* n = head->next;
					::operator delete(head);
					head = n;
				}
#endif
			}
		};

		static MpscNodePool& nodePool()
		{
			thread_local MpscNodePool pool;
			return pool;
		}

		/// 从对象池拿一个 MpscNode（free list 命中时零 malloc）
		static MpscNode* allocateNode(WriteEntry entry)
		{
			static_assert(sizeof(MpscNode) >= sizeof(void*), "MpscNode must be large enough to overlay FreeNode");
			auto& pool = nodePool();
			void* mem = nullptr;
			if (pool.head)
			{
				mem = pool.head;
				pool.head = pool.head->next;
				--pool.count;
			}
			else
			{
				mem = ::operator new(sizeof(MpscNode));
			}
			return ::new (mem) MpscNode(std::move(entry));
		}

		/// 还回对象池（池没满就不 free）
		static void deallocateNode(MpscNode* node)
		{
			node->~MpscNode();
			auto& pool = nodePool();
			if (pool.count >= MpscNodePool::kMaxFreeNodes)
			{
				::operator delete(node);
				return;
			}
			auto* fn = reinterpret_cast<typename MpscNodePool::FreeNode*>(node);
			fn->next = pool.head;
			pool.head = fn;
			++pool.count;
		}

		/**
		 * @brief Vyukov Intrusive MPSC Queue
		 * 生产者 wait-free push，消费者 single-thread pop。
		 */
		struct MpscQueue
		{
			MpscNode stub;                            // 哨兵节点（嵌入，零额外分配）
			MpscNode* head_;                          // 消费者独占，无竞争
			alignas(64) std::atomic<MpscNode*> tail_; // 生产者竞争热点，独占缓存行

			MpscQueue() : head_(&stub), tail_(&stub)
			{
			}

			// 不可拷贝/移动
			MpscQueue(const MpscQueue&) = delete;
			MpscQueue& operator=(const MpscQueue&) = delete;

			/// Wait-free O(1) push
			void push(MpscNode* node)
			{
				node->next.store(nullptr, std::memory_order_relaxed);
				MpscNode* prev = tail_.exchange(node, std::memory_order_acq_rel);
				prev->next.store(node, std::memory_order_release);
			}

			/// Single-consumer pop，返回 nullptr 表示空（或瞬态不一致）
			MpscNode* pop()
			{
				MpscNode* h = head_;
				MpscNode* next = h->next.load(std::memory_order_acquire);

				if (h == &stub)
				{
					if (!next)
					{
						return nullptr;
					}
					head_ = next;
					h = next;
					next = h->next.load(std::memory_order_acquire);
				}

				if (next)
				{
					head_ = next;
					return h;
				}

				MpscNode* t = tail_.load(std::memory_order_acquire);
				if (h != t)
				{
					return nullptr; // 生产者正在 push，next 尚未链接
				}

				push(&stub); // 重新插入哨兵
				next = h->next.load(std::memory_order_acquire);
				if (next)
				{
					head_ = next;
					return h;
				}
				return nullptr;
			}
		};

		// 协程式文件发送（writeLoop 内部调用）
		boost::asio::awaitable<size_t> sendFileNode(const FileWriteNode& node);

		/// writeLoop 单轮最多 drain 这么多节点，别让一个连接把 CPU 全占了
		static constexpr size_t kMaxDrainBatch = 256;

		// --- 热路径字段 ---
		// socket/state/flags 紧挨着放，尽量一条 cache line 搞定
		AsioEventLoop* loop_;
		SocketType socket_;
		std::atomic<State> state_ {State::hConnecting};
		std::atomic<bool> reading_ {false};
		std::atomic<bool> writing_ {false};

		// 接收缓冲区（readLoop 第一次读时才分配，空闲连接不占这块内存）
		std::optional<PmrBuffer> inputBuffer_;

		// 发送队列（MPSC 无锁队列）
		MpscQueue writeQueue_;
		std::atomic<size_t> queuedBytes_ {0}; // 队列总字节数，高水位线用

		// --- 统计字段（跨线程读，alignas 防 false sharing）---
		alignas(64) std::atomic<int64_t> lastActiveTimeMs_ {
			std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
				.count()};
		std::atomic<size_t> bytesSent_ {0};
		std::atomic<size_t> bytesReceived_ {0};

		// --- 冷路径字段 ---
		InetAddress localAddr_;
		InetAddress peerAddr_;

		// 回调（建连前设好就不动了）
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
