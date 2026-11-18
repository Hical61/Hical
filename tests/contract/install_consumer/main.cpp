/**
 * @file main.cpp
 * @brief 用已安装的 Hical 验证 JSON 往返，以及 OpenAPI 和原生反射配置
 */

#include <core/HttpResponse.h>
#include <core/MetaJson.h>
#ifdef HICAL_HAS_OPENAPI
	#include <core/OpenApiRegistry.h>
	#include <core/OpenApiSchema.h>
#endif

#include <boost/json.hpp>
#include <cstdint>
#include <utility>

static_assert(HICAL_HAS_REFLECTION == HICAL_EXPECT_NATIVE, "安装目标的反射模式不符");
#if HICAL_HAS_REFLECTION
static_assert(__cpp_impl_reflection >= 202506L && __cpp_lib_reflection >= 202506L);
#endif
#if defined(HICAL_HAS_OPENAPI) != HICAL_EXPECT_OPENAPI
	#error "安装目标的 OpenAPI 模式不符"
#endif

namespace
{
	struct InstalledDto
	{
#if HICAL_HAS_REFLECTION
		[[= hical::anno::json_rename("wire_value"), = hical::anno::json_required]] std::int32_t value = 0;
#else
		std::int32_t value = 0;
		HICAL_JSON(InstalledDto, REQUIRED_ALIAS(value, "wire_value"))
#endif
	};
} // namespace

int main()
{
	const auto input = boost::json::parse(R"({"wire_value":42})");
	const auto dto = hical::meta::fromJson<InstalledDto>(input);
	const auto response = hical::HttpResponse::json(hical::meta::toJson(dto));
	if (dto.value != 42 || boost::json::parse(response.body()) != input)
	{
		return 1;
	}
#ifdef HICAL_HAS_OPENAPI
	auto schema = hical::meta::openapi::inputSchema<InstalledDto>();
	if (schema.at("properties").as_object().at("wire_value").as_object().at("type") != "integer")
	{
		return 1;
	}
	hical::meta::openapi::OpenApiRegistry registry;
	registry.addSchema("InstalledDto", std::move(schema));
	if (!registry.hasSchema("InstalledDto"))
	{
		return 1;
	}
#endif
	return 0;
}
