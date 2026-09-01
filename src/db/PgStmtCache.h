/**
 * @file PgStmtCache.h
 * @brief libpq 预处理语句 LRU 缓存
 */

#pragma once

#ifdef HICAL_HAS_PGSQL

	#include <cstddef>
	#include <list>
	#include <optional>
	#include <string>
	#include <string_view>
	#include <unordered_map>
	#include <utility>

namespace hical::db
{

	struct PgStringHash
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

	struct PgStringEqual
	{
		using is_transparent = void;

		bool operator()(std::string_view a, std::string_view b) const
		{
			return a == b;
		}
	};

	/**
	 * @brief libpq 预处理语句的每连接 LRU 缓存
	 * 非线程安全，每个 PgsqlConnection 独占一份。
	 * 缓存 SQL 到语句名（stmtName）的映射，命中就提到 MRU。
	 * 跟 MySQL 的 StmtCache 不一样：libpq 的语句名是连接级命名空间，没有独立要
	 * 关闭的资源，淘汰/清空只要从映射里删掉条目，等 PQfinish 统一回收。语句名由
	 * PgsqlConnection 生成（stmt_<counter>），这里只负责按 SQL 存取，不关心它怎么来。
	 */
	class PgStmtCache
	{
	public:
		/**
		 * @param maxSize 缓存容量上限（0 表示禁用缓存）
		 */
		explicit PgStmtCache(size_t maxSize = 64);

		/**
		 * @brief 查找缓存中的语句名字
		 * 命中时将该条目提升到 MRU 位置。
		 * @param sql SQL 模板
		 * @return 命中时返回 stmtName，未命中返回 nullopt
		 */
		[[nodiscard]] std::optional<std::string> find(std::string_view sql);

		/**
		 * @brief 插入 SQL -> stmtName 到缓存
		 * 若 SQL 已存在则更新名字并提升到 MRU。
		 * 若缓存已满则淘汰 LRU 条目。
		 * @param sql SQL 模板
		 * @param stmtName 预处理语句名字
		 */
		void insert(std::string sql, std::string stmtName);

		/**
		 * @brief 移除指定 SQL 的缓存条目
		 * @param sql SQL 模板
		 */
		void erase(std::string_view sql);

		/// 清空缓存
		void clear();

		[[nodiscard]] size_t size() const;
		[[nodiscard]] size_t maxSize() const;

	private:
		size_t maxSize_;

		// LRU 双向链表：front = MRU, back = LRU
		using LruEntry = std::pair<std::string, std::string>;
		std::list<LruEntry> lruList_;

		// SQL -> LRU 链表迭代器的哈希表（透明哈希，string_view 零分配查找）
		std::unordered_map<std::string, std::list<LruEntry>::iterator, PgStringHash, PgStringEqual> map_;
	};

} // namespace hical::db

#endif // HICAL_HAS_PGSQL
