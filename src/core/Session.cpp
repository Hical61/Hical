/**
 * @file Session.cpp
 * @brief 内存 Session 管理实现
 */

#include "Session.h"
#include <openssl/rand.h>
#include <stdexcept>

namespace hical
{

	/// 秒转毫秒的转换因子
	static constexpr int64_t kMsPerSecond = 1000LL;

	std::shared_ptr<Session> SessionManager::find(const std::string& id)
	{
		std::shared_lock<std::shared_mutex> lock(mutex_);
		if (auto it = store_.find(id); it == store_.end())
		{
			return nullptr;
		}
		else
		{
			// 过期检查
			auto now = std::chrono::steady_clock::now();
			auto elapsedMs =
				std::chrono::duration_cast<std::chrono::milliseconds>(now - it->second->lastAccess()).count();
			if (opts_.maxAge > 0 && elapsedMs >= static_cast<long long>(opts_.maxAge) * kMsPerSecond)
			{
				// 过期了，返回 nullptr，交给 gc() 去清
				return nullptr;
			}

			return it->second;
		}
	}

	std::shared_ptr<Session> SessionManager::create()
	{
		// 该 GC 了就先清一波
		if (opts_.gcInterval > 0)
		{
			bool needGc = false;
			{
				std::shared_lock<std::shared_mutex> readLock(mutex_);
				auto now = std::chrono::steady_clock::now();
				auto sinceGcMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastGc_).count();
				needGc = sinceGcMs >= static_cast<long long>(opts_.gcInterval) * kMsPerSecond;
			}
			if (needGc)
			{
				gc(false); // 内部有双重检查
			}
		}

		std::unique_lock<std::shared_mutex> lock(mutex_);

		// Session 数量上限
		if (opts_.maxSessions > 0 && store_.size() >= opts_.maxSessions)
		{
			// 可能有过期幽灵条目占位，先强制 gc 再判断
			lock.unlock();
			gc(true);
			lock.lock();
			if (store_.size() >= opts_.maxSessions)
			{
				return nullptr;
			}
		}

		auto id = generateId();
		// 极低概率碰撞保护
		while (store_.count(id))
		{
			id = generateId();
		}
		auto session = std::make_shared<Session>(id);
		store_[id] = session;
		return session;
	}

	std::shared_ptr<Session> SessionManager::regenerate(const std::string& oldId)
	{
		std::unique_lock<std::shared_mutex> lock(mutex_);
		if (auto it = store_.find(oldId); it == store_.end())
		{
			return nullptr;
		}
		else
		{
			auto oldSession = it->second;
			store_.erase(it);

			auto newId = generateId();
			while (store_.count(newId))
			{
				newId = generateId();
			}

			auto newSession = std::make_shared<Session>(newId);
			newSession->migrateFrom(*oldSession);
			store_[newId] = newSession;
			return newSession;
		}
	}

	void SessionManager::destroy(const std::string& id)
	{
		std::unique_lock<std::shared_mutex> lock(mutex_);
		store_.erase(id);
	}

	void SessionManager::gc(bool force)
	{
		std::unique_lock<std::shared_mutex> lock(mutex_);

		auto now = std::chrono::steady_clock::now();
		if (!force)
		{
			// 双重检查，锁里再确认一次
			if (opts_.gcInterval > 0)
			{
				auto sinceGcMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastGc_).count();
				if (sinceGcMs < static_cast<long long>(opts_.gcInterval) * kMsPerSecond)
				{
					return;
				}
			}
		}
		lastGc_ = now;

		for (auto it = store_.begin(); it != store_.end();)
		{
			auto elapsedMs =
				std::chrono::duration_cast<std::chrono::milliseconds>(now - it->second->lastAccess()).count();
			if (opts_.maxAge > 0 && elapsedMs >= static_cast<long long>(opts_.maxAge) * kMsPerSecond)
			{
				it = store_.erase(it);
			}
			else
			{
				++it;
			}
		}
	}

	size_t SessionManager::count()
	{
		std::shared_lock<std::shared_mutex> lock(mutex_);
		return store_.size();
	}

	std::string SessionManager::generateId()
	{
		// CSPRNG 生成 128-bit ID
		unsigned char buf[16];
		if (RAND_bytes(buf, sizeof(buf)) != 1)
		{
			throw std::runtime_error("SessionManager::generateId: RAND_bytes failed");
		}

		static constexpr char kHex[] = "0123456789abcdef";
		std::string result(32, '\0');
		for (size_t i = 0; i < 16; ++i)
		{
			result[i * 2] = kHex[buf[i] >> 4];
			result[i * 2 + 1] = kHex[buf[i] & 0x0f];
		}
		return result;
	}

} // namespace hical
