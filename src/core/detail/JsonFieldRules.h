/**
 * @file JsonFieldRules.h
 * @brief 两条反射路径共用的字段规则和输入校验
 */

#pragma once

#include "../MetaJsonError.h"
#include <cstdint>
#include <optional>
#include <string_view>

namespace hical::meta::detail
{

	/**
	 * @brief 字段最终使用的 JSON 键、可见性和输入规则
	 * name 引用声明中的静态字符串。数值上下界用 int32 表示。
	 */
	struct JsonFieldRules
	{
		std::string_view name;
		bool required = false;
		bool ignored = false;
		bool notEmpty = false;
		std::optional<std::int32_t> minimum {};
		std::optional<std::int32_t> maximum {};
	};

	/**
	 * @brief 检查字段声明中的数值范围
	 * @param rules 字段的最终规则
	 * @throws std::logic_error 下界大于上界
	 */
	inline void validateJsonFieldRuleDefinition(const JsonFieldRules& rules)
	{
		if (rules.minimum && rules.maximum && *rules.minimum > *rules.maximum)
		{
			throwFieldDefinitionError(rules.name, "数值区间为空");
		}
	}

	/**
	 * @brief 检查必填字段是否存在
	 * @param name 最终 JSON 键
	 * @param required 是否显式声明必填
	 * @param present 输入对象中是否存在该键
	 */
	inline void validateJsonFieldPresence(std::string_view name, bool required, bool present)
	{
		if (required && !present)
		{
			throwMissingField(name);
		}
	}

	/**
	 * @brief 检查可见的必填字段是否存在
	 * @param rules 字段的最终规则
	 * @param present 输入对象中是否存在该键
	 */
	inline void validateJsonFieldPresence(const JsonFieldRules& rules, bool present)
	{
		validateJsonFieldPresence(rules.name, rules.required && !rules.ignored, present);
	}

	/**
	 * @brief 检查需要非空的字符串是否为空
	 * @param name 最终 JSON 键
	 * @param notEmpty 是否禁止空字符串
	 * @param value 已解码的字符串
	 */
	inline void validateJsonNotEmpty(std::string_view name, bool notEmpty, std::string_view value)
	{
		if (notEmpty && value.empty())
		{
			throwValidationErrorStr(name, "not-empty");
		}
	}

	/**
	 * @brief 检查 int32 输入是否在字段声明的范围内
	 * @tparam Limit 原生注解使用 int32 边界，宏使用 double 边界
	 * @param name 最终 JSON 键
	 * @param value 解码后的 int32 值
	 * @param minimum 下界，缺省时不检查
	 * @param maximum 上界，缺省时不检查
	 */
	template <typename Limit>
	void validateJsonInt32Range(std::string_view name,
								std::int32_t value,
								const std::optional<Limit>& minimum,
								const std::optional<Limit>& maximum)
	{
		// int32 转成 double 不损失精度，宏里的小数边界也能直接比较。
		if (minimum && static_cast<double>(value) < *minimum)
		{
			throwValidationErrorNum(name, "minimum", *minimum);
		}
		if (maximum && static_cast<double>(value) > *maximum)
		{
			throwValidationErrorNum(name, "maximum", *maximum);
		}
	}

	/**
	 * @brief 检查可见的 int32 字段是否超出声明的范围
	 * @param rules 字段的最终规则
	 * @param value 解码后的字段值
	 */
	inline void validateJsonFieldValue(const JsonFieldRules& rules, std::int32_t value)
	{
		if (!rules.ignored)
		{
			validateJsonInt32Range(rules.name, value, rules.minimum, rules.maximum);
		}
	}

	/**
	 * @brief 检查可见的字符串字段是否符合非空要求
	 * @param rules 字段的最终规则
	 * @param value 已解码的字段值
	 */
	inline void validateJsonFieldValue(const JsonFieldRules& rules, std::string_view value)
	{
		if (!rules.ignored)
		{
			validateJsonNotEmpty(rules.name, rules.notEmpty, value);
		}
	}

} // namespace hical::meta::detail
