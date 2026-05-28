/**
 * @file StmtCache.h
 * @brief PreparedStatement LRU 缓存
 */

#pragma once

#ifdef HICAL_HAS_DATABASE

	#include <boost/mysql/statement.hpp>
	#include <cstddef>
	#include <list>
	#include <optional>
	#include <string>
	#include <string_view>
	#include <unordered_map>
	#include <vector>

namespace hical::db
{

	struct StringHash
	{
		using is_transparent = void;

		size_t operator()(std::string_view sv) const
		{
			return std::hash<std::string_view> {}(sv);
		}

		size_t operator()(const std::string& s) const
		{
			return std::hash<std::string_view> {}(s);
		}
	};

	struct StringEqual
	{
		using is_transparent = void;

		bool operator()(std::string_view a, std::string_view b) const
		{
			return a == b;
		}
	};

	/**
	 * @brief Per-connection LRU 缓存 for PreparedStatement
	 * 非线程安全：每个 MysqlConnection 独占一个实例。
	 * 缓存 SQL -> boost::mysql::statement 映射，命中时提升到 MRU。
	 * 满时淘汰 LRU 条目并返回其 statement（调用方需异步 close）。
	 */
	class StmtCache
	{
	public:
		/**
		 * @param maxSize 缓存容量上限（0 表示禁用缓存）
		 */
		explicit StmtCache(size_t maxSize = 64);

		/**
		 * @brief 查找缓存中的 statement
		 * 命中时将该条目提升到 MRU 位置。
		 * @param sql SQL 模板
		 * @return 命中时返回 statement 指针，未命中返回 nullptr
		 */
		[[nodiscard]] boost::mysql::statement* find(std::string_view sql);

		/**
		 * @brief 插入 statement 到缓存
		 * 若 SQL 已存在则更新并提升到 MRU。
		 * 若缓存已满则淘汰 LRU 条目。
		 * @param sql SQL 模板
		 * @param stmt 要缓存的 statement
		 * @return 被淘汰的 statement（调用方需关闭）；无淘汰时为 nullopt
		 */
		[[nodiscard]] std::optional<boost::mysql::statement> insert(const std::string& sql,
																	boost::mysql::statement stmt);

		/**
		 * @brief 移除指定 SQL 的缓存条目
		 * @param sql SQL 模板
		 * @return 被移除的 statement；不存在时为 nullopt
		 */
		[[nodiscard]] std::optional<boost::mysql::statement> erase(std::string_view sql);

		/**
		 * @brief 清空缓存，返回所有 statement（调用方需关闭）
		 */
		[[nodiscard]] std::vector<boost::mysql::statement> clear();

		[[nodiscard]] size_t size() const;
		[[nodiscard]] size_t maxSize() const;

	private:
		size_t maxSize_;

		// LRU 双向链表：front = MRU, back = LRU
		using LruEntry = std::pair<std::string, boost::mysql::statement>;
		std::list<LruEntry> lruList_;

		// SQL -> LRU 链表迭代器的哈希表
		std::unordered_map<std::string, std::list<LruEntry>::iterator, StringHash, StringEqual> map_;
	};

} // namespace hical::db

#endif // HICAL_HAS_DATABASE
