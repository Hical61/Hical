/**
 * @file CTConv.h
 * @brief 编译期命名风格转换与整型转字符串工具
 */

#pragma once

#include <array>
#include <charconv>
#include <string>
#include <string_view>
#include <system_error>

namespace hical::CT
{

	/**
	 * @brief constexpr ASCII字符转大写，仅处理a‑z，其余字符原样返回
	 * @param ch 输入ASCII字符
	 * @return 转换后字符
	 */
	constexpr char asciiToUpper(const char ch) noexcept
	{
		if (ch >= 'a' && ch <= 'z')
		{
			return static_cast<char>(ch - ('a' - 'A'));
		}
		return ch;
	}

	/**
	 * @brief constexpr ASCII字符转小写，仅处理A‑Z，其余字符原样返回
	 * @param ch 输入ASCII字符
	 * @return 转换后字符
	 */
	constexpr char asciiToLower(const char ch) noexcept
	{
		if (ch >= 'A' && ch <= 'Z')
		{
			return static_cast<char>(ch + ('a' - 'A'));
		}
		return ch;
	}

	/**
	 * @brief constexpr snake_case → lowerCamelCase
	 * @param str 蛇形命名字符串视图
	 * @return 小驼峰 std::string
	 */
	constexpr std::string caseSnakeToLowerCamel(const std::string_view str)
	{
		std::string result;
		bool capitalize_next = false; // 下一个大写 flag
		bool has_emitted = false;     // 是否已经输出过有效（非下划线）字符

		for (const char c : str)
		{
			if (c == '_')
			{
				// 只有已经输出过字符，下划线才标记下一个要大写；前导下划线直接忽略
				if (has_emitted)
				{
					capitalize_next = true;
				}
				continue;
			}
			if (capitalize_next)
			{
				result += asciiToUpper(c);
				capitalize_next = false;
			}
			else
			{
				result += c;
			}
			has_emitted = true;
		}
		return result;
	}

	/**
	 * @brief constexpr snake_case → UpperCamelCase(PascalCase)
	 * @param str 蛇形命名字符串视图
	 * @return 大驼峰(PascalCase) std::string
	 */
	constexpr std::string caseSnakeToUpperCamel(const std::string_view str)
	{
		std::string result = caseSnakeToLowerCamel(str);
		if (not result.empty())
		{
			result.front() = asciiToUpper(result.front());
		}

		return result;
	}

	/**
	 * @brief constexpr lowerCamelCase / UpperCamelCase → snake_case
	 * @param str 大小驼峰字符串视图
	 * @return snake_case 蛇形字符串
	 */
	constexpr std::string caseCamelToSnake(const std::string_view str)
	{
		std::string result;
		for (const char c : str)
		{
			if (c >= 'A' && c <= 'Z')
			{                        // 判定大写
				if (!result.empty()) // 判定不是开头
				{
					result += '_';
				}
				result += asciiToLower(c);
			}
			else
			{
				result += c;
			}
		}
		return result;
	}

	/// 整型转字符串的栈缓冲长度，64 位整数最长 20 位，留足余量
	inline constexpr std::size_t kIntBufSize = 32;

	/**
	 * @brief 编译期整型转字符串
	 * @tparam IntType 待转换的整型
	 * @param val 待转换的值
	 * @return 十进制字符串，转换失败时返回 "null"
	 * @attention 依赖 std::to_chars 的 constexpr 化，需 C++23 及以上（C++20 下调用它会硬报错）
	 */
	template <std::integral IntType>
	consteval std::string toString(const IntType val)
	{
		std::array<char, kIntBufSize> buf {};
		const auto [ptr, ec] = std::to_chars(buf.begin(), buf.end(), val);

		if (ec != std::errc {})
		{
			return "null";
		}

		const std::string_view str(buf.data(), ptr - buf.data());
		return std::string {str};
	}
} // namespace hical::CT
