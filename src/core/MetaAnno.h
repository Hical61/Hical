//
// Created by LaoZu on 2026/9/9.
//

#pragma once
#include "Reflection.h"

#if HICAL_HAS_REFLECTION
#include <boost/json.hpp>
#include <meta>

namespace hical::anno {
	namespace json = boost::json;
    namespace M = std::meta;

	/// @brief 获取成员字段及其所属类的全部注解
	/// @param memberInfo 成员字段的反射元信息
	/// @return 注解元信息数组，类注解在前，成员字段注解在后
	consteval std::vector<M::info> annotationsOfMemberWithParent(const M::info memberInfo) {
		std::vector<M::info> result;
		result.append_range(M::annotations_of(M::parent_of(memberInfo)));
		result.append_range(M::annotations_of(memberInfo));
		return result;
	}

	/// @brief Key 阶段注解处理结果
	/// @details 由 applyKeyAnnotations() 返回，决定成员字段在 JSON 中的表示方式
	struct MemberAnnotationResult {
		bool ignoreMember_{false};		///< 为 true 时跳过该成员，不参与序列化/反序列化
		std::string_view keyName_;		///< 映射后的 JSON key 名称
	};

	/// @brief Key 注解处理器基类（CRTP）
	/// @details 自定义 key 注解需继承此类并实现 static consteval impl(MemberAnnotationResult&) 方法。
	///          通过 CRTP 的 applyAnnotation() 转发到子类的 impl()。
	struct KeyAnnotationHandler {
		/// @brief 将注解应用到结果上（CRTP 转发）
		/// @param self 派生类自身引用
		/// @param result 注解处理结果，由 impl() 修改
		/// @param memberInfo 成员字段的反射元信息
		consteval void applyAnnotation(this auto& self, MemberAnnotationResult &result, const M::info memberInfo) {
			if constexpr (requires{ { self.impl(result) } -> std::same_as<void>; }) {
				self.impl(result);
			}
			else {
				self.impl(result, memberInfo);
			}
		}
	};

	/// @brief 应用 key 相关注解（编译期）
	/// @tparam MemberInfo 成员字段的反射元信息
	/// @return MemberAnnotationResult 包含 skip 标志和映射后的 key 名称
	/// @details 遍历成员及其所属类的全部注解，仅处理继承自 KeyAnnotationHandler 的注解类型。
	///          注解按声明顺序执行，后续注解可覆盖前序注解的结果。
	template <M::info MemberInfo>
	consteval auto applyKeyAnnotations() -> MemberAnnotationResult {
		MemberAnnotationResult result;
		result.keyName_ = M::identifier_of(MemberInfo);

		constexpr static auto kAnnotationInfos = std::define_static_array(annotationsOfMemberWithParent(MemberInfo));
		template for (constexpr auto annotationInfo : kAnnotationInfos)
			if constexpr (std::derived_from<typename [:M::type_of(annotationInfo):], KeyAnnotationHandler>)
				[:M::constant_of(annotationInfo):].applyAnnotation(result, MemberInfo);

		return result;
	}
}

// ================================================================================================
//  内置注解类型
// ================================================================================================
namespace hical {
	namespace M = std::meta;

	/// @brief 跳过注解：标记该成员不参与序列化/反序列化
	/// @code
	///		[[=Anno::skip]] int internal_counter;
	/// @endcode
	struct IgnoreKeyHandler : anno::KeyAnnotationHandler {
		static consteval void impl(anno::MemberAnnotationResult &result) { result.ignoreMember_ = true; }
	}inline constexpr json_ignore{};

	/// @brief 重命名注解：将成员字段映射为指定的 JSON key 名称
	/// @code
	///		[[=Anno::rename("custom_name")]] int field;
	/// @endcode
	struct RenameKeyHandler : anno::KeyAnnotationHandler {
		const char* name_;		///< 目标 JSON key 名称（编译期静态字符串）
		consteval void impl(anno::MemberAnnotationResult &result) const { result.keyName_ = name_; }
	};
	/// @brief 创建重命名注解实例
	/// @param rename 目标 JSON key 名称
	/// @return RenameKeyHandler 注解实例，内部持有编译期静态字符串
	consteval RenameKeyHandler rename(const std::string_view rename) { return RenameKeyHandler{ .name_ = std::define_static_string(rename) }; }
}

#endif