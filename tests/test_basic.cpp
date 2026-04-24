#include <gtest/gtest.h>
#include <boost/version.hpp>

// 基础环境测试
TEST(BasicTest, BoostVersion)
{
	EXPECT_GE(BOOST_VERSION, 107800); // Boost >= 1.78
}

TEST(BasicTest, CppStandard)
{
	EXPECT_GE(__cplusplus, 202002L); // C++20
}
