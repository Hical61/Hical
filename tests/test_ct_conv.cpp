/**
 * @file test_ct_conv.cpp
 * @brief CTConv 编译期命名转换与整型转字符串测试
 */

#include "core/Utils/CTConv.h"

#include <gtest/gtest.h>

#include <string>

using namespace hical::CT;

TEST(CtConvTest, SnakeToLowerCamelNormal)
{
	EXPECT_EQ(caseSnakeToLowerCamel("hello_world"), "helloWorld");
	EXPECT_EQ(caseSnakeToLowerCamel("user_name_id"), "userNameId");
	EXPECT_EQ(caseSnakeToLowerCamel("already"), "already");
}

TEST(CtConvTest, SnakeToLowerCamelBoundaries)
{
	EXPECT_EQ(caseSnakeToLowerCamel("a"), "a");
	EXPECT_EQ(caseSnakeToLowerCamel(""), "");
	EXPECT_EQ(caseSnakeToLowerCamel("___foo_bar___"), "fooBar");
	EXPECT_EQ(caseSnakeToLowerCamel("a_b"), "aB");
}

TEST(CtConvTest, SnakeToUpperCamel)
{
	EXPECT_EQ(caseSnakeToUpperCamel("hello_world"), "HelloWorld");
	EXPECT_EQ(caseSnakeToUpperCamel("___foo_bar___"), "FooBar");
	EXPECT_EQ(caseSnakeToUpperCamel("test"), "Test");
	EXPECT_EQ(caseSnakeToUpperCamel(""), "");
}

TEST(CtConvTest, CamelToSnake)
{
	EXPECT_EQ(caseCamelToSnake("helloWorld"), "hello_world");
	EXPECT_EQ(caseCamelToSnake("HelloWorld"), "hello_world");
	EXPECT_EQ(caseCamelToSnake("ABC"), "a_b_c");
	EXPECT_EQ(caseCamelToSnake("already_snake"), "already_snake");
	EXPECT_EQ(caseCamelToSnake(""), "");
}

TEST(CtConvTest, AsciiCharacterCase)
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

TEST(CtConvTest, IntegerToString)
{
	// toString 是 consteval，只能在常量表达式里求值。
	// 注意不能用 constexpr std::string 接住结果——字符串缓冲区的堆分配没法在
	// 常量求值里跨出表达式存活，GCC16 会报 "refers to a result of 'operator new'"。
	// 所以当场用 static_assert 比，属于「用完即毁」的临时分配，合法。
	static_assert(toString(0) == "0");
	static_assert(toString(42) == "42");
	static_assert(toString(-7) == "-7");
	static_assert(toString(9223372036854775807LL) == "9223372036854775807");
}

#endif // __cplusplus >= 202302L
