/**
 * @file WsHub.cpp
 * @brief WebSocket 频道广播实现
 */

#include "WsHub.h"
#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace hical
{

	WsConnectionId WsHub::add(std::shared_ptr<WebSocketSession> session)
	{
		WsConnectionId id = nextId_.fetch_add(1, std::memory_order_relaxed);

		std::unique_lock lock(mutex_);
		connections_.emplace(id, ConnectionEntry {std::move(session), {}});
		return id;
	}

	void WsHub::remove(WsConnectionId id)
	{
		std::unique_lock lock(mutex_);

		if (auto it = connections_.find(id); it == connections_.end())
		{
			return;
		}
		else
		{
			// 从所有所属房间中移除该连接
			for (const auto& room : it->second.rooms)
			{
				if (auto roomIt = rooms_.find(room); roomIt != rooms_.end())
				{
					auto& members = roomIt->second;
					std::erase_if(members,
								  [id](const RoomMember& m)
								  {
									  return m.id == id;
								  });
					if (members.empty())
					{
						rooms_.erase(roomIt);
					}
				}
			}

			connections_.erase(it);
		}
	}

	void WsHub::join(WsConnectionId id, std::string_view room)
	{
		std::unique_lock lock(mutex_);

		if (auto it = connections_.find(id); it == connections_.end())
		{
			return;
		}
		else
		{
			it->second.rooms.emplace(room);
			rooms_[std::string(room)].push_back(RoomMember {id, it->second.session});
		}
	}

	void WsHub::leave(WsConnectionId id, std::string_view room)
	{
		std::unique_lock lock(mutex_);

		if (auto connIt = connections_.find(id); connIt != connections_.end())
		{
			// C++20 异构查找仅支持 find/count/contains，erase 需 C++23 (P2077R3)
			if (auto roomIt2 = connIt->second.rooms.find(room); roomIt2 != connIt->second.rooms.end())
			{
				connIt->second.rooms.erase(roomIt2);
			}
		}

		if (auto roomIt = rooms_.find(room); roomIt != rooms_.end())
		{
			auto& members = roomIt->second;
			std::erase_if(members,
						  [id](const RoomMember& m)
						  {
							  return m.id == id;
						  });
			if (members.empty())
			{
				rooms_.erase(roomIt);
			}
		}
	}

	void WsHub::broadcastImpl(std::string_view room, std::string_view payload, WsConnectionId exclude, bool isBinary)
	{
		auto msgPtr = std::make_shared<std::string>(payload);

		std::shared_lock lock(mutex_);

		if (auto roomIt = rooms_.find(room); roomIt == rooms_.end())
		{
			return;
		}
		else
		{
			// 直接遍历 vector<RoomMember>：连续内存顺序访问（cache prefetch 友好），
			// 无需 connections_.find() 二次查找，减少一层指针追踪
			for (const auto& member : roomIt->second)
			{
				if (member.id == exclude)
				{
					continue;
				}

				auto sp = member.session.lock();
				if (sp && sp->isOpen())
				{
					coSpawn(sp->socket().get_executor(),
							[sp, msgPtr, isBinary]() -> Awaitable<void>
							{
								if (sp->isOpen())
								{
									if (isBinary)
									{
										co_await sp->sendBinary(*msgPtr);
									}
									else
									{
										co_await sp->send(*msgPtr);
									}
								}
							});
				}
			}
		}
	}

	void WsHub::broadcast(std::string_view room, std::string_view message, WsConnectionId exclude)
	{
		broadcastImpl(room, message, exclude, false);
	}

	void WsHub::broadcastBinary(std::string_view room, std::string_view data, WsConnectionId exclude)
	{
		broadcastImpl(room, data, exclude, true);
	}

	void WsHub::broadcastAll(std::string_view message, WsConnectionId exclude)
	{
		auto msgPtr = std::make_shared<std::string>(message);

		std::shared_lock lock(mutex_);

		for (const auto& [id, entry] : connections_)
		{
			if (id == exclude)
			{
				continue;
			}

			auto sp = entry.session.lock();
			if (sp && sp->isOpen())
			{
				coSpawn(sp->socket().get_executor(),
						[sp, msgPtr]() -> Awaitable<void>
						{
							if (sp->isOpen())
							{
								co_await sp->send(*msgPtr);
							}
						});
			}
		}
	}

	void WsHub::sendTo(WsConnectionId id, std::string_view message)
	{
		// 单目标发送：直接 move string 进 lambda，省去 shared_ptr 控制块分配
		auto msg = std::string(message);

		std::shared_lock lock(mutex_);

		if (auto it = connections_.find(id); it == connections_.end())
		{
			return;
		}
		else
		{
			auto sp = it->second.session.lock();
			if (sp && sp->isOpen())
			{
				coSpawn(sp->socket().get_executor(),
						[sp, msg = std::move(msg)]() -> Awaitable<void>
						{
							if (sp->isOpen())
							{
								co_await sp->send(msg);
							}
						});
			}
		}
	}

	size_t WsHub::roomSize(std::string_view room) const
	{
		std::shared_lock lock(mutex_);

		if (auto it = rooms_.find(room); it != rooms_.end())
		{
			return it->second.size();
		}
		return 0;
	}

	size_t WsHub::connectionCount() const
	{
		std::shared_lock lock(mutex_);
		return connections_.size();
	}

} // namespace hical
