/**
 * @file DbResult.h
 * @brief 数据库查询结果封装
 */

#pragma once

#include <cassert>
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
		/// 列名（仅 SELECT 有效），索引对应行内列下标
		std::vector<std::string> columns;

		/**
		 * @brief 行数据代理视图
		 * 零拷贝：只持有指向扁平 cell 缓冲的指针 + 行起始偏移 + 列数。下标返回底层
		 * std::string 的引用，所以本 proxy 生命周期绑定 DbResult，不额外分配，
		 * 也不存在 string_view 那种「结果集析构后悬垂」的问题。
		 */
		class RowProxy
		{
		public:
			const std::string& operator[](size_t col) const
			{
				return (*cells_)[rowStart_ + col];
			}

			/// 本行列数（== columns.size()）
			[[nodiscard]] size_t size() const
			{
				return nfields_;
			}

		private:
			friend struct DbResult;

			// 仅 DbResult::operator[] 内部构造，外部拿不到
			RowProxy(const std::vector<std::string>& cells, size_t rowStart, size_t nfields)
				: cells_(&cells), rowStart_(rowStart), nfields_(nfields)
			{
			}

			const std::vector<std::string>* cells_;
			size_t rowStart_;
			size_t nfields_;
		};

		uint64_t affectedRows = 0;
		uint64_t insertId = 0;

		[[nodiscard]] bool empty() const
		{
			return nrows_ == 0;
		}

		/// 行数
		[[nodiscard]] size_t size() const
		{
			return nrows_;
		}

		/// 列数（== columns.size()，冗余缓存方便 RowProxy 算偏移）
		[[nodiscard]] size_t nfields() const
		{
			return nfields_;
		}

		RowProxy operator[](size_t row) const
		{
			return {cells_, row * nfields_, nfields_};
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

		// ============ 供后端 convertResults 填充用的内部接口 ============

		/**
		 * @brief 便捷构造：DML 结果（无行集，仅 affectedRows/insertId）
		 */
		static DbResult fromDml(uint64_t affectedRows, uint64_t insertId = 0)
		{
			DbResult r;
			r.affectedRows = affectedRows;
			r.insertId = insertId;
			return r;
		}

		/**
		 * @brief 便捷构造：用列名 + 二维行数据铺成扁平结果集（主要给测试用）
		 * @param cols 列名
		 * @param rows 行数据（每行一个 vector<string>）
		 */
		static DbResult fromRows(std::vector<std::string> cols, std::vector<std::vector<std::string>> rows)
		{
			DbResult r;
			r.columns = std::move(cols);
			size_t nfields = r.columns.size();
			r.reserveCells(nfields * rows.size());
			for (auto& row : rows)
			{
				for (auto& cell : row)
				{
					r.appendCell(std::move(cell));
				}
			}
			r.setShape(nfields, rows.size());
			return r;
		}

		/// 预留所有 cell 的容量（一次分配，避免 push 反复扩容）
		void reserveCells(size_t totalCells)
		{
			cells_.reserve(totalCells);
		}

		/// 追加一个 cell 值（按行优先顺序）
		void appendCell(std::string value)
		{
			cells_.push_back(std::move(value));
		}

		/// 回填结果集形状（列数、行数），填充完毕后调用一次
		void setShape(size_t nfields, size_t nrows)
		{
			// nfields 必须与 columns.size() 一致，否则 RowProxy 偏移计算会错位
			assert(nfields == columns.size());
			nfields_ = nfields;
			nrows_ = nrows;
		}

	private:
		/// 扁平存储：所有行的所有 cell，按 row-major（先行后列）排布
		std::vector<std::string> cells_;
		size_t nfields_ = 0;
		size_t nrows_ = 0;
	};

} // namespace hical::db
