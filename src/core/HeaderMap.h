/**
 * @file HeaderMap.h
 * @brief HTTP 头部容器（大小写不敏感查找）
 */

#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace hical
{

	/**
	 * @brief HTTP 头部容器（大小写不敏感、支持多值字段）
	 * 内部使用 vector<pair<string,string>> 存储。
	 * HTTP 请求头部通常 < 20 个，线性扫描在 L1 缓存内完成，
	 * 性能优于 hash map（无哈希计算和指针追踪开销）。
	 */
	class HeaderMap
	{
	public:
		using Entry = std::pair<std::string, std::string>;
		using Container = std::vector<Entry>;
		using iterator = Container::iterator;
		using const_iterator = Container::const_iterator;

		HeaderMap() = default;

		/**
		 * @brief 大小写不敏感字符串比较
		 */
		static bool iequals(std::string_view a, std::string_view b) noexcept
		{
			if (a.size() != b.size())
			{
				return false;
			}
			for (size_t i = 0; i < a.size(); ++i)
			{
				if (toLower(a[i]) != toLower(b[i]))
				{
					return false;
				}
			}
			return true;
		}

		/**
		 * @brief 查找首个匹配的头部值（大小写不敏感）
		 * @param name 头部名称
		 * @return 值的 string_view，未找到返回空
		 */
		std::string_view find(std::string_view name) const noexcept
		{
			for (const auto& [k, v] : entries_)
			{
				if (iequals(k, name))
				{
					return v;
				}
			}
			return {};
		}

		/**
		 * @brief 设置头部（覆写首个匹配，否则追加）
		 * @param name 头部名称
		 * @param value 头部值
		 */
		void set(std::string_view name, std::string_view value)
		{
			for (auto& [k, v] : entries_)
			{
				if (iequals(k, name))
				{
					v = value;
					return;
				}
			}
			if (entries_.empty())
			{
				entries_.reserve(8);
			}
			entries_.emplace_back(std::string(name), std::string(value));
		}

		/**
		 * @brief 追加头部（始终新增条目，用于多值字段如 Set-Cookie）
		 * @param name 头部名称
		 * @param value 头部值
		 */
		void insert(std::string_view name, std::string_view value)
		{
			if (entries_.empty())
			{
				entries_.reserve(8);
			}
			entries_.emplace_back(std::string(name), std::string(value));
		}

		/**
		 * @brief 删除所有匹配的头部
		 * @param name 头部名称
		 */
		void erase(std::string_view name)
		{
			entries_.erase(std::remove_if(entries_.begin(),
										  entries_.end(),
										  [&name](const Entry& e)
										  {
											  return iequals(e.first, name);
										  }),
						   entries_.end());
		}

		/**
		 * @brief 查找所有匹配头部的值（大小写不敏感）
		 * @param name 头部名称
		 * @return 所有匹配值的 vector
		 */
		std::vector<std::string_view> findAll(std::string_view name) const
		{
			std::vector<std::string_view> result;
			for (const auto& [k, v] : entries_)
			{
				if (iequals(k, name))
				{
					result.push_back(v);
				}
			}
			return result;
		}

		/**
		 * @brief 是否包含指定头部（不论值是否为空）
		 */
		bool contains(std::string_view name) const noexcept
		{
			for (const auto& [k, v] : entries_)
			{
				if (iequals(k, name))
				{
					return true;
				}
			}
			return false;
		}

		/**
		 * @brief 统计指定名称的头部数量
		 */
		size_t count(std::string_view name) const noexcept
		{
			size_t n = 0;
			for (const auto& [k, v] : entries_)
			{
				if (iequals(k, name))
				{
					++n;
				}
			}
			return n;
		}

		iterator begin() noexcept
		{
			return entries_.begin();
		}

		iterator end() noexcept
		{
			return entries_.end();
		}

		const_iterator begin() const noexcept
		{
			return entries_.begin();
		}

		const_iterator end() const noexcept
		{
			return entries_.end();
		}

		size_t size() const noexcept
		{
			return entries_.size();
		}

		bool empty() const noexcept
		{
			return entries_.empty();
		}

		void reserve(size_t n)
		{
			entries_.reserve(n);
		}

		void clear() noexcept
		{
			entries_.clear();
		}

	private:
		/// 256 条转小写查表，编译期就搭好
		static constexpr std::array<char, 256> buildToLowerTable() noexcept
		{
			std::array<char, 256> tbl {};
			for (int i = 0; i < 256; ++i)
			{
				tbl[static_cast<size_t>(i)] = static_cast<char>(i);
			}
			for (int i = 'A'; i <= 'Z'; ++i)
			{
				tbl[static_cast<size_t>(i)] = static_cast<char>(i + ('a' - 'A'));
			}
			return tbl;
		}

		inline static const std::array<char, 256> kToLowerTable_ = buildToLowerTable();

		static char toLower(char c) noexcept
		{
			return kToLowerTable_[static_cast<unsigned char>(c)];
		}

		Container entries_;
	};

} // namespace hical
