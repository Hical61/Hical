/**
 * @file test_meta_anno.cpp
 * @brief C++26 反射注解框架测试（MetaAnno + MetaJson 注解路径）
 */

#include "core/MetaJson.h"

#include <gtest/gtest.h>

#include <string>

#if HICAL_HAS_REFLECTION

namespace
{
	struct AnnoDto
	{
		[[= hical::anno::json_ignore]] int ignoredField_ = 7;
		[[= hical::anno::json_rename("userName")]] std::string user_name = "bob";
		int plain = 1;
	};

	struct RequiredDto
	{
		[[= hical::anno::json_required]] int mustHave = 0;
	};
} // namespace

TEST(MetaAnnoTest, 序列化_忽略与重命名生效)
{
	AnnoDto dto;
	const auto jv = hical::meta::toJson(dto);

	EXPECT_FALSE(jv.as_object().if_contains("ignoredField_"));
	EXPECT_TRUE(jv.as_object().if_contains("userName"));
	EXPECT_TRUE(jv.as_object().if_contains("plain"));
}

TEST(MetaAnnoTest, 反序列化_重命名字段回填)
{
	boost::json::value jv = boost::json::parse(R"({"userName":"alice","plain":9})");
	const auto dto = hical::meta::fromJson<AnnoDto>(jv);

	EXPECT_EQ(dto.user_name, "alice");
	EXPECT_EQ(dto.plain, 9);
	EXPECT_EQ(dto.ignoredField_, 7); // 被忽略的成员保持默认值
}

TEST(MetaAnnoTest, 反序列化_缺可选字段不报错)
{
	// 回归用例：缺字段应当跳过而不是抛异常（PATCH 语义 / 可选字段）
	boost::json::value jv = boost::json::parse(R"({"plain":1})");
	EXPECT_NO_THROW({
		const auto dto = hical::meta::fromJson<AnnoDto>(jv);
		(void)dto;
	});
}

TEST(MetaAnnoTest, 反序列化_缺必填字段报错)
{
	boost::json::value jv = boost::json::parse(R"({})");
	EXPECT_THROW(hical::meta::fromJson<RequiredDto>(jv), std::exception);
}

#else

TEST(MetaAnnoTest, 反射不可用_跳过)
{
	GTEST_SKIP() << "当前编译器不支持 C++26 反射（HICAL_HAS_REFLECTION == 0）";
}

#endif
