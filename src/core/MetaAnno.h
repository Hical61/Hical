/**
 * @file MetaAnno.h
 * @brief C++26 反射注解框架：注解处理器基类与内置 JSON 注解
 */

#pragma once

#include "Utils/CTConv.h"
#include "detail/JsonFieldRules.h"

#include <boost/json.hpp>
#include <meta>
#include <limits>

/**
 * @brief 编译期注解（Annotation）命名空间
 * 包含注解处理器框架及内置注解类型。
 * 注解处理分为三个阶段：
 * 1. Key 阶段（编译期）：处理成员字段到 JSON key 的映射
 * 2. Serialize/Deserialize 阶段（运行时）：处理值的自定义转换
 * - 所有匹配的 handler 按声明顺序依次执行，共享同一个 std::optional result
 * - handler 可以读取前一个 handler 产出的 result 进行二次修改
 * - 最终返回的 result 由最后一个设置它的 handler 决定（"最后胜出"）
 * - 通过 requires 检测函数重载是否存在，而非通过基类约束。
 * 这使得注解可以放在类上（如 [[=Anno::as_iso_8601]]），但对每个成员按 MemberType
 * 进行重载决议：只有类型匹配的成员（如 time_point）才会命中重载并执行转换，
 * 类型不匹配的成员（如 int、string）则因 requires 失败而静默跳过。
 * 注解编写者只需为关心的类型提供重载，无需关心其他类型。
 * 3. View 阶段：序列化/反序列化完成后的回调通知
 */
namespace hical::anno
{
	namespace json = boost::json;
	namespace M = std::meta;

	/**
	 * @brief 获取成员字段及其所属类的全部注解
	 * @param memberInfo 成员字段的反射元信息
	 * @return 注解元信息数组，类注解在前，成员字段注解在后
	 */
	consteval std::vector<M::info> annotationsOfMemberWithParent(const M::info memberInfo)
	{
		std::vector<M::info> result;
		result.append_range(M::annotations_of(M::parent_of(memberInfo)));
		result.append_range(M::annotations_of(memberInfo));
		return result;
	}

	/**
	 * @brief Key 阶段注解处理结果
	 * @details 由 applyKeyAnnotations() 返回，决定成员字段在 JSON 中的表示方式
	 */
	struct MemberAnnotationResult
	{
		bool ignoreMember_ {false};   ///< 为 true 时跳过该成员，不参与序列化/反序列化
		std::string_view keyName_;    ///< 映射后的 JSON key 名称
		bool requiredMember_ {false}; ///< 为 true 时反序列化缺该字段要报错
	};

	/**
	 * @brief Key 注解处理器基类（CRTP）
	 * @details 自定义 key 注解需继承此类并实现 static consteval impl(MemberAnnotationResult&) 方法。
	 * 通过 CRTP 的 applyAnnotation() 转发到子类的 impl()。
	 */
	struct KeyAnnotationHandler
	{
		/**
		 * @brief 将注解应用到结果上（CRTP 转发）
		 * @param self 派生类自身引用
		 * @param result 注解处理结果，由 impl() 修改
		 * @param memberInfo 成员字段的反射元信息
		 */
		consteval void applyAnnotation(this auto& self, MemberAnnotationResult& result, const M::info memberInfo)
		{
			if constexpr (requires {
							  { self.impl(result) } -> std::same_as<void>;
						  })
			{
				self.impl(result);
			}
			else
			{
				self.impl(result, memberInfo);
			}
		}
	};

	/**
	 * @brief 应用 key 相关注解（编译期）
	 * @tparam MemberInfo 成员字段的反射元信息
	 * @return MemberAnnotationResult 包含 skip 标志和映射后的 key 名称
	 * @details 遍历成员及其所属类的全部注解，仅处理继承自 KeyAnnotationHandler 的注解类型。
	 * 注解按声明顺序执行，后续注解可覆盖前序注解的结果。
	 */
	template <M::info MemberInfo>
	consteval auto applyKeyAnnotations() -> MemberAnnotationResult
	{
		MemberAnnotationResult result;
		result.keyName_ = M::identifier_of(MemberInfo);

		constexpr static auto kAnnotationInfos = std::define_static_array(annotationsOfMemberWithParent(MemberInfo));
		// clang-format off
		// 注意：clang-format 会把 splice 的 ':' 和 ']' 拆到两行，而 [: :] 必须成对相邻，
		// 拆开就是语法错误（GCC16 实测报 parse error in template argument list）。这里禁用它。
		template for (constexpr auto annoInfo : kAnnotationInfos)
			if constexpr (std::derived_from<typename [:M::type_of(annoInfo):], KeyAnnotationHandler>)
				[:M::constant_of(annoInfo):].applyAnnotation(result, MemberInfo);
		// clang-format on

		return result;
	}

	/**
	 * @brief 应用序列化注解，对成员值进行自定义转换
	 * @tparam MemberInfo 成员字段的反射元信息
	 * @tparam MemberType 成员字段的实际类型
	 * @param memberValue 成员字段的当前值
	 * @return 最后胜出的 handler 产出的 json::value；若无任何 handler 设置则返回 std::nullopt
	 * @attention 仅当 applyAnnotationSerialize(member_value, result) 方法重载存在（通过 requires 检测）时才会被调用。
	 * @see hical::anno
	 */
	template <M::info MemberInfo, typename MemberType>
	std::optional<json::value> applySerializeAnnotations(const MemberType& memberValue)
	{
		std::optional<json::value> result;

		constexpr static auto kAnnotationInfos = std::define_static_array(annotationsOfMemberWithParent(MemberInfo));
		template for (constexpr auto annoInfo : kAnnotationInfos)
		{
			constexpr auto annoValue = [:M::constant_of(annoInfo):];
			if constexpr (requires {
							  { annoValue.applyAnnotationSerialize(memberValue, result) } -> std::same_as<void>;
						  })
			{
				annoValue.applyAnnotationSerialize(memberValue, result);
			}
		}

		return result;
	}

	/**
	 * @brief 应用反序列化注解，对 JSON 值进行自定义转换
	 * @tparam MemberInfo 成员字段的反射元信息
	 * @tparam MemberType 成员字段的实际类型，默认为成员字段的声明类型，需满足 std::default_initializable
	 * @param memberJv JSON 中该成员对应的值
	 * @return 最后胜出的 handler 产出的 MemberType；若无任何 handler 设置则返回 std::nullopt
	 * @attention 仅当 applyAnnotationDeserialize(member_jv, result) 方法重载存在（通过 requires 检测）时才会被调用。
	 * @see hical::anno
	 */
	template <M::info MemberInfo, std::default_initializable MemberType = [:M::type_of(MemberInfo):]>
	std::optional<MemberType> applyDeserializeAnnotations( const json::value & memberJv) {
		std::optional<MemberType> result;

		constexpr static auto kAnnotationInfos = std::define_static_array(annotationsOfMemberWithParent(MemberInfo));
		template for (constexpr auto annoInfo : kAnnotationInfos) {
			constexpr auto annoValue = [:M::constant_of(annoInfo):];
			if constexpr (requires { { annoValue.applyAnnotationDeserialize(memberJv, result) } -> std::same_as<void>; }){
				annoValue.applyAnnotationDeserialize(memberJv, result);
}
		}

		return result;
	}

	/**
	 * @brief 注解视图，序列化/反序列化完成后传递给视图注解的上下文
	 * @tparam IsSerializeDir true 表示序列化方向，false 表示反序列化方向
	 * @tparam MemberInfo 成员字段的反射元信息
	 * @tparam ClsType 所属类的类型
	 * @tparam MemType 成员字段的类型，默认为成员字段的声明类型
	 * @details 包含当前处理的完整上下文：类引用、成员值引用、JSON 值引用，
	 * 供视图注解（ViewAnnotationHandler）在序列化/反序列化完成后读取或记录。
	 */
	template <
		bool IsSerializeDir,	///< 序列化方向标志
		M::info MemberInfo,		///< 成员反射元信息
		typename ClsType,		///< 所属类类型
		typename MemType = [:M::type_of(MemberInfo):]	///< 成员字段类型
	>
	struct MemberAnnotationView {
		constexpr static bool kIsSerializeDirection = IsSerializeDir;
		constexpr static M::info kMemberInfo = MemberInfo;
		using ClassType = ClsType;
		using MemberType = MemType;

		 const ClassType & classValue_;	///< 所属类实例的常量引用
		 const MemberType & memberValue_;	///< 成员字段值的常量引用
		 const json::value & memberJv_;	///< 序列化后或反序列化前的 JSON 值引用
	};

	/**
	 * @brief 约束类型为 MemberAnnotationView 的某个模板实例化
	 * @tparam ViewType 待检查的类型
	 */
	template <typename ViewType>
	concept IsViewType = requires {
		requires M::has_template_arguments(^^ViewType);
		requires M::template_of(^^ViewType) == ^^MemberAnnotationView;
	};

	/**
	 * @brief 视图注解处理器基类（CRTP）
	 * @details 自定义视图注解需继承此类并实现 static impl(IsViewType auto const& view) 方法。
	 * 通过 CRTP 的 apply_annotation() 转发到子类的 impl()。
	 */
	struct ViewAnnotationHandler
	{
		/**
		 * @brief 将注解应用到视图上（CRTP 转发）
		 * @param self 派生类自身引用
		 * @param view 注解视图上下文
		 */
		void applyAnnotation(this auto& self, const IsViewType auto& view)
		{
			self.impl(view);
		}
	};

	/**
	 * @brief 应用视图注解
	 * @param view 注解视图上下文（需满足 IsViewType concept）
	 * @details 遍历成员及其所属类的全部注解，仅处理继承自 ViewAnnotationHandler 的注解类型。
	 * 注解按声明顺序依次执行。
	 */
	void applyViewAnnotations(const IsViewType auto& view)
	{
		constexpr static auto kAnnotationInfos =
			std::define_static_array(annotationsOfMemberWithParent(view.kMemberInfo));
		// clang-format off
		template for (constexpr auto annoInfo : kAnnotationInfos)
			if constexpr (std::derived_from<typename [:M::type_of(annoInfo):], ViewAnnotationHandler>)
				[:M::constant_of(annoInfo):].applyAnnotation(view);
		// clang-format on
	}

	/**
	 * @brief 检查成员及其所属类是否声明了 View 处理器
	 * @tparam MemberInfo 成员反射信息
	 * @return 找到 View 处理器时返回 true
	 */
	template <M::info MemberInfo>
	consteval bool hasViewAnnotations()
	{
		constexpr static auto kAnnotations = std::define_static_array(annotationsOfMemberWithParent(MemberInfo));
		// clang-format off
		template for (constexpr auto info : kAnnotations)
		{
			if constexpr (std::derived_from<typename [:M::type_of(info):], ViewAnnotationHandler>)
			{
				return true;
			}
		}
		// clang-format on
		return false;
	}

	/**
	 * @brief 检查是否声明了适用于该成员类型的 Deserialize 处理函数
	 * @tparam MemberInfo 成员反射信息
	 * @return 有处理函数可接收 JSON 值和该成员的解码结果时返回 true
	 */
	template <M::info MemberInfo>
	consteval bool hasDeserializeAnnotations()
	{
		constexpr static auto kAnnotations = std::define_static_array(annotationsOfMemberWithParent(MemberInfo));
		// clang-format off
		using MemberType = [:M::type_of(MemberInfo):];
		template for (constexpr auto info : kAnnotations)
		{
			constexpr auto annotation = [:M::constant_of(info):];
			if constexpr (requires(const json::value& input, std::optional<MemberType>& result)
			{
				{ annotation.applyAnnotationDeserialize(input, result) } -> std::same_as<void>;
			})
			{
				return true;
			}
		}
		// clang-format on
		return false;
	}

	// ================================================================================================
	//  内置注解类型
	// ================================================================================================

	/**
	 * @brief 跳过注解：标记该成员不参与序列化/反序列化
	 * @code
	 * [[=hical::anno::json_ignore]] int internal_counter;
	 * @endcode
	 */
	struct IgnoreKeyHandler : anno::KeyAnnotationHandler
	{
		static consteval void impl(anno::MemberAnnotationResult& result)
		{
			result.ignoreMember_ = true;
		}
	} inline constexpr json_ignore {};

	/**
	 * @brief 必填注解：标记该成员反序列化时缺字段要报错
	 * @code
	 *     [[=hical::anno::json_required]] int id;
	 * @endcode
	 */
	struct RequiredKeyHandler : anno::KeyAnnotationHandler
	{
		static consteval void impl(MemberAnnotationResult& result)
		{
			result.requiredMember_ = true;
		}
	} inline constexpr json_required {};

	/**
	 * @brief 重命名注解：将成员字段映射为指定的 JSON key 名称
	 * @code
	 * [[=hical::anno::json_rename("custom_name")]] int field;
	 * @endcode
	 */
	struct RenameKeyHandler : anno::KeyAnnotationHandler
	{
		const char* name_; ///< 目标 JSON key 名称（编译期静态字符串）

		consteval void impl(anno::MemberAnnotationResult& result) const
		{
			result.keyName_ = name_;
		}
	};

	/**
	 * @brief 创建重命名注解实例
	 * @param rename 目标 JSON key 名称
	 * @return RenameKeyHandler 注解实例，内部持有编译期静态字符串
	 */
	consteval RenameKeyHandler json_rename(const std::string_view rename)
	{
		return RenameKeyHandler {.name_ = std::define_static_string(rename)};
	}

	/**
	 * @brief 重置注解：将成员字段的 JSON key 恢复为原始标识符，并取消跳过
	 * @details 当类注解（如 snake_to_lowerCamel）改变了全局 key 映射策略后，
	 * 可用此注解对特定成员恢复默认行为，实现"全局变换 + 单点豁免"。
	 * @code
	 * [[=hical::anno::json_snake_to_lowerCamel]]		// 类注解：全局转小驼峰
	 * struct Test {
	 * [[=hical::anno::json_reset]] int special_field;	// 此成员保持原始名 "special_field"
	 * int user_name;								// 此成员转为 "userName"
	 * };
	 * @endcode
	 */
	struct ResetKeyHandler : anno::KeyAnnotationHandler
	{
		static consteval void impl(anno::MemberAnnotationResult& result, const M::info memberInfo)
		{
			result.ignoreMember_ = false;
			result.keyName_ = M::identifier_of(memberInfo);
		}
	} inline constexpr json_reset {};

	/**
	 * @brief 蛇形转小驼峰注解：将 snake_case 成员名转为 lowerCamelCase
	 * @code
	 * [[=hical::anno::json_snake_to_lowerCamel]] int user_name;  // JSON key → "userName"
	 * @endcode
	 */
	struct CaseSnakeToLowerCamelKeyHandler : anno::KeyAnnotationHandler
	{
		static consteval void impl(anno::MemberAnnotationResult& result)
		{
			result.keyName_ = std::define_static_string(CT::caseSnakeToLowerCamel(result.keyName_));
		}
	} inline constexpr json_snake_to_lowerCamel {};

	/**
	 * @brief 蛇形转大驼峰注解：将 snake_case 成员名转为 UpperCamelCase
	 * @code
	 * [[=hical::anno::json_snake_to_UpperCamel]] int user_name;  // JSON key → "UserName"
	 * @endcode
	 */
	struct CaseSnakeToUpperCamelKeyHandler : anno::KeyAnnotationHandler
	{
		static consteval void impl(anno::MemberAnnotationResult& result)
		{
			result.keyName_ = std::define_static_string(CT::caseSnakeToUpperCamel(result.keyName_));
		}
	} inline constexpr json_snake_to_UpperCamel {};

	/**
	 * @brief 驼峰转蛇形注解：将 camelCase 或 PascalCase 成员名转为 snake_case
	 * @code
	 * [[=hical::anno::json_camel_to_snake]] int userName;  // JSON key → "user_name"
	 * @endcode
	 */
	struct CaseCamelToSnakeKeyHandler : anno::KeyAnnotationHandler
	{
		static consteval void impl(anno::MemberAnnotationResult& result)
		{
			result.keyName_ = std::define_static_string(CT::caseCamelToSnake(result.keyName_));
		}
	} inline constexpr json_camel_to_snake {};

	/**
	 * @brief int32 字段的最小值注解
	 */
	struct JsonMinRule
	{
		std::int32_t value_;
	};

	/**
	 * @brief int32 字段的最大值注解
	 */
	struct JsonMaxRule
	{
		std::int32_t value_;
	};

	/**
	 * @brief 字符串非空注解
	 */
	struct JsonNotEmptyRule
	{
	} inline constexpr json_not_empty {};

	/**
	 * @brief 检查数值注解的边界能否用 int32 表示
	 * @param value 注解中的整数常量
	 * @return 转换后的 int32 边界
	 */
	template <typename Value>
	consteval std::int32_t jsonInt32RuleBound(Value value)
	{
		static_assert(std::integral<Value> && !std::same_as<Value, bool>, "json_min/json_max 的边界必须是整数");
		if constexpr (std::integral<Value> && !std::same_as<Value, bool>)
		{
			if constexpr (std::is_signed_v<Value>)
			{
				if (value < (std::numeric_limits<std::int32_t>::min)()
					|| value > (std::numeric_limits<std::int32_t>::max)())
				{
					throw "json_min/json_max: 边界必须在 int32 [-2147483648,2147483647] 范围内";
				}
			}
			else if (value > static_cast<std::uint32_t>((std::numeric_limits<std::int32_t>::max)()))
			{
				throw "json_min/json_max: 边界必须在 int32 [-2147483648,2147483647] 范围内";
			}
			return static_cast<std::int32_t>(value);
		}
	}

	/**
	 * @brief 创建 int32 最小值注解
	 * @param value int32 可表示的整数常量
	 * @return 最小值注解
	 */
	template <typename Value>
	consteval JsonMinRule json_min(Value value)
	{
		return {jsonInt32RuleBound(value)};
	}

	/**
	 * @brief 创建 int32 最大值注解
	 * @param value int32 可表示的整数常量
	 * @return 最大值注解
	 */
	template <typename Value>
	consteval JsonMaxRule json_max(Value value)
	{
		return {jsonInt32RuleBound(value)};
	}

	/**
	 * @brief 按原有注解顺序确定 JSON 键，并收集字段的输入规则
	 * @tparam MemberInfo 原生成员反射信息
	 * @return 最终 JSON 键、必填与忽略标记，以及非空和数值范围规则
	 */
	template <M::info MemberInfo>
	consteval meta::detail::JsonFieldRules applyJsonFieldAnnotations()
	{
		constexpr auto key = applyKeyAnnotations<MemberInfo>();
		meta::detail::JsonFieldRules result {key.keyName_, key.requiredMember_, key.ignoreMember_};
		using MemberType = [:M::remove_cvref(M::type_of(MemberInfo)):];
		constexpr static auto kAnnotationInfos = std::define_static_array(annotationsOfMemberWithParent(MemberInfo));
		// clang-format off
		template for (constexpr auto annoInfo : kAnnotationInfos)
		{
			constexpr auto value = [:M::constant_of(annoInfo):];
			using AnnotationType = std::remove_cvref_t<decltype(value)>;
			if constexpr (std::same_as<AnnotationType, JsonMinRule> || std::same_as<AnnotationType, JsonMaxRule>)
			{
				static_assert(std::same_as<MemberType, std::int32_t>, "json_min/json_max 仅支持 int32 字段");
				if constexpr (std::same_as<AnnotationType, JsonMinRule>)
				{
					if (!result.minimum || value.value_ > *result.minimum)
					{
						result.minimum = value.value_;
					}
				}
				else if (!result.maximum || value.value_ < *result.maximum)
				{
					result.maximum = value.value_;
				}
			}
			else if constexpr (std::same_as<AnnotationType, JsonNotEmptyRule>)
			{
				static_assert(std::same_as<MemberType, std::string>, "json_not_empty 仅支持 std::string 字段");
				result.notEmpty = true;
			}
		}
		// clang-format on
		if (result.minimum && result.maximum && *result.minimum > *result.maximum)
		{
			throw "json_min/json_max: min > max，数值区间为空";
		}
		return result;
	}
} // namespace hical::anno
