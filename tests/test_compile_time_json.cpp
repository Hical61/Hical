/**
 * @file test_compile_time_json.cpp
 * @brief CompileTimeJson 编译期直序列化测试
 */

#include "core/CompileTimeJson.h"
#include "core/HttpResponse.h"
#include "core/MetaJson.h"
#include <gtest/gtest.h>
#include <boost/json.hpp>
#include <string>
#include <vector>

using namespace hical;

// ============ 测试用 DTO 结构体 ============

struct SimpleDTO
{
	std::string name;
	int age;

	HICAL_JSON(SimpleDTO, name, age)
};

struct NestedAddress
{
	std::string city;
	std::string street;

	HICAL_JSON(NestedAddress, city, street)
};

struct UserWithAddress
{
	std::string name;
	int age;
	NestedAddress address;

	HICAL_JSON(UserWithAddress, name, age, address)
};

struct WithVector
{
	std::string name;
	std::vector<int> scores;

	HICAL_JSON(WithVector, name, scores)
};

struct WithDouble
{
	double value;
	bool active;

	HICAL_JSON(WithDouble, value, active)
};

struct WithIgnore
{
	std::string publicField;
	std::string privateNote;

	HICAL_JSON(WithIgnore, publicField, HICAL_IGNORE(privateNote))
};

struct WithAlias
{
	std::string firstName;
	int userAge;

	HICAL_JSON(WithAlias, ALIAS(firstName, "first_name"), ALIAS(userAge, "user_age"))
};

struct WithAllTypes
{
	std::string text;
	int count;
	double ratio;
	bool enabled;

	HICAL_JSON(WithAllTypes, text, count, ratio, enabled)
};

struct WithNestedVector
{
	std::string title;
	std::vector<std::string> tags;

	HICAL_JSON(WithNestedVector, title, tags)
};

// 辅助：用 compileTimeToJson 序列化后直接返回 string
template <typename T>
std::string toJson(const T& obj)
{
	return meta::compileTimeToJson(obj);
}

// ============ 基本类型序列化测试 ============

TEST(CompileTimeJsonTest, SimpleDTO_ProducesCorrectJson)
{
	SimpleDTO dto {"Alice", 30};
	auto json = toJson(dto);
	EXPECT_EQ(json, "{\"name\":\"Alice\",\"age\":30}");
}

TEST(CompileTimeJsonTest, BoolFields_TrueAndFalse)
{
	WithDouble dto {3.14, true};
	auto json = toJson(dto);
	EXPECT_EQ(json, "{\"value\":3.14,\"active\":true}");
}

TEST(CompileTimeJsonTest, BoolField_FalseValue)
{
	WithDouble dto {2.0, false};
	auto json = toJson(dto);
	EXPECT_EQ(json, "{\"value\":2,\"active\":false}");
}

TEST(CompileTimeJsonTest, IntField_Zero)
{
	SimpleDTO dto {"", 0};
	auto json = toJson(dto);
	EXPECT_EQ(json, "{\"name\":\"\",\"age\":0}");
}

TEST(CompileTimeJsonTest, IntField_Negative)
{
	SimpleDTO dto {"X", -42};
	auto json = toJson(dto);
	EXPECT_EQ(json, "{\"name\":\"X\",\"age\":-42}");
}

TEST(CompileTimeJsonTest, DoubleField_Fractional)
{
	// std::to_chars general format 会将 1.5 输出为 "1.5"
	WithDouble dto {1.5, false};
	auto json = toJson(dto);
	EXPECT_TRUE(json.find("\"value\":1.5") != std::string::npos);
	EXPECT_TRUE(json.find("\"active\":false") != std::string::npos);
}

// ============ 嵌套结构体测试 ============

TEST(CompileTimeJsonTest, NestedStruct_ProducesNestedJson)
{
	UserWithAddress user {"Dave", 35, {"Beijing", "Chang'an Street"}};
	auto json = toJson(user);

	// 外层字段
	EXPECT_TRUE(json.find("\"name\":\"Dave\"") != std::string::npos);
	EXPECT_TRUE(json.find("\"age\":35") != std::string::npos);

	// 内层嵌套对象
	EXPECT_TRUE(json.find("\"city\":\"Beijing\"") != std::string::npos);
	EXPECT_TRUE(json.find("\"street\":\"Chang'an Street\"") != std::string::npos);
}

// ============ vector 序列化测试 ============

TEST(CompileTimeJsonTest, VectorOfInts_ProducesJsonArray)
{
	WithVector dto {"ScoreSheet", {90, 85, 100}};
	auto json = toJson(dto);
	EXPECT_EQ(json, "{\"name\":\"ScoreSheet\",\"scores\":[90,85,100]}");
}

TEST(CompileTimeJsonTest, VectorOfStrings_ProducesJsonArray)
{
	WithNestedVector dto {"Tags", {"c++", "json", "fast"}};
	auto json = toJson(dto);
	EXPECT_EQ(json, "{\"title\":\"Tags\",\"tags\":[\"c++\",\"json\",\"fast\"]}");
}

TEST(CompileTimeJsonTest, Vector_Empty)
{
	WithVector dto {"Empty", {}};
	auto json = toJson(dto);
	EXPECT_EQ(json, "{\"name\":\"Empty\",\"scores\":[]}");
}

// ============ 字段忽略测试 ============

TEST(CompileTimeJsonTest, IgnoredField_NotSerialized)
{
	WithIgnore dto {"visible", "secret"};
	auto json = toJson(dto);
	// privateNote 不应该出现
	EXPECT_EQ(json, "{\"publicField\":\"visible\"}");
}

// ============ 字段别名测试 ============

TEST(CompileTimeJsonTest, AliasField_UsesAliasName)
{
	WithAlias dto {"John", 28};
	auto json = toJson(dto);
	EXPECT_EQ(json, "{\"first_name\":\"John\",\"user_age\":28}");
}

// ============ JSON 字符串转义测试 ============

TEST(CompileTimeJsonTest, StringEscape_Quotes)
{
	SimpleDTO dto {"He said \"hello\"", 1};
	auto json = toJson(dto);
	EXPECT_EQ(json, "{\"name\":\"He said \\\"hello\\\"\",\"age\":1}");
}

TEST(CompileTimeJsonTest, StringEscape_Backslash)
{
	SimpleDTO dto {"path\\file", 1};
	auto json = toJson(dto);
	EXPECT_EQ(json, "{\"name\":\"path\\\\file\",\"age\":1}");
}

TEST(CompileTimeJsonTest, StringEscape_Slash)
{
	// RFC 要求转义 /（boost::json::serialize 行为）
	SimpleDTO dto {"a/b", 1};
	auto json = toJson(dto);
	EXPECT_EQ(json, "{\"name\":\"a\\/b\",\"age\":1}");
}

TEST(CompileTimeJsonTest, StringEscape_ControlChars)
{
	// 换行、制表符、回车、退格、换页
	SimpleDTO dto {"A\tB\nC\rD", 1};
	auto json = toJson(dto);
	EXPECT_EQ(json, "{\"name\":\"A\\tB\\nC\\rD\",\"age\":1}");
}

TEST(CompileTimeJsonTest, StringEscape_NullChar)
{
	std::string raw;
	raw.push_back('a');
	raw.push_back('\0');
	raw.push_back('b');
	SimpleDTO dto {raw, 1};
	auto json = toJson(dto);
	EXPECT_EQ(json, "{\"name\":\"a\\u0000b\",\"age\":1}");
}

// ============ 边界情况测试 ============

TEST(CompileTimeJsonTest, Int64Max)
{
	struct Int64DTO
	{
		int64_t big;
		HICAL_JSON(Int64DTO, big)
	};

	Int64DTO dto {INT64_MAX};
	auto json = meta::compileTimeToJson(dto);
	EXPECT_TRUE(json.find("\"big\":") != std::string::npos);
}

TEST(CompileTimeJsonTest, StringWithUnicode)
{
	SimpleDTO dto {"\xE4\xBD\xA0\xE5\xA5\xBD", 1}; // UTF-8 "你好"
	auto json = toJson(dto);
	EXPECT_TRUE(json.find("\"name\":\"\xE4\xBD\xA0\xE5\xA5\xBD\"") != std::string::npos);
}

TEST(CompileTimeJsonTest, AllTypes_ComprehensiveRoundtrip)
{
	WithAllTypes dto {"hello", 42, 3.14, true};
	auto json = toJson(dto);
	// 验证每个字段都在 JSON 中
	EXPECT_TRUE(json.find("\"text\":\"hello\"") != std::string::npos);
	EXPECT_TRUE(json.find("\"count\":42") != std::string::npos);
	EXPECT_TRUE(json.find("\"ratio\":") != std::string::npos);
	EXPECT_TRUE(json.find("\"enabled\":true") != std::string::npos);
}

// ============ HttpResponse::jsonFrom<T>() 集成测试 ============

TEST(CompileTimeJsonTest, HttpResponseJsonFrom_BasicDto_CorrectResponse)
{
	SimpleDTO dto {"Alice", 30};
	auto resp = HttpResponse::jsonFrom(dto);

	EXPECT_EQ(resp.statusCode(), HttpStatusCode::hOk);
	EXPECT_EQ(resp.header("Content-Type"), "application/json");
	EXPECT_EQ(resp.body(), "{\"name\":\"Alice\",\"age\":30}");
}

TEST(CompileTimeJsonTest, HttpResponseJsonFrom_SemanticallyEquivalentToRuntime)
{
	WithAllTypes dto {"hello", 42, 3.14, true};

	// 编译期直序列化路径
	auto respJsonFrom = HttpResponse::jsonFrom(dto);

	// 运行时 boost::json 路径：hical::meta::toJson() -> HttpResponse::json()
	auto jsonObj = meta::toJson(dto);
	auto respRuntime = HttpResponse::json(jsonObj);

	// 浮点格式可能不同（3.14 vs 3.14E0），但解析后 JSON 语义等价
	auto parsedFrom = boost::json::parse(respJsonFrom.body());
	auto parsedRuntime = boost::json::parse(respRuntime.body());
	EXPECT_EQ(parsedFrom, parsedRuntime);
}

TEST(CompileTimeJsonTest, HttpResponseJsonFrom_NestedDto_CorrectResponse)
{
	UserWithAddress user {"Dave", 35, {"Beijing", "Chang'an Street"}};
	auto resp = HttpResponse::jsonFrom(user);

	EXPECT_EQ(resp.statusCode(), HttpStatusCode::hOk);
	auto& body = resp.body();
	EXPECT_TRUE(body.find("\"name\":\"Dave\"") != std::string::npos);
	EXPECT_TRUE(body.find("\"age\":35") != std::string::npos);
	EXPECT_TRUE(body.find("\"city\":\"Beijing\"") != std::string::npos);
	EXPECT_TRUE(body.find("\"street\":\"Chang'an Street\"") != std::string::npos);
}

TEST(CompileTimeJsonTest, HttpResponseJsonFrom_VectorField_CorrectResponse)
{
	WithVector dto {"Scores", {90, 85, 100}};
	auto resp = HttpResponse::jsonFrom(dto);

	EXPECT_EQ(resp.statusCode(), HttpStatusCode::hOk);
	EXPECT_EQ(resp.body(), "{\"name\":\"Scores\",\"scores\":[90,85,100]}");
}

TEST(CompileTimeJsonTest, HttpResponseJsonFrom_AliasField_CorrectResponse)
{
	WithAlias dto {"John", 28};
	auto resp = HttpResponse::jsonFrom(dto);

	EXPECT_EQ(resp.statusCode(), HttpStatusCode::hOk);
	EXPECT_EQ(resp.body(), "{\"first_name\":\"John\",\"user_age\":28}");
}

TEST(CompileTimeJsonTest, HttpResponseJsonFrom_IgnoredField_CorrectResponse)
{
	WithIgnore dto {"visible", "secret"};
	auto resp = HttpResponse::jsonFrom(dto);

	EXPECT_EQ(resp.statusCode(), HttpStatusCode::hOk);
	EXPECT_EQ(resp.body(), "{\"publicField\":\"visible\"}");
}
