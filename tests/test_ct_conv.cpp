/**
 * @file test_ct_conv.cpp
 * @brief CTConv 编译期命名转换与整型转字符串测试
 */

#include "core/Utils/CTConv.h"

#include <gtest/gtest.h>

#include <string>

using namespace hical::CT;

TEST(CtConvTest, 蛇形转小驼峰_常规)
{
	EXPECT_EQ(caseSnakeToLowerCamel("hello_world"), "helloWorld");
	EXPECT_EQ(caseSnakeToLowerCamel("user_name_id"), "userNameId");
	EXPECT_EQ(caseSnakeToLowerCamel("already"), "already");
}

TEST(CtConvTest, 蛇形转小驼峰_边界)
{
	EXPECT_EQ(caseSnakeToLowerCamel("a"), "a");
	EXPECT_EQ(caseSnakeToLowerCamel(""), "");
	EXPECT_EQ(caseSnakeToLowerCamel("___foo_bar___"), "fooBar");
	EXPECT_EQ(caseSnakeToLowerCamel("a_b"), "aB");
}

TEST(CtConvTest, 蛇形转大驼峰)
{
	EXPECT_EQ(caseSnakeToUpperCamel("hello_world"), "HelloWorld");
	EXPECT_EQ(caseSnakeToUpperCamel("___foo_bar___"), "FooBar");
	EXPECT_EQ(caseSnakeToUpperCamel("test"), "Test");
	EXPECT_EQ(caseSnakeToUpperCamel(""), "");
}

TEST(CtConvTest, 驼峰转蛇形)
{
	EXPECT_EQ(caseCamelToSnake("helloWorld"), "hello_world");
	EXPECT_EQ(caseCamelToSnake("HelloWorld"), "hello_world");
	EXPECT_EQ(caseCamelToSnake("ABC"), "a_b_c");
	EXPECT_EQ(caseCamelToSnake("already_snake"), "already_snake");
	EXPECT_EQ(caseCamelToSnake(""), "");
}

TEST(CtConvTest, 字符大小写转换)
{
	EXPECT_EQ(asciiToUpper('a'), 'A');
	EXPECT_EQ(asciiToUpper('z'), 'Z');
	EXPECT_EQ(asciiToUpper('A'), 'A');
	EXPECT_EQ(asciiToUpper('1'), '1');
	EXPECT_EQ(asciiToUpper('_'), '_');
	EXPECT_EQ(asciiToLower('A'), 'a');
	EXPECT_EQ(asciiToLower('a'), 'a');
	EXPECT_EQ(asciiToLower('_'), '_');
}

#if __cplusplus >= 202302L

TEST(CtConvTest, 整型转字符串)
{
	// toString 是 consteval，只能在常量表达式中调用
	constexpr std::string zero = toString(0);
	constexpr std::string positive = toString(42);
	constexpr std::string negative = toString(-7);
	constexpr std::string wide = toString(9223372036854775807LL);

	EXPECT_EQ(zero, "0");
	EXPECT_EQ(positive, "42");
	EXPECT_EQ(negative, "-7");
	EXPECT_EQ(wide, "9223372036854775807");
}

#endif // __cplusplus >= 202302L
