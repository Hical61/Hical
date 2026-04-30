#ifdef HICAL_HAS_DATABASE

	#include "StmtCache.h"

namespace hical::db
{

	StmtCache::StmtCache(size_t maxSize) : m_maxSize(maxSize)
	{
	}

	boost::mysql::statement* StmtCache::find(std::string_view sql)
	{
		if (m_maxSize == 0)
		{
			return nullptr;
		}

		auto it = m_map.find(sql);
		if (it == m_map.end())
		{
			return nullptr;
		}

		// 提升到 MRU（链表头部）
		m_lruList.splice(m_lruList.begin(), m_lruList, it->second);
		return &(it->second->second);
	}

	std::optional<boost::mysql::statement> StmtCache::insert(const std::string& sql, boost::mysql::statement stmt)
	{
		if (m_maxSize == 0)
		{
			// 缓存禁用，直接返回传入的 statement 让调用方关闭
			return stmt;
		}

		std::optional<boost::mysql::statement> evicted;

		// 已存在 → 更新并提升
		auto it = m_map.find(sql);
		if (it != m_map.end())
		{
			it->second->second = std::move(stmt);
			m_lruList.splice(m_lruList.begin(), m_lruList, it->second);
			return evicted; // nullopt
		}

		// 缓存已满 → 淘汰 LRU（链表尾部）
		if (m_lruList.size() >= m_maxSize)
		{
			auto& back = m_lruList.back();
			evicted = std::move(back.second);
			m_map.erase(back.first);
			m_lruList.pop_back();
		}

		// 插入新条目到 MRU 位置
		m_lruList.emplace_front(sql, std::move(stmt));
		m_map[sql] = m_lruList.begin();

		return evicted;
	}

	std::optional<boost::mysql::statement> StmtCache::erase(std::string_view sql)
	{
		auto it = m_map.find(sql);
		if (it == m_map.end())
		{
			return std::nullopt;
		}

		auto stmt = std::move(it->second->second);
		m_lruList.erase(it->second);
		m_map.erase(it);
		return stmt;
	}

	std::vector<boost::mysql::statement> StmtCache::clear()
	{
		std::vector<boost::mysql::statement> stmts;
		stmts.reserve(m_lruList.size());
		for (auto& entry : m_lruList)
		{
			stmts.push_back(std::move(entry.second));
		}
		m_lruList.clear();
		m_map.clear();
		return stmts;
	}

	size_t StmtCache::size() const
	{
		return m_lruList.size();
	}

	size_t StmtCache::maxSize() const
	{
		return m_maxSize;
	}

} // namespace hical::db

#endif // HICAL_HAS_DATABASE
