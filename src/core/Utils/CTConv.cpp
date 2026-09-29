/**
 * @file CTConv.cpp
 * @brief CTConv 命名转换的编译期回归断言
 */

#include "CTConv.h"

using namespace hical::CT;

static_assert(caseSnakeToLowerCamel("hello_world") == "helloWorld");
static_assert(caseSnakeToLowerCamel("___foo_bar___") == "fooBar");
static_assert(caseSnakeToLowerCamel("a") == "a");
static_assert(caseSnakeToLowerCamel("").empty());

static_assert(caseSnakeToUpperCamel("hello_world") == "HelloWorld");
static_assert(caseSnakeToUpperCamel("___foo_bar___") == "FooBar");
static_assert(caseSnakeToUpperCamel("test") == "Test");
static_assert(caseSnakeToUpperCamel("").empty());

static_assert(caseCamelToSnake("helloWorld") == "hello_world");
static_assert(caseCamelToSnake("HelloWorld") == "hello_world");
static_assert(caseCamelToSnake("ABC") == "a_b_c");
static_assert(caseCamelToSnake("").empty());