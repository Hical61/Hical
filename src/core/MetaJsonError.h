/**
 * @file MetaJsonError.h
 * @brief MetaJson 非模板错误抛出辅助函数
 * 将 throw std::runtime_error(...) 从模板代码中提取为非模板调用，
 * 减少每个用户类型实例化中的代码体积。
 */

#pragma once

#include <string_view>

namespace hical::meta::detail
{

	/**
	 * @brief JSON 类型不匹配时抛出异常
	 * @param expected 期望的类型名（如 "string", "integer", "object"）
	 */
	[[noreturn]] void throwTypeMismatch(std::string_view expected);

	/**
	 * @brief 缺少必需字段时抛出异常
	 * @param fieldName 缺少的字段名
	 */
	[[noreturn]] void throwMissingField(std::string_view fieldName);

	/**
	 * @brief JSON 解析错误时抛出异常
	 * @param detail 错误描述
	 */
	[[noreturn]] void throwParseError(std::string_view detail);

	/**
	 * @brief 校验失败时抛出异常（数值约束）
	 * @param fieldName 字段名
	 * @param rule 校验规则名（"minimum"/"maximum"/"min length"/"max length"）
	 * @param limit 约束值
	 */
	[[noreturn]] void throwValidationErrorNum(std::string_view fieldName, std::string_view rule, double limit);

	/**
	 * @brief 校验失败时抛出异常（字符串约束）
	 * @param fieldName 字段名
	 * @param rule 校验规则名（"not empty"/"pattern"）
	 */
	[[noreturn]] void throwValidationErrorStr(std::string_view fieldName, std::string_view rule);

	/**
	 * @brief 校验正则表达式是否匹配
	 * @param value 待校验的字符串值
	 * @param pattern 正则表达式字符串
	 * @return true 匹配，false 不匹配
	 */
	bool validatePattern(std::string_view value, std::string_view pattern);

} // namespace hical::meta::detail
