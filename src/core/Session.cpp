#include "Session.h"
#include <iomanip>
#include <sstream>

namespace hical
{

	std::shared_ptr<Session> SessionManager::find(const std::string& id)
	{
		std::lock_guard<std::mutex> lock(mutex_);
		auto it = store_.find(id);
		if (it == store_.end())
		{
			return nullptr;
		}

		// 检查是否已过期（使用毫秒精度，避免秒级截断的边界问题）
		auto now = std::chrono::steady_clock::now();
		auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - it->second->lastAccess()).count();
		if (opts_.maxAge > 0 && elapsedMs >= static_cast<long long>(opts_.maxAge) * 1000LL)
		{
			store_.erase(it);
			return nullptr;
		}

		return it->second;
	}

	std::shared_ptr<Session> SessionManager::create()
	{
		std::lock_guard<std::mutex> lock(mutex_);

		// 懒 GC：每隔 gcInterval 秒在 create() 时顺带清理过期 Session
		if (opts_.gcInterval > 0)
		{
			auto now = std::chrono::steady_clock::now();
			auto sinceGcMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastGc_).count();
			if (sinceGcMs >= static_cast<long long>(opts_.gcInterval) * 1000LL)
			{
				lastGc_ = now;
				for (auto it = store_.begin(); it != store_.end();)
				{
					auto elapsedMs =
						std::chrono::duration_cast<std::chrono::milliseconds>(now - it->second->lastAccess()).count();
					if (opts_.maxAge > 0 && elapsedMs >= static_cast<long long>(opts_.maxAge) * 1000LL)
					{
						it = store_.erase(it);
					}
					else
					{
						++it;
					}
				}
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

	void SessionManager::destroy(const std::string& id)
	{
		std::lock_guard<std::mutex> lock(mutex_);
		store_.erase(id);
	}

	void SessionManager::gc()
	{
		std::lock_guard<std::mutex> lock(mutex_);
		auto now = std::chrono::steady_clock::now();
		for (auto it = store_.begin(); it != store_.end();)
		{
			auto elapsedMs =
				std::chrono::duration_cast<std::chrono::milliseconds>(now - it->second->lastAccess()).count();
			if (opts_.maxAge > 0 && elapsedMs >= static_cast<long long>(opts_.maxAge) * 1000LL)
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
		std::lock_guard<std::mutex> lock(mutex_);
		return store_.size();
	}

	std::string SessionManager::generateId()
	{
		// 使用 thread_local 随机引擎，避免加锁
		thread_local std::mt19937_64 rng(std::random_device {}());
		std::uniform_int_distribution<uint64_t> dist;

		// 生成两个 64 位随机数拼成 128 位 ID
		uint64_t hi = dist(rng);
		uint64_t lo = dist(rng);

		std::ostringstream oss;
		oss << std::hex << std::setfill('0') << std::setw(16) << hi << std::setw(16) << lo;
		return oss.str();
	}

} // namespace hical
