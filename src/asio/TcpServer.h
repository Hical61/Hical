/**
 * @file TcpServer.h
 * @brief TCP 接受器与连接生命周期管理
 */

#pragma once

#include "../core/TcpConnection.h"
#include "../core/InetAddress.h"
#include "../core/Coroutine.h"
#include "../core/SslContext.h"
#include "../core/IdleFd.h"
#include "AsioEventLoop.h"
#include "EventLoopPool.h"
#include <boost/asio.hpp>
#include <atomic>
#include <functional>
#include <memory>
#include <unordered_set>
#include <string>
#include <vector>

namespace hical
{

	/**
	 * @brief per-loop 连接分片
	 * 每个 EventLoop 维护自己的连接集合，所有操作在 loop 线程内完成，无需互斥锁。
	 */
	struct LoopShard
	{
		AsioEventLoop* loop = nullptr;
		// 仅由本 loop 线程访问，无需锁
		std::unordered_set<TcpConnection::Ptr> connections;
	};

	/**
	 * @brief TCP 服务器
	 * 管理连接的监听、接受和分发。
	 * 支持多线程 IO（通过 EventLoopPool）。
	 * 支持 SSL/TLS 加密连接。
	 * 采用协程式 accept 循环。
	 * 连接表 per-loop 分片：每个 EventLoop 维护独立的连接集合，idle 扫描无需全局锁。
	 */
	class TcpServer
	{
	public:
		using NewConnectionCallback = std::function<void(const TcpConnection::Ptr&)>;

		/**
		 * @brief 构造 TCP 服务器
		 * @param baseLoop 主事件循环（用于 accept）
		 * @param listenAddr 监听地址
		 * @param name 服务器名称
		 */
		TcpServer(AsioEventLoop* baseLoop, const InetAddress& listenAddr, const std::string& name);

		~TcpServer();

		/**
		 * @brief 设置 IO 线程数
		 * @param num 线程数（0 表示使用 baseLoop 处理 IO）
		 * @note 必须在 start() 之前调用
		 */
		void setIoLoopNum(size_t num);

		/**
		 * @brief 启动服务器
		 */
		void start();

		/**
		 * @brief 停止服务器（优雅关闭）
		 */
		void stop();

		/**
		 * @brief 设置新连接回调
		 * @param cb 当新连接建立时调用
		 */
		void onNewConnection(NewConnectionCallback cb);

		/**
		 * @brief 设置消息接收回调
		 * @param cb 当收到数据时调用
		 */
		void onMessage(TcpConnection::MessageCallback cb);

		/**
		 * @brief 设置连接关闭回调
		 * @param cb 当连接关闭时调用
		 */
		void onClose(TcpConnection::CloseCallback cb);

		/**
		 * @brief 启用 SSL/TLS
		 * @param ctx SSL 上下文
		 */
		void enableSsl(std::shared_ptr<SslContext> ctx);

		/**
		 * @brief 获取服务器名称
		 * @return 名称
		 */
		[[nodiscard]] const std::string& name() const;

		/**
		 * @brief 获取监听地址
		 * @return 地址
		 */
		[[nodiscard]] const InetAddress& listenAddr() const;

		/**
		 * @brief 获取当前连接数
		 * @return 连接数
		 */
		[[nodiscard]] size_t connectionCount() const;

		/**
		 * @brief 服务器是否已启动
		 * @return true 如果已启动
		 */
		[[nodiscard]] bool isRunning() const;

		/**
		 * @brief 设置空闲连接超时时间
		 * @param seconds 超时秒数（0 表示不检查，默认不检查）
		 * @note 必须在 start() 之前调用
		 */
		void setIdleTimeout(double seconds);

		// 禁止拷贝
		TcpServer(const TcpServer&) = delete;
		TcpServer& operator=(const TcpServer&) = delete;

	private:
		// 协程式 accept 循环
		Awaitable<void> acceptLoop();

		// 空闲连接超时扫描协程（per-shard，运行在对应 loop 线程上）
		// 使用指针而非引用：协程帧存储参数副本，引用参数会被 clang-tidy 标记
		Awaitable<void> idleCheckLoop(LoopShard* shard);

		// 获取下一个 IO 事件循环
		AsioEventLoop* getNextIoLoop();

		// 连接管理（per-shard，无锁，必须在 loop 线程内调用）
		void addConnection(LoopShard& shard, const TcpConnection::Ptr& conn);
		void removeConnection(LoopShard& shard, const TcpConnection::Ptr& conn);

		// 查找 loop 对应的 shard
		LoopShard& findShard(AsioEventLoop* loop);

		AsioEventLoop* baseLoop_;
		InetAddress listenAddr_;
		std::string name_;

		boost::asio::ip::tcp::acceptor acceptor_;
		std::atomic<bool> running_ {false};

		// IO 线程池
		size_t ioLoopNum_ {0};
		std::unique_ptr<EventLoopPool> ioPool_;

		// per-loop 连接分片（start() 时初始化，之后结构只读）
		std::vector<LoopShard> shards_;
		std::atomic<size_t> totalConnections_ {0}; // 全局连接计数（原子操作）

		// 回调
		NewConnectionCallback newConnectionCallback_;
		TcpConnection::MessageCallback messageCallback_;
		TcpConnection::CloseCallback closeCallback_;

		// SSL
		std::shared_ptr<SslContext> sslCtx_;

		// 空闲连接超时（秒，0 表示不检查）
		double idleTimeout_ {0.0};

		// 预留 fd，EMFILE 时有的用
		IdleFd idleFd_;

		// 析构后置 false，回调里靠它判断 this 还在不在
		std::shared_ptr<std::atomic<bool>> alive_;
	};

} // namespace hical
