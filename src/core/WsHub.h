/**
 * @file WsHub.h
 * @brief WebSocket 频道订阅与广播中心
 */

#pragma once

#include "Coroutine.h"
#include "HttpTypes.h"
#include "WebSocket.h"
#include <atomic>
#include <cstdint>
#include <memory>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace hical
{

	using WsConnectionId = uint64_t;

	/**
	 * @brief WebSocket 连接管理器 / 广播 Hub
	 * 线程安全：内部使用 shared_mutex 保护连接注册表。
	 * 广播操作通过 coSpawn 到各连接所属 executor 实现跨线程安全写入。
	 * 连接存储使用 weak_ptr：不延长 WebSocketSession 生命周期。
	 * 注意：断开的连接不会自动清理，用户必须在 onDisconnect 回调中调用 remove(id)。
	 * 未 remove 的 dead entries 在广播时通过 weak_ptr::lock() 跳过，但不会被删除。
	 */
	class WsHub
	{
	public:
		WsHub() = default;
		~WsHub() = default;

		WsHub(const WsHub&) = delete;
		WsHub& operator=(const WsHub&) = delete;

		/**
		 * @brief 注册连接到 Hub
		 * @param session WebSocket 会话（shared_ptr，Hub 存储 weak_ptr）
		 * @return 连接 ID（全局唯一，原子递增）
		 */
		WsConnectionId add(std::shared_ptr<WebSocketSession> session);

		/**
		 * @brief 移除连接（自动离开所有房间）
		 * @param id 连接 ID
		 */
		void remove(WsConnectionId id);

		/**
		 * @brief 将连接加入房间
		 * @param id   连接 ID
		 * @param room 房间名
		 */
		void join(WsConnectionId id, std::string_view room);

		/**
		 * @brief 将连接离开房间
		 * @param id   连接 ID
		 * @param room 房间名
		 */
		void leave(WsConnectionId id, std::string_view room);

		/**
		 * @brief 广播文本消息到房间
		 * @param room    房间名
		 * @param message 消息内容
		 * @param exclude 排除的连接 ID（0 = 不排除）
		 */
		void broadcast(std::string_view room, std::string_view message, WsConnectionId exclude = 0);

		/**
		 * @brief 广播二进制消息到房间
		 * @param room    房间名
		 * @param data    二进制数据
		 * @param exclude 排除的连接 ID（0 = 不排除）
		 */
		void broadcastBinary(std::string_view room, std::string_view data, WsConnectionId exclude = 0);

		/**
		 * @brief 广播文本消息到所有已注册连接
		 * @param message 消息内容
		 * @param exclude 排除的连接 ID（0 = 不排除）
		 */
		void broadcastAll(std::string_view message, WsConnectionId exclude = 0);

		/**
		 * @brief 发送文本消息到指定连接
		 * @param id      连接 ID
		 * @param message 消息内容
		 */
		void sendTo(WsConnectionId id, std::string_view message);

		/**
		 * @brief 获取房间内连接数
		 * @param room 房间名
		 * @return 当前房间内的连接数量
		 */
		size_t roomSize(std::string_view room) const;

		/**
		 * @brief 获取总连接数
		 * @return 已注册的连接数量
		 */
		size_t connectionCount() const;

	private:
		struct ConnectionEntry
		{
			std::weak_ptr<WebSocketSession> session;
			std::unordered_set<std::string, StringHash, StringEqual> rooms; ///< 透明哈希，string_view 查找零临时构造
		};

		/**
		 * @brief 房间成员条目（缓存行优化）
		 * 广播时直接遍历 vector<RoomMember>（连续内存，cache prefetch 友好），
		 * 通过冗余存储 weak_ptr 消除原来的 connections_.find(id) 指针追踪。
		 */
		struct RoomMember
		{
			WsConnectionId id;
			std::weak_ptr<WebSocketSession> session;
		};

		/// 广播实现内核（broadcast/broadcastBinary 共用）
		/// @param isBinary true=sendBinary, false=send
		void broadcastImpl(std::string_view room, std::string_view payload, WsConnectionId exclude, bool isBinary);

		mutable std::shared_mutex mutex_;
		std::unordered_map<WsConnectionId, ConnectionEntry> connections_;
		std::unordered_map<std::string, std::vector<RoomMember>, StringHash, StringEqual> rooms_; ///< 透明哈希
		std::atomic<WsConnectionId> nextId_ {1};
	};

} // namespace hical
