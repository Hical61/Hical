/**
 * @file StmtCache.cpp
 * @brief 语句缓存实现
 */

#ifdef HICAL_HAS_DATABASE

	#include "StmtCache.h"

namespace hical::db
{

	StmtCache::StmtCache(size_t maxSize) : maxSize_(maxSize)
	{
	}

	boost::mysql::statement* StmtCache::find(std::string_view sql)
	{
		if (maxSize_ == 0)
		{
			return nullptr;
		}

		if (auto it = map_.find(sql); it == map_.end())
		{
			return nullptr;
		}
		else
		{
			// 提升到 MRU（链表头部）
			lruList_.splice(lruList_.begin(), lruList_, it->second);
			return &(it->second->second);
		}
	}

	std::optional<boost::mysql::statement> StmtCache::insert(const std::string& sql, boost::mysql::statement stmt)
	{
		if (maxSize_ == 0)
		{
			// 缓存禁用，直接返回传入的 statement 让调用方关闭
			return stmt;
		}

		std::optional<boost::mysql::statement> evicted;

		// 已存在 → 更新并提升
		if (auto it = map_.find(sql); it != map_.end())
		{
			it->second->second = std::move(stmt);
			lruList_.splice(lruList_.begin(), lruList_, it->second);
			return evicted; // nullopt
		}

		// 缓存已满 → 淘汰 LRU（链表尾部）
		if (lruList_.size() >= maxSize_)
		{
			auto& back = lruList_.back();
			evicted = std::move(back.second);
			map_.erase(back.first);
			lruList_.pop_back();
		}

		// 插入新条目到 MRU 位置
		lruList_.emplace_front(sql, std::move(stmt));
		map_[sql] = lruList_.begin();

		return evicted;
	}

	std::optional<boost::mysql::statement> StmtCache::erase(std::string_view sql)
	{
		if (auto it = map_.find(sql); it == map_.end())
		{
			return std::nullopt;
		}
		else
		{
			auto stmt = std::move(it->second->second);
			lruList_.erase(it->second);
			map_.erase(it);
			return stmt;
		}
	}

	std::vector<boost::mysql::statement> StmtCache::clear()
	{
		std::vector<boost::mysql::statement> stmts;
		stmts.reserve(lruList_.size());
		for (auto& entry : lruList_)
		{
			stmts.push_back(std::move(entry.second));
		}
		lruList_.clear();
		map_.clear();
		return stmts;
	}

	size_t StmtCache::size() const
	{
		return lruList_.size();
	}

	size_t StmtCache::maxSize() const
	{
		return maxSize_;
	}

} // namespace hical::db

#endif // HICAL_HAS_DATABASE
