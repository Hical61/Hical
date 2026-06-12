/**
 * @file OpenApiSchema.h
 * @brief OpenAPI 3.0 JSON Schema 自动生成（C++20 回退实现）
 * 从 HICAL_JSON() 宏生成的 FieldDescriptor tuple 中提取类型信息，
 * 自动生成符合 OpenAPI 3.0 / JSON Schema 规范的 schema 对象。
 * 对外 API：
 *   auto schema = hical::meta::openapi::jsonSchema<MyDTO>();
 *   // -> {"type":"object","properties":{...},"required":[...]}
 * 类型名注册（用于 $ref）：
 *   HICAL_SCHEMA_NAME(MyDTO, "MyDTO")
 */

#pragma once

#include "Reflection.h"
#include "MetaJson.h"
#include <boost/json.hpp>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace hical::meta::openapi
{

	// ============ Schema 类型名注册 ============

	/**
	 * @brief 类型名特化模板，决定嵌套类型使用 $ref 还是内联
	 * 当 SchemaName<T>::value != nullptr 时，生成 $ref 引用
	 */
	template <typename T>
	struct SchemaName
	{
		static constexpr const char* value = nullptr;
	};

	// ============ 前向声明 ============

	template <typename T>
	boost::json::object jsonSchema();

	// ============ 编译期类型到 Schema 映射 ============

	namespace detail
	{

		/**
		 * @brief 基本类型 → OpenAPI Schema
		 */
		template <typename T>
		boost::json::object typeToSchema()
		{
			boost::json::object prop;

			if constexpr (std::is_same_v<T, std::string>)
			{
				prop["type"] = "string";
			}
			// bool 得放 is_integral_v 前面，否则 C++ 拿 bool 也当整形处理了
			else if constexpr (std::is_same_v<T, bool>)
			{
				prop["type"] = "boolean";
			}
			else if constexpr (std::is_integral_v<T>)
			{
				prop["type"] = "integer";
				if constexpr (sizeof(T) <= 4)
				{
					prop["format"] = "int32";
				}
				else
				{
					prop["format"] = "int64";
				}
				if constexpr (std::is_unsigned_v<T>)
				{
					prop["minimum"] = 0;
				}
			}
			else if constexpr (std::is_same_v<T, float>)
			{
				prop["type"] = "number";
				prop["format"] = "float";
			}
			else if constexpr (std::is_floating_point_v<T>)
			{
				prop["type"] = "number";
				prop["format"] = "double";
			}
			else if constexpr (IsVector<T>::value)
			{
				using ElemType = typename T::value_type;
				prop["type"] = "array";
				prop["items"] = typeToSchema<ElemType>();
			}
			else if constexpr (HasJsonFields<T>::value)
			{
				// 有 SchemaName → 生成 $ref；否则内联展开
				if constexpr (SchemaName<T>::value != nullptr)
				{
					prop["$ref"] = std::string("#/components/schemas/") + SchemaName<T>::value;
				}
				else
				{
					// 内联展开完整 schema
					return openapi::jsonSchema<T>();
				}
			}
			else
			{
				static_assert(sizeof(T) == 0, "Unsupported type for OpenAPI schema generation");
			}

			return prop;
		}

		/**
		 * @brief 遍历 FieldDescriptor tuple 构建 properties 和 required 数组
		 * 复用 MetaJson.h serializeFields 已验证的 fold expression 模式
		 */
		template <typename T, typename Tuple, size_t... I>
		void buildProperties(boost::json::object& props,
							 boost::json::array& required,
							 const Tuple& fields,
							 std::index_sequence<I...>)
		{
			auto processOne = [&](const auto& field)
			{
				if (field.ignored)
				{
					return;
				}
				// 从成员指针推导字段类型
				using FieldType = std::remove_reference_t<decltype(std::declval<T>().*(field.pointer))>;
				props[field.name] = typeToSchema<FieldType>();
				if (field.required)
				{
					required.push_back(boost::json::value(std::string(field.name)));
				}
			};
			(processOne(std::get<I>(fields)), ...);
		}

	} // namespace detail

	// ============ 公共 API ============

	/**
	 * @brief 从 HICAL_JSON 标注的结构体自动生成 JSON Schema
	 * @tparam T 标注了 HICAL_JSON() 的结构体类型
	 * @return boost::json::object 符合 OpenAPI 3.0 Schema Object 规范
	 * 类型映射：
	 *   std::string        → {"type":"string"}
	 *   bool               → {"type":"boolean"}
	 *   int/int32_t        → {"type":"integer","format":"int32"}
	 *   int64_t            → {"type":"integer","format":"int64"}
	 *   uint64_t           → {"type":"integer","format":"int64","minimum":0}
	 *   float              → {"type":"number","format":"float"}
	 *   double             → {"type":"number","format":"double"}
	 *   std::vector<T>     → {"type":"array","items":{...}}
	 *   嵌套 HasJsonFields → $ref 或内联 object
	 */
	template <typename T>
	boost::json::object jsonSchema()
	{
		static_assert(HasJsonFields<T>::value, "Type must use HICAL_JSON() macro for schema generation");

		boost::json::object schema;
		schema["type"] = "object";

		boost::json::object properties;
		boost::json::array requiredFields;

		const auto& fields = T::hicalJsonFields();
		constexpr auto count = std::tuple_size_v<std::remove_cvref_t<decltype(fields)>>;
		detail::buildProperties<T>(properties, requiredFields, fields, std::make_index_sequence<count> {});

		schema["properties"] = std::move(properties);
		if (!requiredFields.empty())
		{
			schema["required"] = std::move(requiredFields);
		}

		return schema;
	}

	/**
	 * @brief 递归收集类型及其嵌套类型的 schema（供 components/schemas 使用）
	 * 仅收集有 SchemaName 的类型到 map 中
	 * @param schemas 输出 map：schema名称 → schema对象
	 */
	template <typename T>
	void collectSchemas(std::unordered_map<std::string, boost::json::object>& schemas)
	{
		static_assert(HasJsonFields<T>::value, "Type must use HICAL_JSON() macro for schema collection");

		// 注册自身（如果有名称）
		if constexpr (SchemaName<T>::value != nullptr)
		{
			std::string name = SchemaName<T>::value;
			if (schemas.find(name) == schemas.end())
			{
				schemas[name] = jsonSchema<T>();
			}
		}

		// 递归收集嵌套类型
		const auto& fields = T::hicalJsonFields();
		constexpr auto count = std::tuple_size_v<std::remove_cvref_t<decltype(fields)>>;

		[&]<size_t... I>(std::index_sequence<I...>)
		{
			auto collectNested = [&](const auto& field)
			{
				if (field.ignored)
				{
					return;
				}
				using FieldType = std::remove_reference_t<decltype(std::declval<T>().*(field.pointer))>;

				if constexpr (HasJsonFields<FieldType>::value)
				{
					collectSchemas<FieldType>(schemas);
				}
				else if constexpr (IsVector<FieldType>::value)
				{
					using ElemType = typename FieldType::value_type;
					if constexpr (HasJsonFields<ElemType>::value)
					{
						collectSchemas<ElemType>(schemas);
					}
				}
			};
			(collectNested(std::get<I>(fields)), ...);
		}(std::make_index_sequence<count> {});
	}

	/**
	 * @brief 批量注册多个类型的 schema
	 * 用法：registerSchemas<UserDTO, OrderDTO, ProductDTO>(schemas);
	 */
	template <typename... Types>
	void registerSchemas(std::unordered_map<std::string, boost::json::object>& schemas)
	{
		(collectSchemas<Types>(schemas), ...);
	}

} // namespace hical::meta::openapi

// ============ 用户宏 ============

/**
 * @brief 为结构体注册 OpenAPI schema 名称（用于 $ref 引用）
 * 用法：
 *   struct UserDTO { ... HICAL_JSON(UserDTO, ...) };
 *   HICAL_SCHEMA_NAME(UserDTO, "UserDTO")
 * 注册后，嵌套引用此类型时将生成 {"$ref":"#/components/schemas/UserDTO"}
 * 而非内联展开完整 schema
 */
// NOLINTBEGIN(cppcoreguidelines-macro-usage)
#define HICAL_SCHEMA_NAME(Type, Name)              \
	template <>                                    \
	struct hical::meta::openapi::SchemaName<Type>  \
	{                                              \
		static constexpr const char* value = Name; \
	};
// NOLINTEND(cppcoreguidelines-macro-usage)
