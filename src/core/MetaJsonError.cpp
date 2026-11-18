/**
 * @file MetaJsonError.cpp
 * @brief JSON 非模板错误函数实现
 */

#include "MetaJsonError.h"
#include <regex>
#include <stdexcept>
#include <string>

namespace hical::meta::detail
{

	void throwFieldDefinitionError(std::string_view fieldName, std::string_view reason)
	{
		throw std::logic_error(std::string("HICAL_JSON 字段 '") + std::string(fieldName) + "' 定义错误："
							   + std::string(reason));
	}

	void throwTypeMismatch(std::string_view expected)
	{
		throw std::runtime_error(std::string("JSON type mismatch: expected ") + std::string(expected));
	}

	void throwIntegerOutOfRange()
	{
		throw std::runtime_error("fromJson: 整数超出目标类型的可表示范围");
	}

	void throwMissingField(std::string_view fieldName)
	{
		throw std::runtime_error(std::string("fromJson: required field '") + std::string(fieldName) + "' is missing");
	}

	void throwParseError(std::string_view detail)
	{
		throw std::runtime_error(std::string("fromJson: ") + std::string(detail));
	}

	void throwValidationErrorNum(std::string_view fieldName, std::string_view rule, double limit)
	{
		std::string msg = "fromJson: validation failed for field '";
		msg += fieldName;
		msg += "' - ";
		msg += rule;
		msg += " constraint violated (";
		msg += std::to_string(limit);
		msg += ")";
		throw std::runtime_error(msg);
	}

	void throwValidationErrorStr(std::string_view fieldName, std::string_view rule)
	{
		std::string msg = "fromJson: validation failed for field '";
		msg += fieldName;
		msg += "' - ";
		msg += rule;
		msg += " constraint violated";
		throw std::runtime_error(msg);
	}

	bool validatePattern(std::string_view value, std::string_view pattern)
	{
		try
		{
			std::regex re(pattern.data(), pattern.size(), std::regex::ECMAScript);
			return std::regex_match(value.data(), value.data() + value.size(), re);
		}
		catch (const std::regex_error&)
		{
			return false;
		}
	}

	bool validatePattern(std::string_view value, const std::regex& compiled)
	{
		return std::regex_match(value.data(), value.data() + value.size(), compiled);
	}

} // namespace hical::meta::detail
