/**
 * @file PgStmtCache.cpp
 * @brief libpq 预处理语句 LRU 缓存实现
 */

#ifdef HICAL_HAS_PGSQL

	#include "PgStmtCache.h"

namespace hical::db
{

	PgStmtCache::PgStmtCache(size_t maxSize) : maxSize_(maxSize)
	{
	}

	std::optional<std::string> PgStmtCache::find(std::string_view sql)
	{
		// 透明哈希：string_view 直接查，避免临时 std::string 堆分配
		auto it = map_.find(sql);
		if (it == map_.end())
		{
			return std::nullopt;
		}

		// 命中：提升到 MRU（链表 front）
		lruList_.splice(lruList_.begin(), lruList_, it->second);
		return it->second->second;
	}

	void PgStmtCache::insert(std::string sql, std::string stmtName)
	{
		// 已存在：更新名字并提升到 MRU
		auto it = map_.find(sql);
		if (it != map_.end())
		{
			it->second->second = std::move(stmtName);
			lruList_.splice(lruList_.begin(), lruList_, it->second);
			return;
		}

		// 缓存禁用直接返回
		if (maxSize_ == 0)
		{
			return;
		}

		// 缓存已满：淘汰 LRU（链表 back）
		if (lruList_.size() >= maxSize_)
		{
			auto& back = lruList_.back();
			map_.erase(back.first);
			lruList_.pop_back();
		}

		// 插入到 MRU（sql 作为 key，stmtName 作为 value）
		lruList_.emplace_front(std::move(sql), std::move(stmtName));
		map_.emplace(lruList_.front().first, lruList_.begin());
	}

	void PgStmtCache::erase(std::string_view sql)
	{
		auto it = map_.find(sql);
		if (it == map_.end())
		{
			return;
		}
		lruList_.erase(it->second);
		map_.erase(it);
	}

	void PgStmtCache::clear()
	{
		map_.clear();
		lruList_.clear();
	}

	size_t PgStmtCache::size() const
	{
		return lruList_.size();
	}

	size_t PgStmtCache::maxSize() const
	{
		return maxSize_;
	}

} // namespace hical::db

#endif // HICAL_HAS_PGSQL
