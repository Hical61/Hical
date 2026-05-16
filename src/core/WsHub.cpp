#include "WsHub.h"
#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace hical
{

	WsConnectionId WsHub::add(std::shared_ptr<WebSocketSession> session)
	{
		WsConnectionId id = m_nextId.fetch_add(1, std::memory_order_relaxed);

		std::unique_lock lock(m_mutex);
		m_connections.emplace(id, ConnectionEntry {std::move(session), {}});
		return id;
	}

	void WsHub::remove(WsConnectionId id)
	{
		std::unique_lock lock(m_mutex);

		auto it = m_connections.find(id);
		if (it == m_connections.end())
		{
			return;
		}

		// 从所有所属房间中移除该连接
		for (const auto& room : it->second.rooms)
		{
			auto roomIt = m_rooms.find(room);
			if (roomIt != m_rooms.end())
			{
				auto& members = roomIt->second;
				std::erase_if(members,
							  [id](const RoomMember& m)
							  {
								  return m.id == id;
							  });
				if (members.empty())
				{
					m_rooms.erase(roomIt);
				}
			}
		}

		m_connections.erase(it);
	}

	void WsHub::join(WsConnectionId id, const std::string& room)
	{
		std::unique_lock lock(m_mutex);

		auto it = m_connections.find(id);
		if (it == m_connections.end())
		{
			return;
		}

		it->second.rooms.insert(room);
		m_rooms[room].push_back(RoomMember {id, it->second.session});
	}

	void WsHub::leave(WsConnectionId id, const std::string& room)
	{
		std::unique_lock lock(m_mutex);

		auto connIt = m_connections.find(id);
		if (connIt != m_connections.end())
		{
			connIt->second.rooms.erase(room);
		}

		auto roomIt = m_rooms.find(room);
		if (roomIt != m_rooms.end())
		{
			auto& members = roomIt->second;
			std::erase_if(members,
						  [id](const RoomMember& m)
						  {
							  return m.id == id;
						  });
			if (members.empty())
			{
				m_rooms.erase(roomIt);
			}
		}
	}

	void WsHub::broadcast(const std::string& room, std::string_view message, WsConnectionId exclude)
	{
		auto msgPtr = std::make_shared<std::string>(message);

		std::shared_lock lock(m_mutex);

		auto roomIt = m_rooms.find(room);
		if (roomIt == m_rooms.end())
		{
			return;
		}

		// 直接遍历 vector<RoomMember>：连续内存顺序访问（cache prefetch 友好），
		// 无需 m_connections.find() 二次查找，减少一层指针追踪
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

	void WsHub::broadcastBinary(const std::string& room, std::string_view data, WsConnectionId exclude)
	{
		auto dataPtr = std::make_shared<std::string>(data);

		std::shared_lock lock(m_mutex);

		auto roomIt = m_rooms.find(room);
		if (roomIt == m_rooms.end())
		{
			return;
		}

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
						[sp, dataPtr]() -> Awaitable<void>
						{
							if (sp->isOpen())
							{
								co_await sp->sendBinary(*dataPtr);
							}
						});
			}
		}
	}

	void WsHub::broadcastAll(std::string_view message, WsConnectionId exclude)
	{
		auto msgPtr = std::make_shared<std::string>(message);

		std::shared_lock lock(m_mutex);

		for (const auto& [id, entry] : m_connections)
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
		auto msgPtr = std::make_shared<std::string>(message);

		std::shared_lock lock(m_mutex);

		auto it = m_connections.find(id);
		if (it == m_connections.end())
		{
			return;
		}

		auto sp = it->second.session.lock();
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

	size_t WsHub::roomSize(const std::string& room) const
	{
		std::shared_lock lock(m_mutex);

		auto it = m_rooms.find(room);
		if (it == m_rooms.end())
		{
			return 0;
		}
		return it->second.size();
	}

	size_t WsHub::connectionCount() const
	{
		std::shared_lock lock(m_mutex);
		return m_connections.size();
	}

} // namespace hical
