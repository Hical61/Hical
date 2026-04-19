#include "Session.h"
#include <openssl/rand.h>
#include <stdexcept>

namespace hical
{

	/// 秒转毫秒的转换因子
	static constexpr int64_t kMsPerSecond = 1000LL;

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
		if (opts_.maxAge > 0 && elapsedMs >= static_cast<long long>(opts_.maxAge) * kMsPerSecond)
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
			if (sinceGcMs >= static_cast<long long>(opts_.gcInterval) * kMsPerSecond)
			{
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
		}

		// Session 数量上限检查：防止攻击者创建无限 Session 导致 OOM
		if (opts_.maxSessions > 0 && store_.size() >= opts_.maxSessions)
		{
			return nullptr;
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
		std::lock_guard<std::mutex> lock(mutex_);
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
