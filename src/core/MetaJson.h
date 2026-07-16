/**
 * @file MetaJson.h
 * @brief 反射驱动的 JSON 自动序列化/反序列化
 * 双路线：
 * - C++26 反射：通过 ^^T 自动枚举数据成员，无需用户标注
 * - C++20 回退：用户使用 HICAL_JSON(StructType, field1, field2, ...) 宏标注字段
 * 对外 API：
 *   boost::json::value json = hical::meta::toJson(myStruct);
 *   auto obj = hical::meta::fromJson<MyStruct>(jsonValue);
 * 支持类型：int, int64_t, double, bool, std::string, std::vector<T>, 嵌套结构体
 */

#pragma once

#include "Reflection.h"
#include "MetaJsonError.h"
#include "HttpRequest.h"
#include <boost/json.hpp>
#include <optional>
#include <string>
#include <tuple>
#include <type_traits>
#include <vector>

namespace hical::meta
{

	// ============ 类型萃取 ============

	template <typename T>
	struct IsVector : std::false_type
	{
	};

	template <typename T, typename A>
	struct IsVector<std::vector<T, A>> : std::true_type
	{
	};

	// ============ 前向声明 ============

	template <typename T>
	boost::json::object toJson(const T& obj);

	template <typename T>
	T fromJson(const boost::json::value& json);

	// ============ 单值 JSON 转换 ============

	/**
	 * @brief 将 C++ 值转换为 boost::json::value
	 */
	template <typename T>
	boost::json::value valueToJson(const T& val)
	{
		if constexpr (std::is_same_v<T, std::string>)
		{
			return boost::json::value(val);
		}
		else if constexpr (std::is_same_v<T, bool>)
		{
			return val;
		}
		else if constexpr (std::is_integral_v<T>)
		{
			if constexpr (std::is_unsigned_v<T>)
			{
				return static_cast<uint64_t>(val);
			}
			else
			{
				return static_cast<int64_t>(val);
			}
		}
		else if constexpr (std::is_floating_point_v<T>)
		{
			return static_cast<double>(val);
		}
		else if constexpr (IsVector<T>::value)
		{
			boost::json::array arr;
			for (const auto& item : val)
			{
				arr.push_back(valueToJson(item));
			}
			return arr;
		}
		else if constexpr (HasJsonFields<T>::value)
		{
			return toJson(val);
		}
		else
		{
			static_assert(sizeof(T) == 0, "Unsupported type for JSON serialization");
		}
	}

	/**
	 * @brief 从 boost::json::value 提取 C++ 值（带类型检查）
	 * @throws std::runtime_error 当 JSON 值类型与目标 C++ 类型不匹配时
	 */
	template <typename T>
	T valueFromJson(const boost::json::value& val)
	{
		if constexpr (std::is_same_v<T, std::string>)
		{
			if (!val.is_string())
			{
				detail::throwTypeMismatch("string");
			}
			return std::string(val.as_string());
		}
		else if constexpr (std::is_same_v<T, bool>)
		{
			if (!val.is_bool())
			{
				detail::throwTypeMismatch("bool");
			}
			return val.as_bool();
		}
		else if constexpr (std::is_integral_v<T>)
		{
			if (!val.is_int64() && !val.is_uint64())
			{
				detail::throwTypeMismatch("integer");
			}
			if (val.is_uint64())
			{
				return static_cast<T>(val.as_uint64());
			}
			return static_cast<T>(val.as_int64());
		}
		else if constexpr (std::is_floating_point_v<T>)
		{
			if (!val.is_double() && !val.is_int64() && !val.is_uint64())
			{
				detail::throwTypeMismatch("number");
			}
			if (val.is_int64())
			{
				return static_cast<T>(val.as_int64());
			}
			if (val.is_uint64())
			{
				return static_cast<T>(val.as_uint64());
			}
			return static_cast<T>(val.as_double());
		}
		else if constexpr (IsVector<T>::value)
		{
			if (!val.is_array())
			{
				detail::throwTypeMismatch("array");
			}
			using ElemType = typename T::value_type;
			T result;
			const auto& arr = val.as_array();
			result.reserve(arr.size());
			for (const auto& item : arr)
			{
				result.push_back(valueFromJson<ElemType>(item));
			}
			return result;
		}
		else if constexpr (HasJsonFields<T>::value)
		{
			if (!val.is_object())
			{
				detail::throwTypeMismatch("object");
			}
			return fromJson<T>(val);
		}
		else
		{
			static_assert(sizeof(T) == 0, "Unsupported type for JSON deserialization");
		}
	}

	// ============ C++20 回退实现 ============

#if !HICAL_HAS_REFLECTION

	namespace detail
	{

		/**
		 * @brief 字段描述器：绑定字段名 + 成员指针 + 必需/忽略标记 + 校验约束
		 */
		template <typename Class, typename FieldType>
		struct FieldDescriptor
		{
			std::string_view name;
			FieldType Class::* pointer;
			bool required = false;
			bool ignored = false;
			// 校验约束
			std::optional<double> minVal;
			std::optional<double> maxVal;
			std::optional<std::string> pattern;
			bool notEmpty = false;
			std::optional<size_t> lengthMin;
			std::optional<size_t> lengthMax;
		};

		/**
		 * @brief 创建字段描述器的辅助函数（2 参数，向后兼容）
		 */
		template <typename Class, typename FieldType>
		constexpr FieldDescriptor<Class, FieldType> makeField(std::string_view name, FieldType Class::* ptr)
		{
			return {name, ptr, false, false, {}, {}, {}, false, {}, {}};
		}

		/**
		 * @brief 创建字段描述器的辅助函数（4 参数，支持 required/ignored）
		 */
		template <typename Class, typename FieldType>
		constexpr FieldDescriptor<Class, FieldType> makeField(std::string_view name,
															  FieldType Class::* ptr,
															  bool isRequired,
															  bool isIgnored)
		{
			return {name, ptr, isRequired, isIgnored, {}, {}, {}, false, {}, {}};
		}

		/**
		 * @brief 创建字段描述器的辅助函数（满参数，支持校验约束）
		 */
		template <typename Class, typename FieldType>
		constexpr FieldDescriptor<Class, FieldType> makeField(std::string_view name,
															  FieldType Class::* ptr,
															  bool isRequired,
															  bool isIgnored,
															  std::optional<double> minVal,
															  std::optional<double> maxVal,
															  std::optional<std::string> pattern,
															  bool notEmpty,
															  std::optional<size_t> lengthMin,
															  std::optional<size_t> lengthMax)
		{
			return {name, ptr, isRequired, isIgnored, minVal, maxVal, pattern, notEmpty, lengthMin, lengthMax};
		}

		/**
		 * @brief 序列化所有字段（跳过 ignored 字段）
		 */
		template <typename T, typename Tuple, size_t... I>
		void serializeFields(const T& obj, boost::json::object& jsonObj, const Tuple& fields, std::index_sequence<I...>)
		{
			auto serializeOne = [&](const auto& field)
			{
				if (!field.ignored)
				{
					jsonObj[field.name] = valueToJson(obj.*field.pointer);
				}
			};
			(serializeOne(std::get<I>(fields)), ...);
		}

		/**
		 * @brief 校验单字段约束
		 * 根据 FieldDescriptor 中的约束规则校验已反序列化的值。
		 * 支持：最小值/最大值/正则/非空/长度范围。
		 */
		template <typename FieldDesc, typename FieldType>
		void validateField(const FieldDesc& field, const FieldType& value)
		{
			if (field.minVal)
			{
				if constexpr (std::is_arithmetic_v<FieldType>)
				{
					if (static_cast<double>(value) < *field.minVal)
					{
						detail::throwValidationErrorNum(field.name, "minimum", *field.minVal);
					}
				}
			}
			if (field.maxVal)
			{
				if constexpr (std::is_arithmetic_v<FieldType>)
				{
					if (static_cast<double>(value) > *field.maxVal)
					{
						detail::throwValidationErrorNum(field.name, "maximum", *field.maxVal);
					}
				}
			}
			if (field.notEmpty)
			{
				if constexpr (std::is_same_v<FieldType, std::string>)
				{
					if (value.empty())
					{
						detail::throwValidationErrorStr(field.name, "not-empty");
					}
				}
			}
			if (field.pattern)
			{
				if constexpr (std::is_same_v<FieldType, std::string>)
				{
					if (!detail::validatePattern(value, *field.pattern))
					{
						detail::throwValidationErrorStr(field.name, "pattern");
					}
				}
			}
			if (field.lengthMin)
			{
				if constexpr (std::is_same_v<FieldType, std::string>)
				{
					if (value.size() < *field.lengthMin)
					{
						detail::throwValidationErrorNum(field.name,
														"min-length",
														static_cast<double>(*field.lengthMin));
					}
				}
			}
			if (field.lengthMax)
			{
				if constexpr (std::is_same_v<FieldType, std::string>)
				{
					if (value.size() > *field.lengthMax)
					{
						detail::throwValidationErrorNum(field.name,
														"max-length",
														static_cast<double>(*field.lengthMax));
					}
				}
			}
		}

		/**
		 * @brief 反序列化所有字段（跳过 ignored，检查 required）
		 */
		template <typename T, typename Tuple, size_t... I>
		void deserializeFields(T& obj,
							   const boost::json::object& jsonObj,
							   const Tuple& fields,
							   std::index_sequence<I...>)
		{
			auto trySet = [&](const auto& field)
			{
				if (field.ignored)
				{
					return;
				}
				auto it = jsonObj.find(field.name);
				if (it != jsonObj.end())
				{
					using FieldType = std::remove_reference_t<decltype(obj.*field.pointer)>;
					obj.*field.pointer = valueFromJson<FieldType>(it->value());

					// 校验约束
					validateField(field, obj.*field.pointer);
				}
				else if (field.required)
				{
					detail::throwMissingField(field.name);
				}
			};
			(trySet(std::get<I>(fields)), ...);
		}

	} // namespace detail

	/**
	 * @brief 序列化结构体为 JSON（C++20 回退）
	 */
	template <typename T>
	boost::json::object toJson(const T& obj)
	{
		static_assert(HasJsonFields<T>::value, "Type must use HICAL_JSON() macro or have C++26 reflection support");

		const auto& fields = T::hicalJsonFields();
		boost::json::object jsonObj;
		constexpr auto count = std::tuple_size_v<std::remove_cvref_t<decltype(fields)>>;
		detail::serializeFields(obj, jsonObj, fields, std::make_index_sequence<count> {});
		return jsonObj;
	}

	/**
	 * @brief 从 JSON 反序列化为结构体（C++20 回退）
	 */
	template <typename T>
	T fromJson(const boost::json::value& json)
	{
		static_assert(HasJsonFields<T>::value, "Type must use HICAL_JSON() macro or have C++26 reflection support");

		if (!json.is_object())
		{
			detail::throwParseError("expected JSON object, got " + std::string(to_string(json.kind())));
		}

		T obj {};
		const auto& jsonObj = json.as_object();
		const auto& fields = T::hicalJsonFields();
		constexpr auto count = std::tuple_size_v<std::remove_cvref_t<decltype(fields)>>;
		detail::deserializeFields(obj, jsonObj, fields, std::make_index_sequence<count> {});
		return obj;
	}

#else // HICAL_HAS_REFLECTION == 1

	// ============ C++26 反射实现 ============

	namespace detail
	{

		/**
		 * @brief 检测字段是否带指定属性（通用基础函数）
		 */
		template <auto Member>
		consteval bool hasAttribute(std::string_view attrName)
		{
			for (auto attr : std::meta::attributes_of(Member))
			{
				if (std::meta::identifier_of(attr) == attrName)
				{
					return true;
				}
			}
			return false;
		}

		template <auto Member>
		consteval bool isJsonIgnored()
		{
			return hasAttribute<Member>("hical::json_ignore");
		}

		template <auto Member>
		consteval bool isJsonRequired()
		{
			return hasAttribute<Member>("hical::json_required");
		}

		/**
		 * @brief 获取字段的 JSON key（优先 [[hical::json_name("xxx")]]，否则原名）
		 */
		template <auto Member>
		consteval std::string_view jsonKeyOf()
		{
			for (auto attr : std::meta::attributes_of(Member))
			{
				if (std::meta::identifier_of(attr) == "hical::json_name")
				{
					auto args = std::meta::attribute_arguments_of(attr);
					return std::meta::extract<const char*>(args[0]);
				}
			}
			return std::meta::identifier_of(Member);
		}

		/**
		 * @brief camelCase → snake_case 编译期转换
		 */
		consteval bool isUpperAscii(char c) noexcept
		{
			return c >= 'A' && c <= 'Z';
		}

		consteval char toLowerAscii(char c) noexcept
		{
			return isUpperAscii(c) ? static_cast<char>(c + ('a' - 'A')) : c;
		}

		consteval std::string camelToSnake(std::string_view input)
		{
			std::string result;
			for (size_t i = 0; i < input.size(); ++i)
			{
				if (i > 0 && isUpperAscii(input[i]))
				{
					result += '_';
				}
				result += toLowerAscii(input[i]);
			}
			return result;
		}

	} // namespace detail

	/**
	 * @brief 序列化结构体为 JSON（C++26 反射）
	 * 支持 [[hical::json_ignore]]、[[hical::json_name("xxx")]]
	 */
	template <typename T>
	boost::json::object toJson(const T& obj)
	{
		boost::json::object jsonObj;

		template for (constexpr auto member :
					  std::meta::nonstatic_data_members_of(^^T, std::meta::access_context::unprivileged()))
		{
			if constexpr (!detail::isJsonIgnored<member>())
			{
				constexpr auto key = detail::jsonKeyOf<member>();
				jsonObj[key] = valueToJson(obj.[:member:]);
			}
		}

		return jsonObj;
	}

	/**
	 * @brief 从 JSON 反序列化为结构体（C++26 反射）
	 * 支持 [[hical::json_ignore]]、[[hical::json_name("xxx")]]、[[hical::json_required]]
	 */
	template <typename T>
	T fromJson(const boost::json::value& json)
	{
		if (!json.is_object())
		{
			detail::throwParseError("expected JSON object, got " + std::string(to_string(json.kind())));
		}

		T obj {};
		const auto& jsonObj = json.as_object();

		template for (constexpr auto member :
					  std::meta::nonstatic_data_members_of(^^T, std::meta::access_context::unprivileged()))
		{
			if constexpr (!detail::isJsonIgnored<member>())
			{
				constexpr auto key = detail::jsonKeyOf<member>();
				auto it = jsonObj.find(key);
				if (it != jsonObj.end())
				{
					using FieldType = typename[:std::meta::type_of(member):];
					obj.[:member:] = valueFromJson<FieldType>(it->value());
				}
				else if constexpr (detail::isJsonRequired<member>())
				{
					detail::throwMissingField(key);
				}
			}
		}

		return obj;
	}

	/**
	 * @brief 编译期生成 JSON Schema（C++26 反射）
	 * 根据结构体成员类型和属性注解自动生成符合 JSON Schema 规范的描述。
	 */
	template <typename T>
	boost::json::object jsonSchema()
	{
		boost::json::object schema;
		schema["type"] = "object";
		boost::json::object properties;
		boost::json::array requiredFields;

		template for (constexpr auto member :
					  std::meta::nonstatic_data_members_of(^^T, std::meta::access_context::unprivileged()))
		{
			if constexpr (!detail::isJsonIgnored<member>())
			{
				constexpr auto key = detail::jsonKeyOf<member>();
				using FT = typename[:std::meta::type_of(member):];

				boost::json::object prop;
				if constexpr (std::is_same_v<FT, std::string>)
				{
					prop["type"] = "string";
				}
				else if constexpr (std::is_same_v<FT, bool>)
				{
					prop["type"] = "boolean";
				}
				else if constexpr (std::is_integral_v<FT>)
				{
					prop["type"] = "integer";
				}
				else if constexpr (std::is_floating_point_v<FT>)
				{
					prop["type"] = "number";
				}
				else if constexpr (IsVector<FT>::value)
				{
					prop["type"] = "array";
				}
				else if constexpr (HasJsonFields<FT>::value)
				{
					prop = jsonSchema<FT>();
				}

				properties[key] = prop;

				if constexpr (detail::isJsonRequired<member>())
				{
					requiredFields.push_back(std::string(key));
				}
			}
		}

		schema["properties"] = properties;
		if (!requiredFields.empty())
		{
			schema["required"] = requiredFields;
		}
		return schema;
	}

	/**
	 * @brief 序列化策略：camelCase → snake_case 自动转换（C++26 反射）
	 * 优先使用 [[hical::json_name(...)]] 显式注解，否则自动转换。
	 */
	template <typename T>
	boost::json::object toJsonSnakeCase(const T& obj)
	{
		boost::json::object jsonObj;

		template for (constexpr auto member :
					  std::meta::nonstatic_data_members_of(^^T, std::meta::access_context::unprivileged()))
		{
			if constexpr (!detail::isJsonIgnored<member>())
			{
				constexpr auto explicitKey = detail::jsonKeyOf<member>();
				constexpr auto memberName = std::meta::identifier_of(member);
				constexpr auto key = (explicitKey != memberName) ? explicitKey : detail::camelToSnake(memberName);
				jsonObj[key] = valueToJson(obj.[:member:]);
			}
		}

		return jsonObj;
	}

#endif // HICAL_HAS_REFLECTION

	/**
	 * @brief 从 HttpRequest 消息体反序列化为指定类型
	 * @tparam T 目标类型（需标注 HICAL_JSON 或支持 C++26 反射）
	 * @param req HTTP 请求
	 * @return T 反序列化后的对象
	 * @throws std::runtime_error 当请求体不是合法 JSON 或类型不匹配时
	 * 用法：auto user = hical::meta::readJson<UserDTO>(req);
	 * 或直接：auto user = req.readJson<UserDTO>();（需 include MetaJson.h）
	 */
	template <typename T>
	T readJson(const ::hical::HttpRequest& req)
	{
		const auto& json = req.jsonBody();
		if (json.is_null())
		{
			detail::throwParseError("request body is not valid JSON");
		}
		return fromJson<T>(json);
	}

} // namespace hical::meta

// ============ HttpRequest::readJson 扩展 ============
// 需要 include MetaJson.h 后才可使用，避免 HttpRequest.h 的编译耦合

namespace hical
{

	/**
	 * @brief 将消息体反序列化为指定类型（反射驱动）
	 * @tparam T 目标类型（需标注 HICAL_JSON 或支持 C++26 反射）
	 * @return T 反序列化后的对象
	 * 用法：
	 *   #include "core/MetaJson.h"
	 *   auto user = req.readJson<UserDTO>();
	 */
	template <typename T>
	T HttpRequest::readJson() const
	{
		return meta::readJson<T>(*this);
	}

} // namespace hical

// ============ C++20 回退宏 ============

/**
 * @brief 标注结构体字段用于自动 JSON 序列化（C++20 回退方案）
 * 基本用法（字段名 = JSON key，全部可选）：
 * ```cpp
 * struct UserDTO
 * {
 *     std::string name;
 *     int age;
 *     HICAL_JSON(UserDTO, name, age)
 * };
 * ```
 * 高级用法（装饰器混写）：
 * ```cpp
 * struct ApiResponse
 * {
 *     std::string requestId;
 *     int statusCode;
 *     std::string message;
 *     std::string traceId;
 *     HICAL_JSON(ApiResponse,
 *         REQUIRED_ALIAS(requestId, "request_id"),
 *         REQUIRED(statusCode),
 *         ALIAS(message, "status_message"),
 *         HICAL_IGNORE(traceId))
 * };
 * ```
 * 装饰器：
 *   ALIAS(field, "json_key")          - 自定义 JSON key
 *   REQUIRED(field)                   - 反序列化时必需
 *   REQUIRED_ALIAS(field, "json_key") - 必需 + 自定义 key
 *   HICAL_IGNORE(field)               - 序列化/反序列化均跳过
 *   MIN(field, val)                   - 数值最小值校验
 *   MAX(field, val)                   - 数值最大值校验
 *   NOT_EMPTY(field)                  - 字符串非空校验
 *   PATTERN(field, "re")              - 正则匹配校验
 *   LENGTH(field, min, max)           - 字符串长度范围校验
 * 当 C++26 反射可用时，此宏及所有装饰器为空操作          - 自定义 JSON key
 *   REQUIRED(field)                   - 反序列化时必需
 *   REQUIRED_ALIAS(field, "json_key")          - 自定义 JSON key
 *   REQUIRED(field)                   - 反序列化时必需
 *   REQUIRED_ALIAS(field, "json_key") - 必需 + 自定义 key
 *   HICAL_IGNORE(field)               - 序列化/反序列化均跳过
 *   MIN(field, val)                   - 数值最小值校验
 *   MAX(field, val)                   - 数值最大值校验
 *   NOT_EMPTY(field)                  - 字符串非空校验
 *   PATTERN(field, "re")              - 正则匹配校验
 *   LENGTH(field, min, max)           - 字符串长度范围校验
 * 当 C++26 反射可用时，此宏及所有装饰器为空操作 - 必需 + 自定义 key
 *   HICAL_IGNORE(field)               - 序列化/反序列化均跳过
 * 当 C++26 反射可用时，此宏及所有装饰器为空操作。
 */
#if !HICAL_HAS_REFLECTION

	// ---- 装饰器宏（展开为括号元组，对 FOR_EACH 仍是单个 token）----

	// NOLINTBEGIN(cppcoreguidelines-macro-usage)
	#define ALIAS(field, alias) (hical_alias_, field, alias)
	#define REQUIRED(field) (hical_required_, field)
	#define REQUIRED_ALIAS(field, alias) (hical_required_alias_, field, alias)

	#define HICAL_IGNORE(field) (hical_ignore_, field)

	// 校验约束装饰器
	#define MIN(field, val) (hical_min_, field, val)
	#define MAX(field, val) (hical_max_, field, val)
	#define NOT_EMPTY(field) (hical_not_empty_, field)
	#define PATTERN(field, re) (hical_pattern_, field, re)
	#define LENGTH(field, minLen, maxLen) (hical_length_, field, minLen, maxLen)

// ---- IS_PAREN 检测（区分裸字段名 vs 括号装饰器）----

	#define HICAL_IS_PAREN_PROBE_(...) ~, 1,
	#define HICAL_IS_PAREN_CHECK_(...) HICAL_IS_PAREN_CHECK_N_(__VA_ARGS__, 0)
	#define HICAL_IS_PAREN_CHECK_N_(x, n, ...) n
	#define HICAL_IS_PAREN_(x) HICAL_IS_PAREN_CHECK_(HICAL_IS_PAREN_PROBE_ x)

// ---- Token 拼接辅助 ----

	#define HICAL_JSON_PASTE2_(a, b) a##b
	#define HICAL_JSON_PASTE_(a, b) HICAL_JSON_PASTE2_(a, b)

// ---- 叶子宏分派 ----

	#define HICAL_JSON_FIELD_(T, arg) HICAL_JSON_PASTE_(HICAL_JSON_LEAF_, HICAL_IS_PAREN_(arg))(T, arg)

	// 字段存在性校验 + makeField 构造（公共骨架）
	#define HICAL_JSON_MAKE_FIELD_(T, field, ...)                                                          \
		(                                                                                                  \
			[]()                                                                                           \
			{                                                                                              \
				static_assert(                                                                             \
					requires { std::declval<T>().field; },                                                 \
					"HICAL_JSON: field '" #field "' does not exist or is not publicly accessible in " #T); \
				return ::hical::meta::detail::makeField<T>(__VA_ARGS__);                                   \
			}())

	// 裸字段：name → makeField("name", &T::name)
	#define HICAL_JSON_LEAF_0(T, field) HICAL_JSON_MAKE_FIELD_(T, field, #field, &T::field)

	// 括号装饰 → 解包括号 → 提取 tag → 分派到对应 TAG 处理器
	// HICAL_JSON_DEPAREN_ 剥离外层括号：(a, b, c) → a, b, c
	#define HICAL_JSON_DEPAREN_(...) __VA_ARGS__
	#define HICAL_JSON_LEAF_1(T, tagged) HICAL_JSON_UNWRAP_(T, HICAL_JSON_DEPAREN_ tagged)
	#define HICAL_JSON_UNWRAP_(T, ...) HICAL_JSON_TAG_DISPATCH_(T, __VA_ARGS__)
	#define HICAL_JSON_TAG_DISPATCH_(T, tag, ...) HICAL_JSON_PASTE_(HICAL_JSON_TAG_, tag)(T, __VA_ARGS__)

// ---- Tag 处理器 ----

	#define HICAL_JSON_TAG_hical_alias_(T, field, alias) HICAL_JSON_MAKE_FIELD_(T, field, alias, &T::field)
	#define HICAL_JSON_TAG_hical_required_(T, field) HICAL_JSON_MAKE_FIELD_(T, field, #field, &T::field, true, false)
	#define HICAL_JSON_TAG_hical_required_alias_(T, field, alias) \
		HICAL_JSON_MAKE_FIELD_(T, field, alias, &T::field, true, false)
	#define HICAL_JSON_TAG_hical_ignore_(T, field) HICAL_JSON_MAKE_FIELD_(T, field, #field, &T::field, false, true)

	// 校验约束 Tag 处理器
	#define HICAL_JSON_TAG_hical_min_(T, field, val) \
		HICAL_JSON_MAKE_FIELD_(T, field, #field, &T::field, false, false, val, {}, {}, false, {}, {})
	#define HICAL_JSON_TAG_hical_max_(T, field, val) \
		HICAL_JSON_MAKE_FIELD_(T, field, #field, &T::field, false, false, {}, val, {}, false, {}, {})
	#define HICAL_JSON_TAG_hical_not_empty_(T, field) \
		HICAL_JSON_MAKE_FIELD_(T, field, #field, &T::field, false, false, {}, {}, {}, true, {}, {})
	#define HICAL_JSON_TAG_hical_pattern_(T, field, re) \
		HICAL_JSON_MAKE_FIELD_(T, field, #field, &T::field, false, false, {}, {}, re, false, {}, {})
	#define HICAL_JSON_TAG_hical_length_(T, field, minLen, maxLen) \
		HICAL_JSON_MAKE_FIELD_(T, field, #field, &T::field, false, false, {}, {}, {}, false, minLen, maxLen)

// ---- __VA_OPT__ 递归展开（无字段数量限制）----

	#define HICAL_JSON_PARENS_ ()

	#define HICAL_JSON_FOR_EACH_(macro, T, a, ...) \
		macro(T, a) __VA_OPT__(, HICAL_JSON_FE_AGAIN_ HICAL_JSON_PARENS_(macro, T, __VA_ARGS__))

	#define HICAL_JSON_FE_AGAIN_() HICAL_JSON_FOR_EACH_

	// 多层 EXPAND 解锁递归深度（支持最多 243 字段）
	#define HICAL_JSON_EXPAND_(...) HICAL_JSON_EXP4_(HICAL_JSON_EXP4_(__VA_ARGS__))
	#define HICAL_JSON_EXP4_(...) HICAL_JSON_EXP3_(HICAL_JSON_EXP3_(__VA_ARGS__))
	#define HICAL_JSON_EXP3_(...) HICAL_JSON_EXP2_(HICAL_JSON_EXP2_(__VA_ARGS__))
	#define HICAL_JSON_EXP2_(...) HICAL_JSON_EXP1_(HICAL_JSON_EXP1_(__VA_ARGS__))
	#define HICAL_JSON_EXP1_(...) __VA_ARGS__

// ---- 入口宏 ----

	#define HICAL_JSON(Type, ...)                                                                                \
		static const auto& hicalJsonFields()                                                                     \
		{                                                                                                        \
			static const auto fields =                                                                           \
				std::make_tuple(HICAL_JSON_EXPAND_(HICAL_JSON_FOR_EACH_(HICAL_JSON_FIELD_, Type, __VA_ARGS__))); \
			return fields;                                                                                       \
		}

// NOLINTEND(cppcoreguidelines-macro-usage)

#else
// C++26 反射模式下，所有宏为空操作
	#define HICAL_JSON(Type, ...)
	#define ALIAS(field, alias)
	#define REQUIRED(field)
	#define REQUIRED_ALIAS(field, alias)
	#define HICAL_IGNORE(field)
#endif
