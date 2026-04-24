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
		auto it = store_.find(id);
		if (it == store_.end())
		{
			return nullptr;
		}

		// 检查是否已过期（使用毫秒精度，避免秒级截断的边界问题）
		auto now = std::chrono::steady_clock::now();
		auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - it->second->lastAccess()).count();
		if (opts_.maxAge > 0 && elapsedMs >= static_cast<long long>(opts_.maxAge) * kMsPerSecond)
		{
			// 过期 Session：直接返回 nullptr，不在读锁路径上做删除。
			// 过期条目由 gc() 懒清理（create() 定期触发 + maxSessions 满时强制触发），
			// 避免 shared_lock→unlock→unique_lock 锁升级的 TOCTOU 窗口。
			return nullptr;
		}

		return it->second;
	}

	std::shared_ptr<Session> SessionManager::create()
	{
		// 懒 GC：先检查是否需要清理（读锁检查时间，写锁执行清理）
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
				gc(false); // 懒 GC：内部有双重检查，避免多线程重复空跑
			}
		}

		std::unique_lock<std::shared_mutex> lock(mutex_);

		// Session 数量上限检查：防止攻击者创建无限 Session 导致 OOM
		if (opts_.maxSessions > 0 && store_.size() >= opts_.maxSessions)
		{
			// find() 不再即时删除过期条目（由 gc() 懒清理），可能存在大量过期幽灵条目
			// 占据 store_ 槽位导致提前触达上限。在拒绝前先强制清理一轮过期条目再重新检查。
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
		auto it = store_.find(oldId);
		if (it == store_.end())
		{
			return nullptr;
		}

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
			// 双重检查：获取写锁后再次确认是否需要 GC，
			// 避免多线程同时判定 needGc=true 后串行执行多次空跑 GC
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
		// 使用 OpenSSL 密码学安全随机数生成 128 位 Session ID
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
