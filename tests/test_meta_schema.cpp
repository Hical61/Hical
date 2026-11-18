/**
 * @file test_meta_schema.cpp
 * @brief C++26 反射 JSON Schema 生成测试（MetaSchema）
 */

#include "core/MetaJson.h"

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <vector>

#if HICAL_HAS_REFLECTION

	#include "core/MetaSchema.h"

namespace
{
	struct SchemaDto
	{
		int count = 0;
		std::string name;
		std::optional<int> maybe;
		std::vector<int> items;
	};

	enum class Color
	{
		Red,
		Green
	};
} // namespace

// 检查基本类型和必填字段在 Schema 里的写法。
TEST(MetaSchemaTest, GeneratesPrimitiveProperties)
{
	const auto schema = hical::meta::jsonSchema<SchemaDto>();
	const auto& props = schema.at("properties").as_object();

	EXPECT_EQ(props.at("count").at("type").as_string(), "integer");
	EXPECT_EQ(props.at("name").at("type").as_string(), "string");
	EXPECT_EQ(props.at("items").at("type").as_string(), "array");
}

// optional 字段不会出现在 required 列表里。
TEST(MetaSchemaTest, ExcludesOptionalFieldsFromRequired)
{
	const auto schema = hical::meta::jsonSchema<SchemaDto>();
	const auto& required = schema.at("required").as_array();

	for (const auto& item : required)
	{
		EXPECT_NE(item.as_string(), "maybe");
	}
	EXPECT_EQ(schema.at("properties").as_object().at("maybe").at("nullable").as_bool(), true);
}

// 枚举的 Schema 特化会列出允许的值。
TEST(MetaSchemaTest, GeneratesEnumValuesFromSpecialization)
{
	boost::json::object prop;
	hical::schema::kSchema<Color>(prop);

	EXPECT_EQ(prop.at("type").as_string(), "string");
	EXPECT_EQ(prop.at("enum").as_array().size(), 2u);
}

#else

// 当前编译配置不支持原生反射时跳过。
TEST(MetaSchemaTest, SkipsWhenReflectionIsUnavailable)
{
	GTEST_SKIP() << "当前编译器不支持 C++26 反射（HICAL_HAS_REFLECTION == 0）";
}

#endif
