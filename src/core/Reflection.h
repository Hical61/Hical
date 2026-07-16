/**
 * @file Reflection.h
 * @brief C++26 反射特性检测与基础设施
 * 双路线策略：
 * - 当编译器支持 P2996 反射时（HICAL_HAS_REFLECTION == 1），使用原生反射语法
 * - 否则回退到 C++20 宏 + 模板方案，提供相同的用户 API
 * 检测方式：
 * - __cpp_impl_reflection >= 202306L 且 __cpp_lib_reflection >= 202306L（P2996 标准特性测试宏）
 * - 或 CMake 手动定义 HICAL_FORCE_REFLECTION
 */

#pragma once

// C++26 反射特性检测
// P2996 定义了两个特性测试宏：
//   __cpp_impl_reflection — 核心语言反射支持（^^ 操作符、std::meta::info 等）
//   __cpp_lib_reflection  — 反射库支持（<meta> 头文件）
// 两者都 >= 202306L 才认为完整可用
#if (defined(__cpp_impl_reflection) && __cpp_impl_reflection >= 202306L) \
	&& (defined(__cpp_lib_reflection) && __cpp_lib_reflection >= 202306L)
	#define HICAL_HAS_REFLECTION 1
#elif defined(HICAL_FORCE_REFLECTION)
	#define HICAL_HAS_REFLECTION 1
#else
	#define HICAL_HAS_REFLECTION 0
#endif

#include "HttpTypes.h"
#include <string_view>
#include <type_traits>

namespace hical::meta
{

	/**
	 * @brief 路由信息描述（编译期常量）
	 * 描述一个路由处理器的方法、路径和名称。
	 * 由 HICAL_HANDLER 宏或 C++26 反射自动生成。
	 */
	struct RouteInfo
	{
		HttpMethod method;
		std::string_view path;
		std::string_view handlerName;
	};

	/**
	 * @brief 标记类型，用于检测是否注册了 HICAL_ROUTES
	 */
	template <typename T, typename = void>
	struct HasRouteTable : std::false_type
	{
	};

	template <typename T>
	struct HasRouteTable<T, std::void_t<decltype(T::hicalRouteTable())>> : std::true_type
	{
	};

	/**
	 * @brief 标记类型，用于检测是否注册了 HICAL_JSON
	 */
	template <typename T, typename = void>
	struct HasJsonFields : std::false_type
	{
	};

	template <typename T>
	struct HasJsonFields<T, std::void_t<decltype(T::hicalJsonFields())>> : std::true_type
	{
	};

} // namespace hical::meta
