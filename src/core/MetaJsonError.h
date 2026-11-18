/**
 * @file MetaJsonError.h
 * @brief MetaJson 非模板错误抛出辅助函数
 * 将 throw std::runtime_error(...) 从模板代码中提取为非模板调用，
 * 减少每个用户类型实例化中的代码体积。
 */

#pragma once

#include <regex>
#include <string_view>

namespace hical::meta::detail
{

	/**
	 * @brief 报告字段声明中的错误
	 * @param fieldName 原始成员名或冲突的 JSON 键
	 * @param reason 定义冲突的原因
	 * @throws std::logic_error 字段定义无效
	 */
	[[noreturn]] void throwFieldDefinitionError(std::string_view fieldName, std::string_view reason);

	/**
	 * @brief JSON 类型不匹配时抛出异常
	 * @param expected 期望的类型名（如 "string", "integer", "object"）
	 */
	[[noreturn]] void throwTypeMismatch(std::string_view expected);

	/**
	 * @brief 报告 JSON 整数超出目标类型的范围
	 * @throws std::runtime_error 整数越界
	 */
	[[noreturn]] void throwIntegerOutOfRange();

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
	 * @note 每次调用都要重新编译 pattern。字段校验路径用下面的重载复用已编译结果。
	 */
	bool validatePattern(std::string_view value, std::string_view pattern);

	/**
	 * @brief 用已编译的正则校验字符串
	 * @param value 待校验的字符串值
	 * @param compiled 已编译的正则表达式
	 * @return true 匹配，false 不匹配
	 */
	bool validatePattern(std::string_view value, const std::regex& compiled);

} // namespace hical::meta::detail
