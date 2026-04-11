#pragma once

/**
 * @brief 反射驱动的 JSON 自动序列化/反序列化
 *
 * 双路线：
 * - C++26 反射：通过 ^^T 自动枚举数据成员，无需用户标注
 * - C++20 回退：用户使用 HICAL_JSON(StructType, field1, field2, ...) 宏标注字段
 *
 * 对外 API：
 *   boost::json::value json = hical::meta::toJson(myStruct);
 *   auto obj = hical::meta::fromJson<MyStruct>(jsonValue);
 *
 * 支持类型：int, int64_t, double, bool, std::string, std::vector<T>, 嵌套结构体
 */

#include "Reflection.h"
#include "HttpRequest.h"
#include <boost/json.hpp>
#include <stdexcept>
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
			return static_cast<int64_t>(val);
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
				throw std::runtime_error("JSON type mismatch: expected string");
			}
			return std::string(val.as_string());
		}
		else if constexpr (std::is_same_v<T, bool>)
		{
			if (!val.is_bool())
			{
				throw std::runtime_error("JSON type mismatch: expected bool");
			}
			return val.as_bool();
		}
		else if constexpr (std::is_integral_v<T>)
		{
			if (!val.is_int64() && !val.is_uint64())
			{
				throw std::runtime_error("JSON type mismatch: expected integer");
			}
			return static_cast<T>(val.as_int64());
		}
		else if constexpr (std::is_floating_point_v<T>)
		{
			if (!val.is_double() && !val.is_int64())
			{
				throw std::runtime_error("JSON type mismatch: expected number");
			}
			if (val.is_int64())
			{
				return static_cast<T>(val.as_int64());
			}
			return static_cast<T>(val.as_double());
		}
		else if constexpr (IsVector<T>::value)
		{
			if (!val.is_array())
			{
				throw std::runtime_error("JSON type mismatch: expected array");
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
				throw std::runtime_error("JSON type mismatch: expected object");
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
         * @brief 字段描述器：绑定字段名 + 成员指针
         */
		template <typename Class, typename FieldType>
		struct FieldDescriptor
		{
			std::string_view name;
			FieldType Class::* pointer;
		};

		/**
         * @brief 创建字段描述器的辅助函数
         */
		template <typename Class, typename FieldType>
		constexpr FieldDescriptor<Class, FieldType> makeField(std::string_view name, FieldType Class::* ptr)
		{
			return {name, ptr};
		}

		/**
         * @brief 序列化所有字段
         */
		template <typename T, typename Tuple, size_t... I>
		void serializeFields(const T& obj, boost::json::object& jsonObj, const Tuple& fields, std::index_sequence<I...>)
		{
			((jsonObj[std::get<I>(fields).name] = valueToJson(obj.*std::get<I>(fields).pointer)), ...);
		}

		/**
         * @brief 反序列化所有字段
         */
		template <typename T, typename Tuple, size_t... I>
		void deserializeFields(T& obj,
							   const boost::json::object& jsonObj,
							   const Tuple& fields,
							   std::index_sequence<I...>)
		{
			auto trySet = [&](const auto& field)
			{
				auto it = jsonObj.find(field.name);
				if (it != jsonObj.end())
				{
					using FieldType = std::remove_reference_t<decltype(obj.*field.pointer)>;
					obj.*field.pointer = valueFromJson<FieldType>(it->value());
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

		auto fields = T::hicalJsonFields();
		boost::json::object jsonObj;
		constexpr auto count = std::tuple_size_v<decltype(fields)>;
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
			throw std::runtime_error("fromJson: expected JSON object, got " + std::string(to_string(json.kind())));
		}

		T obj {};
		const auto& jsonObj = json.as_object();
		auto fields = T::hicalJsonFields();
		constexpr auto count = std::tuple_size_v<decltype(fields)>;
		detail::deserializeFields(obj, jsonObj, fields, std::make_index_sequence<count> {});
		return obj;
	}

#else // HICAL_HAS_REFLECTION == 1

	// ============ C++26 反射实现 ============

	/**
     * @brief 序列化结构体为 JSON（C++26 反射，无需宏标注）
     */
	template <typename T>
	boost::json::object toJson(const T& obj)
	{
		boost::json::object jsonObj;

		template for (constexpr auto member : std::meta::nonstatic_data_members_of(^^T))
		{
			constexpr auto name = std::meta::identifier_of(member);
			jsonObj[name] = valueToJson(obj.[:member:]);
		}

		return jsonObj;
	}

	/**
     * @brief 从 JSON 反序列化为结构体（C++26 反射，无需宏标注）
     */
	template <typename T>
	T fromJson(const boost::json::value& json)
	{
		T obj {};
		const auto& jsonObj = json.as_object();

		template for (constexpr auto member : std::meta::nonstatic_data_members_of(^^T))
		{
			constexpr auto name = std::meta::identifier_of(member);
			auto it = jsonObj.find(name);
			if (it != jsonObj.end())
			{
				using FieldType = typename[:std::meta::type_of(member):];
				obj.[:member:] = valueFromJson<FieldType>(it->value());
			}
		}

		return obj;
	}

#endif // HICAL_HAS_REFLECTION

	/**
     * @brief 从 HttpRequest 消息体反序列化为指定类型
     * @tparam T 目标类型（需标注 HICAL_JSON 或支持 C++26 反射）
     * @param req HTTP 请求
     * @return T 反序列化后的对象
     * @throws std::runtime_error 当请求体不是合法 JSON 或类型不匹配时
     *
     * 用法：auto user = hical::meta::readJson<UserDTO>(req);
     * 或直接：auto user = req.readJson<UserDTO>();（需 include MetaJson.h）
     */
	template <typename T>
	T readJson(const ::hical::HttpRequest& req)
	{
		auto json = req.jsonBody();
		if (json.is_null())
		{
			throw std::runtime_error("readJson: request body is not valid JSON");
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
     *
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
 *
 * 用法：
 * ```cpp
 * struct UserDTO
 * {
 *     std::string name;
 *     int age;
 *     double score;
 *
 *     HICAL_JSON(UserDTO, name, age, score)
 * };
 * ```
 *
 * 当 C++26 反射可用时，此宏为空操作。
 */
#if !HICAL_HAS_REFLECTION

	// 展开辅助
	#define HICAL_JSON_FIELD_(T, field) ::hical::meta::detail::makeField<T>(#field, &T::field)

	// HICAL_JSON 宏：生成 hicalJsonFields() 静态方法
	// 使用 FOR_EACH 模式遍历字段列表
	#define HICAL_JSON(Type, ...)                                            \
		static auto hicalJsonFields()                                        \
		{                                                                    \
			return std::make_tuple(HICAL_JSON_FOR_EACH_(Type, __VA_ARGS__)); \
		}

	// 可变参数 FOR_EACH 宏（支持 1-16 个字段）
	#define HICAL_JSON_ARG_N_(_1, _2, _3, _4, _5, _6, _7, _8, _9, _10, _11, _12, _13, _14, _15, _16, N, ...) N
	#define HICAL_JSON_NARGS_(...) HICAL_JSON_ARG_N_(__VA_ARGS__, 16, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1)

	#define HICAL_JSON_PASTE2_(a, b) a##b
	#define HICAL_JSON_PASTE_(a, b) HICAL_JSON_PASTE2_(a, b)

	#define HICAL_JSON_FOR_EACH_(T, ...) \
		HICAL_JSON_PASTE_(HICAL_JSON_FE_, HICAL_JSON_NARGS_(__VA_ARGS__))(T, __VA_ARGS__)

	#define HICAL_JSON_FE_1(T, a) HICAL_JSON_FIELD_(T, a)
	#define HICAL_JSON_FE_2(T, a, b) HICAL_JSON_FIELD_(T, a), HICAL_JSON_FIELD_(T, b)
	#define HICAL_JSON_FE_3(T, a, b, c) HICAL_JSON_FE_2(T, a, b), HICAL_JSON_FIELD_(T, c)
	#define HICAL_JSON_FE_4(T, a, b, c, d) HICAL_JSON_FE_3(T, a, b, c), HICAL_JSON_FIELD_(T, d)
	#define HICAL_JSON_FE_5(T, a, b, c, d, e) HICAL_JSON_FE_4(T, a, b, c, d), HICAL_JSON_FIELD_(T, e)
	#define HICAL_JSON_FE_6(T, a, b, c, d, e, f) HICAL_JSON_FE_5(T, a, b, c, d, e), HICAL_JSON_FIELD_(T, f)
	#define HICAL_JSON_FE_7(T, a, b, c, d, e, f, g) HICAL_JSON_FE_6(T, a, b, c, d, e, f), HICAL_JSON_FIELD_(T, g)
	#define HICAL_JSON_FE_8(T, a, b, c, d, e, f, g, h) HICAL_JSON_FE_7(T, a, b, c, d, e, f, g), HICAL_JSON_FIELD_(T, h)
	#define HICAL_JSON_FE_9(T, a, b, c, d, e, f, g, h, i) \
		HICAL_JSON_FE_8(T, a, b, c, d, e, f, g, h), HICAL_JSON_FIELD_(T, i)
	#define HICAL_JSON_FE_10(T, a, b, c, d, e, f, g, h, i, j) \
		HICAL_JSON_FE_9(T, a, b, c, d, e, f, g, h, i), HICAL_JSON_FIELD_(T, j)
	#define HICAL_JSON_FE_11(T, a, b, c, d, e, f, g, h, i, j, k) \
		HICAL_JSON_FE_10(T, a, b, c, d, e, f, g, h, i, j), HICAL_JSON_FIELD_(T, k)
	#define HICAL_JSON_FE_12(T, a, b, c, d, e, f, g, h, i, j, k, l) \
		HICAL_JSON_FE_11(T, a, b, c, d, e, f, g, h, i, j, k), HICAL_JSON_FIELD_(T, l)
	#define HICAL_JSON_FE_13(T, a, b, c, d, e, f, g, h, i, j, k, l, m) \
		HICAL_JSON_FE_12(T, a, b, c, d, e, f, g, h, i, j, k, l), HICAL_JSON_FIELD_(T, m)
	#define HICAL_JSON_FE_14(T, a, b, c, d, e, f, g, h, i, j, k, l, m, n) \
		HICAL_JSON_FE_13(T, a, b, c, d, e, f, g, h, i, j, k, l, m), HICAL_JSON_FIELD_(T, n)
	#define HICAL_JSON_FE_15(T, a, b, c, d, e, f, g, h, i, j, k, l, m, n, o) \
		HICAL_JSON_FE_14(T, a, b, c, d, e, f, g, h, i, j, k, l, m, n), HICAL_JSON_FIELD_(T, o)
	#define HICAL_JSON_FE_16(T, a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p) \
		HICAL_JSON_FE_15(T, a, b, c, d, e, f, g, h, i, j, k, l, m, n, o), HICAL_JSON_FIELD_(T, p)

#else
// C++26 反射模式下，HICAL_JSON 为空操作
	#define HICAL_JSON(Type, ...)
#endif
