/**
 * @file DbResult.h
 * @brief 数据库查询结果封装
 */

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace hical::db
{

	/**
	 * @brief 数据库查询结果集（SELECT 返回行列数据，DML 返回 affectedRows/insertId）
	 */
	struct DbResult
	{
		/// 列名（仅 SELECT 有效），索引对应 rows 内层下标
		std::vector<std::string> columns;

		/// 行数据，每行的 size() == columns.size()
		std::vector<std::vector<std::string>> rows;

		uint64_t affectedRows = 0;
		uint64_t insertId = 0;

		[[nodiscard]] bool empty() const
		{
			return rows.empty();
		}

		[[nodiscard]] size_t size() const
		{
			return rows.size();
		}

		const std::vector<std::string>& operator[](size_t index) const
		{
			return rows[index];
		}

		std::vector<std::string>& operator[](size_t index)
		{
			return rows[index];
		}

		static constexpr size_t npos = static_cast<size_t>(-1);

		/**
		 * @brief 按列名查找列索引，未找到返回 npos
		 */
		[[nodiscard]] size_t columnIndex(std::string_view name) const
		{
			for (size_t i = 0; i < columns.size(); ++i)
			{
				if (columns[i] == name)
				{
					return i;
				}
			}
			return npos;
		}
	};

} // namespace hical::db
