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
	const auto members = hical::CollectMember::collectNonstaticMemberInfos(^^Derived, ctx);

	bool hasBase = false;
	bool hasDerived = false;
	template for (constexpr auto info : std::define_static_array(members))
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
