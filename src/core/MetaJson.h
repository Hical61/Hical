/**
 * @file MetaJson.h
 * @brief 反射驱动的 JSON 自动序列化/反序列化
 * 双路线：
 * - C++26 反射：通过 ^^T 自动枚举数据成员，无需用户标注
 * - C++20 回退：用户使用 HICAL_JSON(StructType, field1, field2, ...) 宏标注字段
 * 对外 API：
 *   boost::json::object json = hical::meta::toJson(myStruct);
 *   auto obj = hical::meta::fromJson<MyStruct>(jsonValue);
 * 支持类型：int, int64_t, double, bool, std::string, std::vector<T>, 嵌套结构体
 */

#pragma once

#include "Reflection.h"
#include "MetaJsonError.h"
#include "HttpRequest.h"
#include "detail/JsonFieldRules.h"
#include <boost/json.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <optional>
#include <regex>
#include <span>
#include <string>
#include <tuple>
#include <type_traits>
#include <vector>

// CPP26 反射路线用到的头文件
#if HICAL_HAS_REFLECTION
	#include "MetaAnno.h"
	#include "MetaSchema.h"
	#include "Utils/CollectMember.h"
#endif

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

	namespace detail
	{
		/**
		 * @brief 通过 ADL 查找使用 Boost.JSON 公开标签的显式输出转换
		 * @tparam T 待转换类型，Boost 的通用容器转换不计入
		 */
		template <typename T>
		concept HasExplicitJsonValueFrom = requires(boost::json::value& output, const std::remove_cvref_t<T>& value) {
			tag_invoke(boost::json::value_from_tag {}, output, value);
		};

		/**
		 * @brief 通过 ADL 查找使用 Boost.JSON 公开标签的显式输入转换，包括非抛出式转换
		 * @tparam T 待转换类型
		 */
		template <typename T>
		concept HasExplicitJsonValueTo = requires(const boost::json::value& input) {
			tag_invoke(boost::json::value_to_tag<std::remove_cvref_t<T>> {}, input);
		} || requires(const boost::json::value& input) {
			tag_invoke(boost::json::try_value_to_tag<std::remove_cvref_t<T>> {}, input);
		};

		/**
		 * @brief 按原始整数值检查范围，再转换成目标类型
		 * @param value Boost.JSON 存储的有符号或无符号整数
		 * @return 转换后的整数
		 * @throws std::runtime_error 负数转无符号或数值超出目标范围
		 */
		template <typename T, typename Integer>
		T checkedJsonIntegerCast(Integer value)
		{
			if constexpr (std::is_signed_v<Integer> && std::is_unsigned_v<T>)
			{
				if (value < 0)
				{
					throwIntegerOutOfRange();
				}
			}
			if constexpr (std::numeric_limits<T>::digits < std::numeric_limits<Integer>::digits)
			{
				if (value > static_cast<Integer>((std::numeric_limits<T>::max)()))
				{
					throwIntegerOutOfRange();
				}
				if constexpr (std::is_signed_v<Integer> && std::is_signed_v<T>)
				{
					if (value < static_cast<Integer>((std::numeric_limits<T>::min)()))
					{
						throwIntegerOutOfRange();
					}
				}
			}
			return static_cast<T>(value);
		}
	} // namespace detail

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
	 * @param val 原始 JSON 值
	 * @return 转换后的 C++ 值
	 * @throws std::runtime_error JSON 类型不匹配或整数超出目标范围
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
			if (const auto* integer = val.if_int64())
			{
				return detail::checkedJsonIntegerCast<T>(*integer);
			}
			if (const auto* integer = val.if_uint64())
			{
				return detail::checkedJsonIntegerCast<T>(*integer);
			}
			detail::throwTypeMismatch("integer");
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
		 * @brief 首次归并要用到的字段名、可见性和校验规则
		 */
		struct FieldOptions
		{
			std::string_view name;
			bool required = false;
			bool ignored = false;
			// 校验约束
			std::optional<double> minVal;
			std::optional<double> maxVal;
			std::optional<std::string> pattern;
			bool notEmpty = false;
			std::optional<size_t> lengthMin;
			std::optional<size_t> lengthMax;
			std::string_view memberName;
			bool explicitAlias = false;
			bool active = true;
			std::vector<std::string> additionalPatterns {};
			// 正则缓存：定义期（hicalJsonFields 的 static 初始化）编译好，读入时只匹配不编译。
			// 这个阶段在静态对象发布之前，天然串行；发布之后这两个成员只读，不再写。
			mutable std::optional<std::regex> compiledPattern;
			mutable std::vector<std::regex> compiledAdditionalPatterns {};
		};

		template <typename Class, typename FieldType>
		struct FieldDescriptor : FieldOptions
		{
			using ValueType = FieldType;
			FieldType Class::* pointer;
		};

		/**
		 * @brief 创建字段描述器的辅助函数（2 参数，向后兼容）
		 * @note 描述器里带正则缓存（std::regex 不是字面类型），所以不能是 constexpr
		 */
		template <typename Class, typename FieldType>
		FieldDescriptor<Class, FieldType> makeField(std::string_view name, FieldType Class::* ptr)
		{
			return {{name, false, false, {}, {}, {}, false, {}, {}, name, false, true, {}, {}, {}}, ptr};
		}

		/**
		 * @brief 创建字段描述器的辅助函数（4 参数，支持 required/ignored）
		 */
		template <typename Class, typename FieldType>
		FieldDescriptor<Class, FieldType> makeField(std::string_view name,
													FieldType Class::* ptr,
													bool isRequired,
													bool isIgnored)
		{
			return {{name, isRequired, isIgnored, {}, {}, {}, false, {}, {}, name, false, true, {}, {}, {}}, ptr};
		}

		/**
		 * @brief 创建字段描述器的辅助函数（满参数，支持校验约束）
		 */
		template <typename Class, typename FieldType>
		FieldDescriptor<Class, FieldType> makeField(std::string_view name,
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
			return {{name,
					 isRequired,
					 isIgnored,
					 minVal,
					 maxVal,
					 std::move(pattern),
					 notEmpty,
					 lengthMin,
					 lengthMax,
					 name,
					 false,
					 true,
					 {},
					 {},
					 {}},
					ptr};
		}

		/**
		 * @brief 生成共同契约时，把宏声明的边界转换成 int32
		 * @param name 最终 JSON 键
		 * @param bound 宏描述器原有的 double 边界
		 * @return 转换后的整数边界。未声明边界时返回空值
		 * @throws std::logic_error 边界不在共同契约支持范围内
		 */
		inline std::optional<std::int32_t> toJsonInt32RuleBound(std::string_view name,
																const std::optional<double>& bound)
		{
			if (!bound)
			{
				return {};
			}
			if (!std::isfinite(*bound) || std::trunc(*bound) != *bound
				|| *bound < (std::numeric_limits<std::int32_t>::min)()
				|| *bound > (std::numeric_limits<std::int32_t>::max)())
			{
				throwFieldDefinitionError(name, "共同契约的数值边界必须是 int32 范围内的整数");
			}
			return static_cast<std::int32_t>(*bound);
		}

		/**
		 * @brief 生成契约时，从归并后的宏字段提取共同规则
		 * 读入数据时直接用描述器里的规则校验。
		 * @param field 归并后的有效字段，字段名引用静态字符串
		 * @return JSON 键、可见性及共同契约支持的输入规则
		 * @throws std::logic_error 共同契约不支持声明的字段约束
		 */
		template <typename FieldDesc>
		JsonFieldRules toJsonFieldRules(const FieldDesc& field)
		{
			JsonFieldRules rules {field.name, field.required, field.ignored};
			if (field.ignored)
			{
				return rules;
			}
			if (field.pattern || !field.additionalPatterns.empty() || field.lengthMin || field.lengthMax)
			{
				throwFieldDefinitionError(field.name, "共同契约暂不支持长度或正则约束");
			}
			using FieldType = typename FieldDesc::ValueType;
			if constexpr (std::is_same_v<FieldType, std::string>)
			{
				if (field.minVal || field.maxVal)
				{
					throwFieldDefinitionError(field.name, "共同契约不支持给字符串设置数值范围");
				}
				rules.notEmpty = field.notEmpty;
			}
			else
			{
				if (field.notEmpty)
				{
					throwFieldDefinitionError(field.name, "共同契约只支持对字符串设置非空约束");
				}
				if constexpr (std::is_same_v<FieldType, std::int32_t>)
				{
					rules.minimum = toJsonInt32RuleBound(field.name, field.minVal);
					rules.maximum = toJsonInt32RuleBound(field.name, field.maxVal);
				}
				else if (field.minVal || field.maxVal)
				{
					throwFieldDefinitionError(field.name, "共同契约只支持对 int32 字段设置数值范围");
				}
			}
			validateJsonFieldRuleDefinition(rules);
			return rules;
		}

		enum class FieldConstraintKind
		{
			hOther,
			hNumeric,
			// 64 位整型单独标出来：double 只能精确表示 ±2^53 内的整数，
			// 边界超出这个范围或者带小数，实际比较时就不是声明者想要的意思了
			hWideNumeric,
			hString
		};

		/// double 能精确表示的最大连续整数是 2^53
		inline constexpr double kMaxExactDoubleInteger = 9007199254740992.0;

		/**
		 * @brief 检查 64 位整型字段的数值边界能不能被 double 精确表示
		 * 边界先转成 double 存，精度不够就说明约束本身表达不出意图，直接报错。
		 * @param field 带数值边界的描述器
		 * @throws std::logic_error 边界超出 ±2^53 或不是整数
		 */
		inline void validateWideNumericBounds(const FieldOptions& field)
		{
			const auto checkBound = [&](const std::optional<double>& bound)
			{
				if (!bound)
				{
					return;
				}
				if (std::trunc(*bound) != *bound || *bound > kMaxExactDoubleInteger || *bound < -kMaxExactDoubleInteger)
				{
					throwFieldDefinitionError(field.memberName, "64 位整型的数值边界必须是 ±2^53 以内的整数");
				}
			};
			checkBound(field.minVal);
			checkBound(field.maxVal);
		}

		/**
		 * @brief 定义期编译正则并存入缓存，顺手把写错的正则报出来
		 * @param field 待编译的描述器
		 * @throws std::logic_error 正则表达式无法编译
		 */
		inline void compileFieldPatterns(const FieldOptions& field)
		{
			field.compiledPattern.reset();
			field.compiledAdditionalPatterns.clear();
			if (field.pattern)
			{
				try
				{
					field.compiledPattern.emplace(*field.pattern, std::regex::ECMAScript);
				}
				catch (const std::regex_error&)
				{
					throwFieldDefinitionError(field.memberName, "正则表达式无法编译: " + *field.pattern);
				}
			}
			field.compiledAdditionalPatterns.reserve(field.additionalPatterns.size());
			for (const auto& pattern : field.additionalPatterns)
			{
				try
				{
					field.compiledAdditionalPatterns.emplace_back(pattern, std::regex::ECMAScript);
				}
				catch (const std::regex_error&)
				{
					throwFieldDefinitionError(field.memberName, "正则表达式无法编译: " + pattern);
				}
			}
		}

		/**
		 * @brief 在读取输入前检查该字段类型支持的约束
		 * 顺带把正则编译进缓存。类型和约束错配由宏层的 static_assert 拦截；
		 * 直接构造描述器的代码（比如测试）不受这个限制，仍然保持静默忽略。
		 * @param field 待检查的描述器
		 * @param kind 成员类型支持的约束种类
		 * @throws std::logic_error 数值边界为 NaN 或无穷大、约束区间为空，或正则无法编译
		 */
		inline void validateFieldDefinition(const FieldOptions& field, FieldConstraintKind kind)
		{
			if (kind == FieldConstraintKind::hNumeric || kind == FieldConstraintKind::hWideNumeric)
			{
				if ((field.minVal && !std::isfinite(*field.minVal)) || (field.maxVal && !std::isfinite(*field.maxVal)))
				{
					throwFieldDefinitionError(field.memberName, "数值边界必须有限");
				}
				if (field.minVal && field.maxVal && *field.minVal > *field.maxVal)
				{
					throwFieldDefinitionError(field.memberName, "数值区间为空");
				}
			}
			if (kind == FieldConstraintKind::hWideNumeric)
			{
				validateWideNumericBounds(field);
			}
			if (kind == FieldConstraintKind::hString)
			{
				if ((field.lengthMin && field.lengthMax && *field.lengthMin > *field.lengthMax)
					|| (field.notEmpty && field.lengthMax && *field.lengthMax == 0))
				{
					throwFieldDefinitionError(field.memberName, "字符串长度区间为空");
				}
				// 写缓存发生在字段元数据的 static 初始化里，那会儿还没有别的线程看见这个对象。
				// 发布之后这里只读，validateField 只匹配不再编译。
				compileFieldPatterns(field);
			}
		}

		template <typename Limit>
		void tightenLowerBound(std::optional<Limit>& current, const std::optional<Limit>& incoming)
		{
			if (incoming && (!current || *incoming > *current))
			{
				current = incoming;
			}
		}

		template <typename Limit>
		void tightenUpperBound(std::optional<Limit>& current, const std::optional<Limit>& incoming)
		{
			if (incoming && (!current || *incoming < *current))
			{
				current = incoming;
			}
		}

		/**
		 * @brief 把同一成员的声明合并到第一次出现的位置
		 * @param target 首次出现的成员描述器
		 * @param source 同一成员的后续描述器
		 * @throws std::logic_error 忽略声明和其他声明混用，或显式别名冲突
		 */
		inline void mergeFieldRules(FieldOptions& target, const FieldOptions& source)
		{
			if (target.ignored != source.ignored)
			{
				throwFieldDefinitionError(target.memberName, "IGNORE 不能与该成员的其他声明混用");
			}
			if (source.explicitAlias)
			{
				if (target.explicitAlias && target.name != source.name)
				{
					throwFieldDefinitionError(target.memberName, "同一成员声明了不同的显式别名");
				}
				target.name = source.name;
				target.explicitAlias = true;
			}
			target.required = target.required || source.required;
			target.notEmpty = target.notEmpty || source.notEmpty;
			tightenLowerBound(target.minVal, source.minVal);
			tightenUpperBound(target.maxVal, source.maxVal);
			tightenLowerBound(target.lengthMin, source.lengthMin);
			tightenUpperBound(target.lengthMax, source.lengthMax);
			auto appendPattern = [&](const std::string& pattern)
			{
				if (!target.pattern)
				{
					target.pattern = pattern;
				}
				else if (*target.pattern != pattern
						 && std::find(target.additionalPatterns.begin(), target.additionalPatterns.end(), pattern)
								== target.additionalPatterns.end())
				{
					target.additionalPatterns.push_back(pattern);
				}
			};
			if (source.pattern)
			{
				appendPattern(*source.pattern);
			}
			for (const auto& pattern : source.additionalPatterns)
			{
				appendPattern(pattern);
			}
		}

		struct FieldMergeEntry
		{
			FieldOptions& options;
			const void* pointer;
			const void* pointerType;
			bool (*equalPointers)(const void*, const void*);
			FieldConstraintKind kind;
		};

		template <typename Pointer>
		inline char g_fieldPointerTypeToken = 0;

		template <typename FieldDesc>
		FieldMergeEntry makeFieldMergeEntry(FieldDesc& field)
		{
			using Pointer = decltype(field.pointer);
			using FieldType = std::remove_cvref_t<typename FieldDesc::ValueType>;
			// 64 位整型（含 64 位 Linux 上的 long）单独归一类，边界要查 double 精度
			constexpr bool kWideInteger = std::is_integral_v<FieldType> && sizeof(FieldType) >= 8;
			constexpr auto kind = kWideInteger                             ? FieldConstraintKind::hWideNumeric
								  : std::is_arithmetic_v<FieldType>        ? FieldConstraintKind::hNumeric
								  : std::is_same_v<FieldType, std::string> ? FieldConstraintKind::hString
																		   : FieldConstraintKind::hOther;
			// 类型标签相同才还原成员指针对象的地址，再用 == 比较成员指针。
			return {field,
					&field.pointer,
					&g_fieldPointerTypeToken<Pointer>,
					[](const void* lhs, const void* rhs)
					{
						return *static_cast<const Pointer*>(lhs) == *static_cast<const Pointer*>(rhs);
					},
					kind};
		}

		inline void normalizeFieldEntries(std::span<const FieldMergeEntry> fields)
		{
			for (const auto& field : fields)
			{
				validateFieldDefinition(field.options, field.kind);
			}
			for (size_t index = 0; index < fields.size(); ++index)
			{
				const auto& first = fields[index];
				if (!first.options.active)
				{
					continue;
				}
				for (const auto& next : fields.subspan(index + 1))
				{
					if (next.options.active && first.options.memberName == next.options.memberName
						&& first.pointerType == next.pointerType && first.equalPointers(first.pointer, next.pointer))
					{
						mergeFieldRules(first.options, next.options);
						next.options.active = false;
					}
				}
			}
			for (const auto& field : fields)
			{
				validateFieldDefinition(field.options, field.kind);
			}
			for (size_t index = 0; index < fields.size(); ++index)
			{
				const auto& first = fields[index].options;
				if (!first.active || first.ignored)
				{
					continue;
				}
				for (const auto& next : fields.subspan(index + 1))
				{
					if (next.options.active && !next.options.ignored && first.name == next.options.name)
					{
						throwFieldDefinitionError(first.name, "不同的可见成员使用了同一个 JSON 键");
					}
				}
			}
		}

		template <typename Tuple>
		void normalizeFieldsInPlace(Tuple& fields)
		{
			std::apply(
				[](auto&... field)
				{
					const std::array<FieldMergeEntry, sizeof...(field)> entries {makeFieldMergeEntry(field)...};
					normalizeFieldEntries(entries);
				},
				fields);
		}

		/**
		 * @brief 合并同一成员的声明，保留第一次出现的位置
		 * 后续声明标为 active=false。
		 * @param fields 原始字段 tuple，持有正则等约束字符串
		 * @return 归并后的 tuple
		 * @throws std::logic_error 字段定义冲突
		 */
		template <typename Tuple>
		Tuple normalizeFields(Tuple fields)
		{
			normalizeFieldsInPlace(fields);
			return fields;
		}

		/**
		 * @brief 把首次构造和归并放在独立函数里，便于内联后续的字段访问
		 * @param factory 构造原始字段 tuple 的工厂
		 * @return 归并后的字段 tuple
		 */
		template <typename Factory>
	#if defined(_MSC_VER)
		__declspec(noinline)
	#elif defined(__GNUC__) || defined(__clang__)
		__attribute__((noinline))
	#endif
		auto initializeJsonFields(Factory factory)
		{
			auto fields = factory();
			normalizeFieldsInPlace(fields);
			return fields;
		}

		/**
		 * @brief 序列化归并后的可见字段
		 */
		template <typename T, typename Tuple, size_t... I>
		void serializeFields(const T& obj, boost::json::object& jsonObj, const Tuple& fields, std::index_sequence<I...>)
		{
			auto serializeOne = [&](const auto& field)
			{
				if (field.active && !field.ignored)
				{
					jsonObj[field.name] = valueToJson(obj.*field.pointer);
				}
			};
			(serializeOne(std::get<I>(fields)), ...);
		}

		/**
		 * @brief 匹配一条正则，有缓存就复用，没有就临时编译一次
		 * 缓存正常情况下在定义期就填好了。这里是给直接动描述器、绕过归并流程的代码兜底。
		 * @param value 待校验的字符串
		 * @param pattern 原始正则字符串
		 * @param compiled 已编译的正则，可能为空
		 * @return true 匹配，false 不匹配或正则写错
		 */
		inline bool matchFieldPattern(std::string_view value, std::string_view pattern, const std::regex* compiled)
		{
			if (compiled)
			{
				return detail::validatePattern(value, *compiled);
			}
			return detail::validatePattern(value, pattern);
		}

		/**
		 * @brief 检查字段值是否符合归并后的规则
		 * 调用方已跳过重复声明和忽略字段。
		 * 检查数值上下界、正则、非空和长度范围。
		 */
		template <typename FieldDesc, typename FieldType>
		void validateField(const FieldDesc& field, const FieldType& value)
		{
			if constexpr (std::is_same_v<FieldType, std::int32_t>)
			{
				validateJsonInt32Range(field.name, value, field.minVal, field.maxVal);
			}
			else if constexpr (std::is_arithmetic_v<FieldType>)
			{
				if (field.minVal && static_cast<double>(value) < *field.minVal)
				{
					detail::throwValidationErrorNum(field.name, "minimum", *field.minVal);
				}
				if (field.maxVal && static_cast<double>(value) > *field.maxVal)
				{
					detail::throwValidationErrorNum(field.name, "maximum", *field.maxVal);
				}
			}
			if constexpr (std::is_same_v<FieldType, std::string>)
			{
				validateJsonNotEmpty(field.name, field.notEmpty, value);
				// 先检查字节长度，超出范围就跳过正则匹配。
				if (field.lengthMin && value.size() < *field.lengthMin)
				{
					detail::throwValidationErrorNum(field.name, "min-length", static_cast<double>(*field.lengthMin));
				}
				if (field.lengthMax && value.size() > *field.lengthMax)
				{
					detail::throwValidationErrorNum(field.name, "max-length", static_cast<double>(*field.lengthMax));
				}
				if (field.pattern
					&& !matchFieldPattern(value,
										  *field.pattern,
										  field.compiledPattern ? &*field.compiledPattern : nullptr))
				{
					detail::throwValidationErrorStr(field.name, "pattern");
				}
				for (size_t index = 0; index < field.additionalPatterns.size(); ++index)
				{
					const std::regex* compiled = index < field.compiledAdditionalPatterns.size()
													 ? &field.compiledAdditionalPatterns[index]
													 : nullptr;
					if (!matchFieldPattern(value, field.additionalPatterns[index], compiled))
					{
						detail::throwValidationErrorStr(field.name, "pattern");
					}
				}
			}
		}

		/**
		 * @brief 按最终 JSON 键读取可见字段，并检查必填项
		 */
		template <typename T, typename Tuple, size_t... I>
		void deserializeFields(T& obj,
							   const boost::json::object& jsonObj,
							   const Tuple& fields,
							   std::index_sequence<I...>)
		{
			auto trySet = [&](const auto& field)
			{
				if (!field.active || field.ignored)
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
				else
				{
					validateJsonFieldPresence(field.name, field.required, false);
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
		jsonObj.reserve(count);
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

	// detail 之外的 jsonSchema 也要用 M，别名放在 hical::meta 这一层
	namespace M = std::meta;

	namespace detail
	{

		namespace M = std::meta;
		namespace json = boost::json;

		/**
		 * @brief 原生读写时检查最终 JSON 键是否重复，并触发字段声明的编译期检查
		 * @tparam ClassType 目标对象类型
		 * @tparam MembersOfFunc 成员收集函数
		 * @tparam Ctx 反射访问上下文
		 */
		template <typename ClassType, CollectMember::MemberInfoGatherer auto MembersOfFunc, M::access_context Ctx>
		void validateNativeJsonFields()
		{
			constexpr auto duplicateKey = []() consteval -> std::optional<std::string_view>
			{
				std::vector<std::string_view> names;
				constexpr static auto kMembers =
					std::define_static_array(MembersOfFunc(M::remove_cvref(^^ClassType), Ctx));
				template for (constexpr auto memberInfo : kMembers)
				{
					constexpr auto rules = anno::applyJsonFieldAnnotations<memberInfo>();
					if constexpr (!rules.ignored)
					{
						if (std::find(names.begin(), names.end(), rules.name) != names.end())
						{
							return rules.name;
						}
						names.push_back(rules.name);
					}
				}
				return {};
			}();
			if constexpr (duplicateKey.has_value())
			{
				throwFieldDefinitionError(*duplicateKey, "不同的可见成员使用了同一个 JSON 键");
			}
		}

		/**
		 * @brief 判断当前对象是否有可能改写子对象的 View
		 * @tparam ClassType 目标类型
		 * @tparam MembersOfFunc 成员收集函数
		 * @tparam Ctx 反射访问上下文
		 * @param input JSON 输入。传空指针时，只在编译期检查 View 声明
		 * @return 存在符合条件的可见字段时返回 true
		 */
		template <typename ClassType, CollectMember::MemberInfoGatherer auto MembersOfFunc, M::access_context Ctx>
		constexpr bool nativeHasViewAnnotations(const json::object* input = nullptr)
		{
			constexpr static auto kMembers = std::define_static_array(MembersOfFunc(M::remove_cvref(^^ClassType), Ctx));
			template for (constexpr auto member : kMembers)
			{
				constexpr auto rules = anno::applyJsonFieldAnnotations<member>();
				if constexpr (!rules.ignored && anno::hasViewAnnotations<member>())
				{
					if (!input || input->contains(rules.name))
					{
						return true;
					}
				}
			}
			return false;
		}

		/**
		 * @brief 无 View 时，只校验输入中实际出现的本层字段
		 * @tparam ClassType 对象类型
		 * @tparam MembersOfFunc 成员收集函数
		 * @tparam Ctx 反射访问上下文
		 * @param value 已解码的对象
		 * @param input 原始输入对象
		 */
		template <typename ClassType, CollectMember::MemberInfoGatherer auto MembersOfFunc, M::access_context Ctx>
		void validateNativeJsonOwnValues(const ClassType& value, const json::object& input)
		{
			constexpr static auto kMembers = std::define_static_array(MembersOfFunc(M::remove_cvref(^^ClassType), Ctx));
			template for (constexpr auto member : kMembers)
			{
				constexpr auto rules = anno::applyJsonFieldAnnotations<member>();
				if constexpr (!rules.ignored && (rules.notEmpty || rules.minimum || rules.maximum))
				{
					if (input.contains(rules.name))
					{
						validateJsonFieldValue(rules, value.[:member:]);
					}
				}
			}
		}

		// 只有 View 会执行时才收集规则。vector 按字段路径合并各元素的规则，保存并集。
		struct NativeJsonValidation
		{
			std::string_view name {};
			bool hasRules = false;
			bool customDecoded = false;
			std::vector<NativeJsonValidation> fields {};

			const NativeJsonValidation* findField(std::string_view key) const
			{
				const auto it = std::ranges::find(fields, key, &NativeJsonValidation::name);
				return it == fields.end() ? nullptr : &*it;
			}

			NativeJsonValidation& field(std::string_view key)
			{
				const auto it = std::ranges::find(fields, key, &NativeJsonValidation::name);
				if (it != fields.end())
				{
					return *it;
				}
				return fields.emplace_back(NativeJsonValidation {.name = key});
			}
		};

		template <typename T>
		consteval bool nativeCanDeferJsonRules()
		{
			// 整型和字符串由 fromJsonTemplate 开头的严格解码直接处理，没机会暂缓
			if constexpr (HasExplicitJsonValueTo<T> || std::is_integral_v<T> || std::is_same_v<T, std::string>)
			{
				return false;
			}
			else if constexpr (IsVector<T>::value)
			{
				return nativeCanDeferJsonRules<typename T::value_type>();
			}
			else
			{
				return !json::has_value_to<T>::value;
			}
		}

		/**
		 * @brief 等最外层有 View 的对象处理完，再检查暂缓的规则
		 * @param value 最终对象或列表
		 * @param validation 普通解码过程中记录的规则路径
		 */
		template <typename ClassType, CollectMember::MemberInfoGatherer auto MembersOfFunc, M::access_context Ctx>
		void validateNativeJsonDeferredValues(const ClassType& value, const NativeJsonValidation& validation)
		{
			if (!validation.hasRules)
			{
				return;
			}
			if (validation.customDecoded)
			{
				// View 可能重排元素，无法确认最终位置上的元素走了哪种解码。
				throwValidationErrorStr(validation.name, "ambiguous-view-list-decode");
			}
			if constexpr (IsVector<ClassType>::value)
			{
				// View 可能插入或重排元素。列表中出现过的规则要检查所有最终元素。
				for (const auto& element : value)
				{
					validateNativeJsonDeferredValues<typename ClassType::value_type, MembersOfFunc, Ctx>(element,
																										 validation);
				}
			}
			else
			{
				constexpr static auto kMembers =
					std::define_static_array(MembersOfFunc(M::remove_cvref(^^ClassType), Ctx));
				template for (constexpr auto member : kMembers)
				{
					constexpr auto rules = anno::applyJsonFieldAnnotations<member>();
					if constexpr (!rules.ignored)
					{
						const auto* field = validation.findField(rules.name);
						if (!field || !field->hasRules)
						{
							continue;
						}
						using MemberType = [:M::type_of(member):];
						if constexpr (rules.notEmpty || rules.minimum || rules.maximum)
						{
							validateJsonFieldValue(rules, value.[:member:]);
						}
						else if constexpr (nativeCanDeferJsonRules<MemberType>())
						{
							validateNativeJsonDeferredValues<MemberType, MembersOfFunc, Ctx>(value.[:member:], *field);
						}
					}
				}
			}
		}

		/**
		 * @brief 基于反射规则将对象实例转换为json::value的执行模板。class -> json
		 * @tparam ClassType 待序列化的目标对象类型
		 * @tparam MembersOfFunc 获取成员反射信息的函数
		 * @tparam Ctx 反射访问权限检查上下文，控制是否可以访问私有/保护成员
		 * @param classValue 待序列化的对象const引用
		 * @return json::value 序列化好的值
		 * @note 优先使用显式转换。普通 vector 逐项转换，其余类型交给 Boost 或逐字段处理
		 * @attention 仅在走通用成员遍历函数会自动处理注解相关
		 */
		template <typename ClassType,
				  CollectMember::MemberInfoGatherer auto MembersOfFunc = CollectMember::collectNonstaticMemberInfos,
				  M::access_context Ctx = M::access_context::unprivileged()>
		auto toJsonTemplate(const ClassType& classValue) -> json::value
		{
			using ValueType = std::remove_const_t<ClassType>;
			if constexpr (HasExplicitJsonValueFrom<ClassType>)
			{
				return json::value_from(classValue);
			}
			else if constexpr (IsVector<ValueType>::value)
			{
				json::array result;
				result.reserve(classValue.size());
				using ElementType = typename ValueType::value_type;
				for (const auto& element : classValue)
				{
					// 把 vector<bool> 的代理引用转成 bool。
					if constexpr (std::is_same_v<ElementType, bool>)
					{
						result.push_back(toJsonTemplate<ElementType, MembersOfFunc, Ctx>(static_cast<bool>(element)));
					}
					else
					{
						result.push_back(toJsonTemplate<ElementType, MembersOfFunc, Ctx>(element));
					}
				}
				return result;
			}
			else if constexpr (json::has_value_from<ClassType>::value)
			{
				return json::value_from(classValue);
			}

			// 通用函数部分
			else
			{
				validateNativeJsonFields<ClassType, MembersOfFunc, Ctx>();
				// 获取成员反射信息集合
				constexpr static auto kMemberInfos =
					std::define_static_array(MembersOfFunc(M::remove_cvref(^^ClassType), Ctx));

				// 结果，先预分配
				constexpr auto joCapacity = kMemberInfos.size() + 1;
				json::object jo {};
				jo.reserve(joCapacity);

				template for (constexpr auto memberInfo : kMemberInfos)
				{
					// 执行编译时注解
					constexpr auto rules = anno::applyJsonFieldAnnotations<memberInfo>();
					if constexpr (!rules.ignored)
					{
						constexpr auto memberName = rules.name;

						// 成员值
						const auto& memberValue = classValue.[:memberInfo:];

						// 序列化
						json::object::const_iterator memberJvIt;
						if (auto optValue = anno::applySerializeAnnotations<memberInfo>(memberValue))
						{
							memberJvIt = jo.emplace(memberName, *std::move(optValue)).first;
						}
						else
						{
							memberJvIt =
								jo.emplace(memberName,
										   toJsonTemplate<typename[:M::type_of(memberInfo):], MembersOfFunc, Ctx>(
											   memberValue))
									.first;
						}

						// 视图注解
						anno::MemberAnnotationView<true, memberInfo, ClassType> view {.classValue_ = classValue,
																					  .memberValue_ = memberValue,
																					  .memberJv_ = memberJvIt->value()};
						anno::applyViewAnnotations(view);
					}
				}
				return jo;
			}
		}

		/**
		 * @brief 基于反射规则将json::value转换为对象实例的执行模板。 json -> class
		 * @tparam ClassType 待序列化的目标对象类型
		 * @tparam MembersOfFunc 获取成员反射信息的函数
		 * @tparam Ctx 反射访问权限检查上下文，控制是否可以访问私有/保护成员
		 * @tparam DeferRules 是否把规则留到父级 View 执行完再检查
		 * @param jsonValue 待反序列化的 json::value const&
		 * @param parentValidation 父级 View 执行完后要检查的规则树
		 * @return 返回填充好的对象
		 * @note 优先使用显式转换。整型、bool 和字符串在前面按严格规则解码，vector 逐项读取，其余类型交给 Boost 或逐字段处理
		 * @attention 仅在走通用成员遍历函数会自动处理注解相关
		 */
		template <typename ClassType,
				  CollectMember::MemberInfoGatherer auto MembersOfFunc = CollectMember::collectNonstaticMemberInfos,
				  M::access_context Ctx = M::access_context::unprivileged(),
				  bool DeferRules = false>
		auto fromJsonTemplate(const json::value& jsonValue, NativeJsonValidation* parentValidation = nullptr)
			-> ClassType
		{
			if constexpr (HasExplicitJsonValueTo<ClassType>)
			{
				return json::value_to<ClassType>(jsonValue);
			}
			// 整型（bool 也算整型）和字符串统一走严格解码：宽度、符号、bool 只认 true/false，
			// valueFromJson 都已经处理好，Boost 的 value_to 反而会接受 1.0 这种写法
			else if constexpr (std::is_integral_v<ClassType> || std::is_same_v<ClassType, std::string>)
			{
				return valueFromJson<ClassType>(jsonValue);
			}
			else if constexpr (IsVector<ClassType>::value)
			{
				static_assert(std::default_initializable<ClassType>, "原生 JSON 解码需要能默认构造列表");
				if (!jsonValue.is_array())
				{
					throwTypeMismatch("array");
				}
				ClassType result;
				const auto& array = jsonValue.as_array();
				result.reserve(array.size());
				for (const auto& element : array)
				{
					result.push_back(fromJsonTemplate<typename ClassType::value_type, MembersOfFunc, Ctx, DeferRules>(
						element,
						parentValidation));
				}
				return result;
			}
			else if constexpr (json::has_value_to<ClassType>::value)
			{
				return json::value_to<ClassType>(jsonValue);
			}

			// 通用函数部分
			else
			{
				static_assert(std::default_initializable<ClassType>, "原生 JSON 解码需要能默认构造对象");
				validateNativeJsonFields<ClassType, MembersOfFunc, Ctx>();
				// 获取成员反射信息集合
				constexpr static auto kMemberInfos =
					std::define_static_array(MembersOfFunc(M::remove_cvref(^^ClassType), Ctx));

				// 默认构造
				ClassType classValue {};

				// 通用成员遍历只处理对象，非对象的反序列化前面已由 value_to 重载接走
				const json::object& jsonObj = jsonValue.as_object();
				constexpr bool kHasView = nativeHasViewAnnotations<ClassType, MembersOfFunc, Ctx>();
				constexpr bool kCanTrack = DeferRules || kHasView;
				[[maybe_unused]] std::conditional_t<kHasView && !DeferRules,
													std::optional<NativeJsonValidation>,
													std::nullptr_t> localValidation {};
				NativeJsonValidation* validation = nullptr;
				if constexpr (DeferRules)
				{
					validation = parentValidation;
				}
				else if constexpr (kHasView)
				{
					if (nativeHasViewAnnotations<ClassType, MembersOfFunc, Ctx>(&jsonObj))
					{
						validation = &localValidation.emplace();
					}
				}

				template for (constexpr auto memberInfo : kMemberInfos)
				{
					// 执行编译时注解
					constexpr auto rules = anno::applyJsonFieldAnnotations<memberInfo>();
					if constexpr (!rules.ignored)
					{
						constexpr auto memberName = rules.name;

						// 缺字段时：标了 required 的报错，其余静默跳过（与 C++20 回退路线语义一致）
						auto memberJvIt = jsonObj.find(memberName);
						if (memberJvIt != jsonObj.end())
						{
							const json::value& memberJv = memberJvIt->value();
							auto& memberValue = classValue.[:memberInfo:];
							using MemberType = [:M::type_of(memberInfo):];
							constexpr bool kHasRule = rules.notEmpty || rules.minimum || rules.maximum;
							constexpr bool kTrackChild =
								kCanTrack && !kHasRule && nativeCanDeferJsonRules<MemberType>();
							NativeJsonValidation* childValidation = nullptr;
							if constexpr (kCanTrack)
							{
								if (validation)
								{
									if constexpr (kHasRule)
									{
										validation->field(memberName).hasRules = true;
										validation->hasRules = true;
									}
									else if constexpr (kTrackChild)
									{
										childValidation = &validation->field(memberName);
									}
								}
							}

							// 反序列化
							if (auto optValue = anno::applyDeserializeAnnotations<memberInfo>(memberJv))
							{
								memberValue = *std::move(optValue);
								if constexpr (kTrackChild)
								{
									if (childValidation)
									{
										childValidation->customDecoded = true;
									}
								}
							}
							else
							{
								const auto decode = [&]() -> MemberType
								{
									if constexpr (kTrackChild)
									{
										if (childValidation)
										{
											return fromJsonTemplate<MemberType, MembersOfFunc, Ctx, true>(
												memberJv,
												childValidation);
										}
									}
									return fromJsonTemplate<MemberType, MembersOfFunc, Ctx>(memberJv);
								};
								memberValue = decode();
								if constexpr (kTrackChild)
								{
									if (childValidation)
									{
										validation->hasRules |= childValidation->hasRules;
									}
								}
							}

							// 视图注解
							anno::MemberAnnotationView<false, memberInfo, ClassType> view {.classValue_ = classValue,
																						   .memberValue_ = memberValue,
																						   .memberJv_ = memberJv};
							anno::applyViewAnnotations(view);
						}
						else if constexpr (rules.required)
						{
							validateJsonFieldPresence(rules, false);
						}
					}
				}

				if constexpr (kHasView && !DeferRules)
				{
					if (localValidation)
					{
						validateNativeJsonDeferredValues<ClassType, MembersOfFunc, Ctx>(classValue, *localValidation);
					}
				}
				if constexpr (!DeferRules)
				{
					if (!validation)
					{
						validateNativeJsonOwnValues<ClassType, MembersOfFunc, Ctx>(classValue, jsonObj);
					}
				}
				return classValue;
			}
		}

	} // namespace detail

	/**
	 * @brief 序列化结构体为 JSON（C++26 反射）
	 * 支持 [[=hical::anno::json_ignore]]、[[=hical::anno::json_rename("xxx")]]
	 */
	template <typename T>
	boost::json::object toJson(const T& obj)
	{
		return detail::toJsonTemplate(obj).as_object();
	}

	/**
	 * @brief 从 JSON 反序列化为结构体（C++26 反射）
	 * 支持 [[=hical::anno::json_ignore]]、[[=hical::anno::json_rename("xxx")]]、[[=hical::anno::json_required]]
	 * View 执行完后再校验普通解码的值。View 可能移动列表元素，因此用输入中出现过的规则检查列表中的所有最终元素。
	 */
	template <typename T>
	T fromJson(const boost::json::value& json)
	{
		return detail::fromJsonTemplate<T>(json);
	}

	/**
	 * @brief 编译期生成 JSON Schema（C++26 反射）
	 * 根据结构体成员类型和属性注解自动生成符合 JSON Schema 规范的描述。
	 */
	template <typename ClassType,
			  CollectMember::MemberInfoGatherer auto MembersOfFunc = CollectMember::collectNonstaticMemberInfos,
			  M::access_context Ctx = M::access_context::unprivileged()>
	boost::json::object jsonSchema()
	{
		namespace json = boost::json;
		constexpr M::info classTypeInfo = M::remove_cvref(^^ClassType);

		json::object schema;
		schema["type"] = "object";
		schema["format"] = M::display_string_of(classTypeInfo);
		json::object properties;
		json::array requiredFields;

		template for (constexpr auto memberInfo : std::define_static_array(MembersOfFunc(classTypeInfo, Ctx)))
		{
			constexpr auto [ignoreMember, memberName, requiredMember] = anno::applyKeyAnnotations<memberInfo>();
			if constexpr (ignoreMember)
			{
				continue;
			}
			using MemberType = [:M::remove_cvref(M::type_of(memberInfo)):];

			// 获取成员注解信息集合
			constexpr static auto kAnnotationInfos = std::define_static_array(
				[memberInfo]
				{
					std::vector<M::info> result;
					template for (constexpr auto annoInfo :
								  std::define_static_array(anno::annotationsOfMemberWithParent(memberInfo)))
					{
						constexpr auto annoValue = [:M::constant_of(annoInfo):];
						constexpr auto typeValue = std::type_identity<MemberType> {};
						if constexpr (requires {
										  {
											  annoValue.applyAnnotationSchema(std::declval<json::object&>(), typeValue)
										  } -> std::same_as<void>;
									  })
						{
							result.push_back(annoInfo);
						}
					}
					return result;
				}());

			json::object prop;
			// 如果有注解，就直接调用注解的函数
			if constexpr (not kAnnotationInfos.empty())
			{
				template for (constexpr auto annoInfo : kAnnotationInfos)
				{
					[:M::constant_of(annoInfo):].applyAnnotationSchema(prop, std::type_identity<MemberType> {});
				}
			}
			// 否则，如果MetaSchema有对应的偏特化，就调用偏特化函数
			else if constexpr (requires {
								   { schema::kSchema<MemberType>(prop) } -> std::same_as<void>;
							   })
			{
				schema::kSchema<MemberType>(prop);
			}
			// 否则，如果成员本身是类对象，继续递归调用 jsonSchema
			else if constexpr (M::is_class_type(M::remove_cvref(M::type_of(memberInfo))))
			{
				prop = jsonSchema<MemberType>();
			}
			// 否则，直接硬错误
			else
			{
				static_assert(false,
							  std::string {"No JSON Schema specialization for type: "}
								  + M::display_string_of(M::remove_cvref(M::type_of(memberInfo))));
			}

			// 显式标了 json_required 的必进；否则只有 nullable（optional）才豁免
			const json::value* nullableIt = prop.if_contains("nullable");
			const bool nullableMember = nullableIt != nullptr && nullableIt->is_bool() && nullableIt->get_bool();
			if (requiredMember || not nullableMember)
			{
				requiredFields.emplace_back(memberName);
			}

			properties[memberName] = std::move(prop);
		}

		schema["properties"] = properties;
		if (!requiredFields.empty())
		{
			schema["required"] = requiredFields;
		}
		return schema;
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
	#define HICAL_JSON_MAKE_FIELD_(T, field, isAlias, ...)                                                 \
		(                                                                                                  \
			[]()                                                                                           \
			{                                                                                              \
				static_assert(                                                                             \
					requires { std::declval<T>().field; },                                                 \
					"HICAL_JSON: field '" #field "' does not exist or is not publicly accessible in " #T); \
				auto descriptor = ::hical::meta::detail::makeField<T>(__VA_ARGS__);                        \
				descriptor.memberName = #field;                                                            \
				descriptor.explicitAlias = isAlias;                                                        \
				return descriptor;                                                                         \
			}())

	// 约束装饰器的公共骨架：先按字段实际类型做编译期检查，再构造描述器。
	// 检查表达式只用来推导类型，不会真的求值，所以放 decltype 里。
	#define HICAL_JSON_CONSTRAINED_FIELD_(T, field, isAlias, HicalCheck_, ...)                             \
		(                                                                                                  \
			[]()                                                                                           \
			{                                                                                              \
				static_assert(                                                                             \
					requires { std::declval<T>().field; },                                                 \
					"HICAL_JSON: field '" #field "' does not exist or is not publicly accessible in " #T); \
				HicalCheck_;                                                                               \
				auto descriptor = ::hical::meta::detail::makeField<T>(__VA_ARGS__);                        \
				descriptor.memberName = #field;                                                            \
				descriptor.explicitAlias = isAlias;                                                        \
				return descriptor;                                                                         \
			}())

	// 裸字段：name → makeField("name", &T::name)
	#define HICAL_JSON_LEAF_0(T, field) HICAL_JSON_MAKE_FIELD_(T, field, false, #field, &T::field)

	// 括号装饰 → 解包括号 → 提取 tag → 分派到对应 TAG 处理器
	// HICAL_JSON_DEPAREN_ 剥离外层括号：(a, b, c) → a, b, c
	#define HICAL_JSON_DEPAREN_(...) __VA_ARGS__
	#define HICAL_JSON_LEAF_1(T, tagged) HICAL_JSON_UNWRAP_(T, HICAL_JSON_DEPAREN_ tagged)
	#define HICAL_JSON_UNWRAP_(T, ...) HICAL_JSON_TAG_DISPATCH_(T, __VA_ARGS__)
	#define HICAL_JSON_TAG_DISPATCH_(T, tag, ...) HICAL_JSON_PASTE_(HICAL_JSON_TAG_, tag)(T, __VA_ARGS__)

// ---- Tag 处理器 ----

	#define HICAL_JSON_TAG_hical_alias_(T, field, alias) HICAL_JSON_MAKE_FIELD_(T, field, true, alias, &T::field)
	#define HICAL_JSON_TAG_hical_required_(T, field) \
		HICAL_JSON_MAKE_FIELD_(T, field, false, #field, &T::field, true, false)
	#define HICAL_JSON_TAG_hical_required_alias_(T, field, alias) \
		HICAL_JSON_MAKE_FIELD_(T, field, true, alias, &T::field, true, false)
	#define HICAL_JSON_TAG_hical_ignore_(T, field) \
		HICAL_JSON_MAKE_FIELD_(T, field, false, #field, &T::field, false, true)

	// 校验约束 Tag 处理器
	// 宏体里拿不到字段的成员类型，用 decltype 推一遍再检查。
	// 类型不对是编译期错误，运行时行为保持原样（这些约束对非匹配类型本来就不生效）。
	// decltype 包在 requires 里，字段压根不存在时让外层那条 static_assert 先报，别刷一堆模板错误。
	#define HICAL_JSON_CHECK_ARITHMETIC_(T, field)                                               \
		static_assert(                                                                           \
			!requires { std::declval<T>().field; }                                               \
				|| std::is_arithmetic_v<std::remove_cvref_t<decltype(std::declval<T>().field)>>, \
			"HICAL_JSON: MIN/MAX 只能用于算术类型字段 '" #field "' in " #T)

	#define HICAL_JSON_CHECK_STRING_(T, field, HicalRule_)                                              \
		static_assert(                                                                                  \
			!requires { std::declval<T>().field; }                                                      \
				|| std::is_same_v<std::remove_cvref_t<decltype(std::declval<T>().field)>, std::string>, \
			"HICAL_JSON: " HicalRule_ " 只能用于 std::string 字段 '" #field "' in " #T)

	#define HICAL_JSON_TAG_hical_min_(T, field, val)                          \
		HICAL_JSON_CONSTRAINED_FIELD_(T,                                      \
									  field,                                  \
									  false,                                  \
									  HICAL_JSON_CHECK_ARITHMETIC_(T, field), \
									  #field,                                 \
									  &T::field,                              \
									  false,                                  \
									  false,                                  \
									  val,                                    \
									  {},                                     \
									  {},                                     \
									  false,                                  \
									  {},                                     \
									  {})
	#define HICAL_JSON_TAG_hical_max_(T, field, val)                          \
		HICAL_JSON_CONSTRAINED_FIELD_(T,                                      \
									  field,                                  \
									  false,                                  \
									  HICAL_JSON_CHECK_ARITHMETIC_(T, field), \
									  #field,                                 \
									  &T::field,                              \
									  false,                                  \
									  false,                                  \
									  {},                                     \
									  val,                                    \
									  {},                                     \
									  false,                                  \
									  {},                                     \
									  {})
	#define HICAL_JSON_TAG_hical_not_empty_(T, field)                                  \
		HICAL_JSON_CONSTRAINED_FIELD_(T,                                               \
									  field,                                           \
									  false,                                           \
									  HICAL_JSON_CHECK_STRING_(T, field, "NOT_EMPTY"), \
									  #field,                                          \
									  &T::field,                                       \
									  false,                                           \
									  false,                                           \
									  {},                                              \
									  {},                                              \
									  {},                                              \
									  true,                                            \
									  {},                                              \
									  {})
	#define HICAL_JSON_TAG_hical_pattern_(T, field, re)                              \
		HICAL_JSON_CONSTRAINED_FIELD_(T,                                             \
									  field,                                         \
									  false,                                         \
									  HICAL_JSON_CHECK_STRING_(T, field, "PATTERN"), \
									  #field,                                        \
									  &T::field,                                     \
									  false,                                         \
									  false,                                         \
									  {},                                            \
									  {},                                            \
									  re,                                            \
									  false,                                         \
									  {},                                            \
									  {})
	#define HICAL_JSON_TAG_hical_length_(T, field, minLen, maxLen)                  \
		HICAL_JSON_CONSTRAINED_FIELD_(T,                                            \
									  field,                                        \
									  false,                                        \
									  HICAL_JSON_CHECK_STRING_(T, field, "LENGTH"), \
									  #field,                                       \
									  &T::field,                                    \
									  false,                                        \
									  false,                                        \
									  {},                                           \
									  {},                                           \
									  {},                                           \
									  false,                                        \
									  minLen,                                       \
									  maxLen)

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

	#define HICAL_JSON(Type, ...)                                                                        \
		static const auto& hicalJsonFields()                                                             \
		{                                                                                                \
			static const auto fields = ::hical::meta::detail::initializeJsonFields(                      \
				[]()                                                                                     \
				{                                                                                        \
					return std::make_tuple(                                                              \
						HICAL_JSON_EXPAND_(HICAL_JSON_FOR_EACH_(HICAL_JSON_FIELD_, Type, __VA_ARGS__))); \
				});                                                                                      \
			return fields;                                                                               \
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
