/**
 * @file OpenApiSchema.h
 * @brief 生成 OpenAPI 3.0 Schema
 * 原有入口沿用 HICAL_JSON() 的 Schema 注册和引用方式。两条反射路径都可以分别生成输入和输出契约。
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
#include <cstdint>
#include <limits>
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
				if (!field.active || field.ignored)
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

		enum class ContractDirection
		{
			hInput,
			hOutput
		};

		using hical::meta::detail::JsonFieldRules;

		/**
		 * @brief 判断当前反射路径是否按普通对象读写该类型
		 * @tparam T 对象类型
		 * @return 读写都按对象字段处理时返回 true
		 */
		template <typename T>
		consteval bool isContractDto()
		{
			if constexpr (std::is_class_v<T> && std::is_default_constructible_v<T> && std::is_move_constructible_v<T>)
			{
#if HICAL_HAS_REFLECTION
				if constexpr (std::is_aggregate_v<T>)
				{
					// Boost 能接管读写的类型走另一条路径。显式 ADL 转换另行检查。
					return !boost::json::has_value_from<T>::value && !boost::json::has_value_to<T>::value;
				}
#else
				return HasJsonFields<T>::value;
#endif
			}
			return false;
		}

#if HICAL_HAS_REFLECTION
		/**
		 * @brief 检查成员是否有共同 Schema 无法描述的自定义处理
		 * @tparam Member 成员反射信息
		 * @return 找到适用于该成员的读写、View 或 Schema 注解时返回 true
		 */
		template <std::meta::info Member>
		consteval bool hasContractMemberCustomization()
		{
			if constexpr (anno::hasViewAnnotations<Member>() || anno::hasDeserializeAnnotations<Member>())
			{
				return true;
			}
			// clang-format off
			using MemberType = [:std::meta::remove_cvref(std::meta::type_of(Member)):];
			constexpr static auto kAnnotations = std::define_static_array(anno::annotationsOfMemberWithParent(Member));
			template for (constexpr auto info : kAnnotations)
			{
				constexpr auto annotation = [:std::meta::constant_of(info):];
				if constexpr (requires(const MemberType& value, std::optional<boost::json::value>& result)
				{
					{ annotation.applyAnnotationSerialize(value, result) } -> std::same_as<void>;
				} || requires(boost::json::object& schema)
				{
					{ annotation.applyAnnotationSchema(schema, std::type_identity<MemberType> {}) } -> std::same_as<void>;
				})
				{
					return true;
				}
			}
			// clang-format on
			return false;
		}
#endif

		template <typename T, typename... Parents>
		boost::json::object projectContractType(ContractDirection direction, const JsonFieldRules& rules);

		template <typename FieldType>
		void validateContractAssignment(ContractDirection direction, std::string_view name)
		{
			if constexpr (!std::is_assignable_v<FieldType&, FieldType>)
			{
				if (direction == ContractDirection::hInput)
				{
					hical::meta::detail::throwFieldDefinitionError(name, "共同输入契约要求字段可赋值");
				}
			}
		}

		/**
		 * @brief 用当前反射路径的可见字段生成内联对象 Schema
		 * @tparam T DTO 类型
		 * @tparam Parents 当前路径上已处理的对象类型，用来识别递归定义
		 * @param direction 输入或输出方向
		 * @return 该对象的 Schema
		 */
		template <typename T, typename... Parents>
		boost::json::object projectContractObject(ContractDirection direction)
		{
			boost::json::object properties;
			boost::json::array required;
			const auto append = [&]<typename FieldType>(const JsonFieldRules& rules)
			{
				validateContractAssignment<FieldType>(direction, rules.name);
				properties[rules.name] =
					projectContractType<std::remove_const_t<FieldType>, Parents..., T>(direction, rules);
				if (rules.required || direction == ContractDirection::hOutput)
				{
					required.push_back(boost::json::value(rules.name));
				}
			};
#if HICAL_HAS_REFLECTION
			constexpr auto ctx = std::meta::access_context::unprivileged();
			hical::meta::detail::validateNativeJsonFields<T, CollectMember::collectNonstaticMemberInfos, ctx>();
			constexpr static auto kMembers =
				std::define_static_array(CollectMember::collectNonstaticMemberInfos(^^T, ctx));
			// clang-format off
			template for (constexpr auto member : kMembers)
			{
				constexpr auto rules = anno::applyJsonFieldAnnotations<member>();
				if constexpr (!rules.ignored)
				{
					using FieldType = [:std::meta::type_of(member):];
					if constexpr (std::meta::is_bit_field(member)
						|| std::is_reference_v<FieldType> || std::is_volatile_v<FieldType>
						|| std::is_array_v<FieldType>)
					{
						hical::meta::detail::throwFieldDefinitionError(rules.name, "共同契约不支持位域、引用、volatile 或 C 数组字段");
					}
					else if constexpr (hasContractMemberCustomization<member>())
					{
						hical::meta::detail::throwFieldDefinitionError(rules.name, "共同契约不支持成员转换、View 或 Schema 定制");
					}
					else
					{
						append.template operator()<FieldType>(rules);
					}
				}
			}
			// clang-format on
#else
			const auto appendMacroField = [&](const auto& field)
			{
				using FieldType = typename std::remove_cvref_t<decltype(field)>::ValueType;
				// 宏的读写代码在运行时跳过忽略和重复字段，编译时仍会实例化成员指针访问。
				if constexpr (!requires(const T& obj) { obj.*field.pointer; })
				{
					hical::meta::detail::throwFieldDefinitionError(field.name,
																   "共同契约要求宏声明的字段能通过 DTO 访问");
				}
				else if (field.ignored)
				{
					// 宏也会为忽略字段实例化转换。这里只接收能直接确认可转换的标量。
					if constexpr (std::is_volatile_v<FieldType>
								  || !(std::is_arithmetic_v<FieldType>
									   || std::is_same_v<std::remove_const_t<FieldType>, std::string>))
					{
						hical::meta::detail::throwFieldDefinitionError(field.name,
																	   "宏轨的共同契约只允许忽略算术类型或字符串字段");
					}
					validateContractAssignment<FieldType>(direction, field.name);
				}
				else if (field.active)
				{
					append.template operator()<FieldType>(hical::meta::detail::toJsonFieldRules(field));
				}
			};
			std::apply(
				[&](const auto&... fields)
				{
					(appendMacroField(fields), ...);
				},
				T::hicalJsonFields());
#endif
			boost::json::object schema {{"type", "object"}, {"properties", std::move(properties)}};
			if (!required.empty())
			{
				schema["required"] = std::move(required);
			}
			return schema;
		}

		/**
		 * @brief 为共同契约支持的字段类型生成 Schema
		 * @tparam T 字段类型
		 * @tparam Parents 当前路径上已处理的对象类型
		 * @param direction 输入或输出方向
		 * @param rules 字段的共同规则
		 * @return 字段的内联 Schema
		 * @throws std::logic_error 类型、字段规则或转换超出支持范围
		 */
		template <typename T, typename... Parents>
		boost::json::object projectContractType(ContractDirection direction, const JsonFieldRules& rules)
		{
			if constexpr ((std::is_same_v<T, Parents> || ...))
			{
				hical::meta::detail::throwFieldDefinitionError(rules.name, "共同契约不支持递归 DTO");
			}
			else if constexpr (hical::meta::detail::HasExplicitJsonValueFrom<T>
							   || hical::meta::detail::HasExplicitJsonValueTo<T>)
			{
				hical::meta::detail::throwFieldDefinitionError(rules.name, "共同契约不支持显式 JSON 转换");
			}
			else if constexpr (std::is_same_v<T, std::string>)
			{
				boost::json::object schema {{"type", "string"}};
				if (direction == ContractDirection::hInput && rules.notEmpty)
				{
					schema["minLength"] = 1;
				}
				return schema;
			}
			// bool 排在整型判断前面，跟上面 typeToSchema 的顺序保持一致
			else if constexpr (std::is_same_v<T, bool>)
			{
				return boost::json::object {{"type", "boolean"}};
			}
			else if constexpr (std::is_same_v<T, std::int32_t>)
			{
				const auto minimum = std::numeric_limits<std::int32_t>::min();
				const auto maximum = std::numeric_limits<std::int32_t>::max();
				const bool isInput = direction == ContractDirection::hInput;
				return {{"type", "integer"},
						{"format", "int32"},
						{"minimum", isInput ? rules.minimum.value_or(minimum) : minimum},
						{"maximum", isInput ? rules.maximum.value_or(maximum) : maximum}};
			}
			else if constexpr (IsVector<T>::value)
			{
				using Element = typename T::value_type;
				if constexpr (!std::is_same_v<T, std::vector<Element>>)
				{
					hical::meta::detail::throwFieldDefinitionError(rules.name,
																   "共同契约只支持使用默认分配器的 std::vector");
				}
				else if constexpr (isContractDto<Element>())
				{
					return {{"type", "array"}, {"items", projectContractType<Element, Parents...>(direction, rules)}};
				}
				else
				{
					hical::meta::detail::throwFieldDefinitionError(rules.name, "共同契约的列表元素必须是普通 DTO");
				}
			}
			else if constexpr (isContractDto<T>() && std::is_same_v<T, std::remove_cvref_t<T>>)
			{
				return projectContractObject<T, Parents...>(direction);
			}
			else
			{
				hical::meta::detail::throwFieldDefinitionError(rules.name, "共同契约不支持此字段类型");
			}
		}

	} // namespace detail

	// ============ 公共 API ============

	/**
	 * @brief 为普通 JSON 解码生成输入契约，列出显式必填字段和支持的输入规则
	 * @tparam T 字符串、int32、bool、普通 DTO 或 DTO 列表类型
	 * @return 内联的 OpenAPI Schema
	 * @throws std::logic_error 类型或定制超出共同契约范围
	 */
	template <typename T>
	boost::json::object inputSchema()
	{
		return detail::projectContractType<T>(detail::ContractDirection::hInput, {"根类型"});
	}

	/**
	 * @brief 为普通 JSON 序列化生成输出契约，描述会输出的字段及其类型范围
	 * @tparam T 字符串、int32、bool、普通 DTO 或 DTO 列表类型
	 * @return 内联的 OpenAPI Schema
	 * @throws std::logic_error 类型或定制超出共同契约范围
	 */
	template <typename T>
	boost::json::object outputSchema()
	{
		return detail::projectContractType<T>(detail::ContractDirection::hOutput, {"根类型"});
	}

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
				if (!field.active || field.ignored)
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
