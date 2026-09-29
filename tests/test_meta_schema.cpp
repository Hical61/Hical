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

TEST(MetaSchemaTest, 生成_基本类型与必填)
{
	const auto schema = hical::meta::jsonSchema<SchemaDto>();
	const auto& props = schema.at("properties").as_object();

	EXPECT_EQ(props.at("count").at("type").as_string(), "integer");
	EXPECT_EQ(props.at("name").at("type").as_string(), "string");
	EXPECT_EQ(props.at("items").at("type").as_string(), "array");
}

TEST(MetaSchemaTest, 生成_optional不进required)
{
	const auto schema = hical::meta::jsonSchema<SchemaDto>();
	const auto& required = schema.at("required").as_array();

	for (const auto& item : required)
	{
		EXPECT_NE(item.as_string(), "maybe");
	}
	EXPECT_EQ(schema.at("properties").as_object().at("maybe").at("nullable").as_bool(), true);
}

TEST(MetaSchemaTest, 枚举特化生成enum数组)
{
	boost::json::object prop;
	hical::schema::kSchema<Color>(prop);

	EXPECT_EQ(prop.at("type").as_string(), "string");
	EXPECT_EQ(prop.at("enum").as_array().size(), 2u);
}

#else

TEST(MetaSchemaTest, 反射不可用_跳过)
{
	GTEST_SKIP() << "当前编译器不支持 C++26 反射（HICAL_HAS_REFLECTION == 0）";
}

#endif
