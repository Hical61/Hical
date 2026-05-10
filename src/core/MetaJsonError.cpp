#include "MetaJsonError.h"
#include <stdexcept>
#include <string>

namespace hical::meta::detail
{

	void throwTypeMismatch(std::string_view expected)
	{
		throw std::runtime_error(std::string("JSON type mismatch: expected ") + std::string(expected));
	}

	void throwMissingField(std::string_view fieldName)
	{
		throw std::runtime_error(std::string("fromJson: required field '") + std::string(fieldName) + "' is missing");
	}

	void throwParseError(std::string_view detail)
	{
		throw std::runtime_error(std::string("fromJson: ") + std::string(detail));
	}

} // namespace hical::meta::detail
