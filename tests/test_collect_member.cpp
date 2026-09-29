/**
 * @file test_collect_member.cpp
 * @brief C++26 反射继承链成员收集测试（CollectMember）
 */

#include "core/Reflection.h"

#include <gtest/gtest.h>

#if HICAL_HAS_REFLECTION

	#include "core/Utils/CollectMember.h"

	#include <meta>

namespace
{
	struct Base
	{
		int baseField = 0;
	};

	struct Derived : Base
	{
		int derivedField = 0;
	};
} // namespace

TEST(CollectMemberTest, 收集_包含基类成员)
{
	constexpr auto ctx = std::meta::access_context::unprivileged();

	// 结果不能先存成变量：collectNonstaticMemberInfos 是 consteval 且返回
	// std::vector（带堆分配），存下来会让分配跨出常量求值而报 operator new。
	// 直接用 define_static_array 包住，让它在同一个常量表达式里用完即毁——
	// 这也是库内的用法。
	// static 是必须的：函数内的 constexpr 变量每次调用地址都可能不同，
	// 而 template for 要求固定地址。
	constexpr static auto kMembers =
		std::define_static_array(hical::CollectMember::collectNonstaticMemberInfos(^^Derived, ctx));

	bool hasBase = false;
	bool hasDerived = false;
	template for (constexpr auto info : kMembers)
	{
		if (std::meta::identifier_of(info) == "baseField")
		{
			hasBase = true;
		}
		if (std::meta::identifier_of(info) == "derivedField")
		{
			hasDerived = true;
		}
	}

	EXPECT_TRUE(hasBase);
	EXPECT_TRUE(hasDerived);
}

#else

TEST(CollectMemberTest, 反射不可用_跳过)
{
	GTEST_SKIP() << "当前编译器不支持 C++26 反射（HICAL_HAS_REFLECTION == 0）";
}

#endif
