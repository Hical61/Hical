/**
 * @file test_json_contract.cpp
 * @brief 用同一组输入检查宏轨和原生轨，并导出各自的结果
 */

#ifndef HICAL_JSON_CONTRACT_EXPECT_NATIVE
	#error "请在 JsonContractTest 的构建目标里指定预期反射模式"
#endif
#ifdef HICAL_FORCE_REFLECTION
	#error "JsonContractTest 禁止使用 HICAL_FORCE_REFLECTION"
#endif

#include "core/OpenApiSchema.h"
#include "contract/field_cases.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace
{
	namespace json = boost::json;
	namespace meta = hical::meta;
	namespace openapi = hical::meta::openapi;
	namespace fs = std::filesystem;
	using hical::test::contract::FieldCase;
	using hical::test::contract::FieldCaseKind;
	using hical::test::contract::kFieldCases;

	constexpr int kExpectedReflection = HICAL_JSON_CONTRACT_EXPECT_NATIVE;
#ifdef __cpp_impl_reflection
	constexpr long kImplReflection = __cpp_impl_reflection;
#else
	constexpr long kImplReflection = 0;
#endif
#ifdef __cpp_lib_reflection
	constexpr long kLibReflection = __cpp_lib_reflection;
#else
	constexpr long kLibReflection = 0;
#endif
	static_assert(kExpectedReflection == 0 || kExpectedReflection == 1, "预期反射模式只能是 0 或 1");
	static_assert(HICAL_HAS_REFLECTION == kExpectedReflection, "实际启用的反射模式和构建目标指定的不同");
	static_assert(kExpectedReflection == 1 ? kImplReflection >= 202506L && kLibReflection >= 202506L
										   : kImplReflection == 0 && kLibReflection == 0,
				  "原生轨的两个标准反射宏都要达到 202506L，宏轨的两个值都要为 0");

	constexpr std::string_view kHiddenDefault = "保留值";

	struct Room
	{
#if HICAL_HAS_REFLECTION
		[[= hical::anno::json_rename("room_name"),
		  = hical::anno::json_required,
		  = hical::anno::json_not_empty]] std::string roomName;
		[[= hical::anno::json_required,
		  = hical::anno::json_min(1),
		  = hical::anno::json_max(100)]] std::int32_t capacity = 0;
		[[= hical::anno::json_ignore]] std::string hidden {kHiddenDefault};
#else
		std::string roomName;
		std::int32_t capacity = 0;
		std::string hidden {kHiddenDefault};
		HICAL_JSON(Room,
				   ALIAS(roomName, "room_name"),
				   REQUIRED(roomName),
				   NOT_EMPTY(roomName),
				   REQUIRED(capacity),
				   MIN(capacity, 1),
				   MAX(capacity, 100),
				   HICAL_IGNORE(hidden))
#endif
	};

	struct NestedRoom
	{
		Room room;
		HICAL_JSON(NestedRoom, room)
	};

	struct RoomList
	{
		std::vector<Room> rooms;
		std::int32_t total = 0;
		HICAL_JSON(RoomList, rooms, total)
	};

	// ---- 共同契约必须拒绝的形态 ----
	// 递归判定和「成员带显式 JSON 转换」的拒绝分支，原生注解轨里有独立实现。
	// 两个方向都得抛 std::logic_error，否则文档声明支持范围、实际悄悄展开，没人发现。

	struct RecursiveNode
	{
		std::vector<RecursiveNode> children;
		HICAL_JSON(RecursiveNode, children)
	};

	struct CustomConverted
	{
		std::string raw;
		HICAL_JSON(CustomConverted, raw)
	};

	inline void tag_invoke(const boost::json::value_from_tag&, boost::json::value& out, const CustomConverted& value)
	{
		out = value.raw;
	}

	inline CustomConverted tag_invoke(const boost::json::value_to_tag<CustomConverted>&, const boost::json::value& in)
	{
		return CustomConverted {in.as_string().c_str()};
	}

	struct HolderWithCustom
	{
		CustomConverted converted;
		HICAL_JSON(HolderWithCustom, converted)
	};

	std::string_view classifyFailure(std::string_view message)
	{
		if (message.starts_with("fromJson: required field '") && message.ends_with("' is missing"))
		{
			return "missing";
		}
		if (message.starts_with("JSON type mismatch: expected "))
		{
			return "type";
		}
		if (message == "fromJson: 整数超出目标类型的可表示范围")
		{
			return "integer-range";
		}
		if (message.starts_with("fromJson: validation failed for field '"))
		{
			if (message.find("' - not-empty constraint violated") != std::string_view::npos)
			{
				return "not-empty";
			}
			if (message.find("' - minimum constraint violated") != std::string_view::npos)
			{
				return "minimum";
			}
			if (message.find("' - maximum constraint violated") != std::string_view::npos)
			{
				return "maximum";
			}
		}
		return "unexpected";
	}

	json::value readAndWrite(const FieldCase& fieldCase)
	{
		if (fieldCase.kind == FieldCaseKind::hRoomOutput)
		{
			Room room;
			room.roomName = "大厅";
			room.capacity = 101;
			return meta::toJson(room);
		}
		const auto input = json::parse(fieldCase.input);
		switch (fieldCase.kind)
		{
			case FieldCaseKind::hRoomInput:
			{
				const auto room = meta::fromJson<Room>(input);
				EXPECT_EQ(room.hidden, kHiddenDefault);
				return meta::toJson(room);
			}
			case FieldCaseKind::hNestedInput:
			{
				const auto nested = meta::fromJson<NestedRoom>(input);
				EXPECT_EQ(nested.room.hidden, kHiddenDefault);
				return meta::toJson(nested);
			}
			case FieldCaseKind::hListInput:
			{
				const auto list = meta::fromJson<RoomList>(input);
				for (const auto& room : list.rooms)
				{
					EXPECT_EQ(room.hidden, kHiddenDefault);
				}
				return meta::toJson(list);
			}
			case FieldCaseKind::hRoomOutput:
				break;
		}
		throw std::logic_error("无法处理这个契约用例类型");
	}

	void sortRequiredForAssertion(json::value& schema)
	{
		if (auto* object = schema.if_object())
		{
			for (auto& field : *object)
			{
				if (field.key() == "required" && field.value().is_array())
				{
					auto& names = field.value().as_array();
					std::sort(names.begin(),
							  names.end(),
							  [](const auto& left, const auto& right)
							  {
								  return left.as_string() < right.as_string();
							  });
				}
				else
				{
					sortRequiredForAssertion(field.value());
				}
			}
		}
		else if (auto* array = schema.if_array())
		{
			for (auto& item : *array)
			{
				sortRequiredForAssertion(item);
			}
		}
	}

	json::object expectedSchemas()
	{
		const auto input = json::parse(R"({"type":"object","properties":{
			"room_name":{"type":"string","minLength":1},
			"capacity":{"type":"integer","format":"int32","minimum":1,"maximum":100}},
			"required":["capacity","room_name"]})");
		const auto output = json::parse(R"({"type":"object","properties":{
			"room_name":{"type":"string"},
			"capacity":{"type":"integer","format":"int32","minimum":-2147483648,"maximum":2147483647}},
			"required":["capacity","room_name"]})");
		const auto total =
			json::parse(R"({"type":"integer","format":"int32","minimum":-2147483648,"maximum":2147483647})");
		const json::object nestedInput {{"type", "object"}, {"properties", json::object {{"room", input}}}};
		const json::object nestedOutput {{"type", "object"},
										 {"properties", json::object {{"room", output}}},
										 {"required", json::array {"room"}}};
		const json::object listProperties {{"rooms", json::object {{"type", "array"}, {"items", output}}},
										   {"total", total}};
		const json::object listOutput {{"type", "object"},
									   {"properties", listProperties},
									   {"required", json::array {"rooms", "total"}}};
		return {{"input", input},
				{"output", output},
				{"nested_input", nestedInput},
				{"nested_output", nestedOutput},
				{"list_output", listOutput}};
	}

	json::object checkAndCollectSchemas()
	{
		json::object actual {{"input", openapi::inputSchema<Room>()},
							 {"output", openapi::outputSchema<Room>()},
							 {"nested_input", openapi::inputSchema<NestedRoom>()},
							 {"nested_output", openapi::outputSchema<NestedRoom>()},
							 {"list_output", openapi::outputSchema<RoomList>()}};
		const auto expected = expectedSchemas();
		for (const auto& schema : expected)
		{
			SCOPED_TRACE(schema.key());
			auto normalized = actual.at(schema.key());
			sortRequiredForAssertion(normalized);
			EXPECT_EQ(normalized, schema.value());
		}
		return actual;
	}

	void writeReport(const fs::path& destination, const json::object& report)
	{
		auto temporary = destination;
		temporary += ".tmp";
		ASSERT_FALSE(fs::exists(fs::symlink_status(temporary))) << "临时结果文件已经存在，请换一个新目录";
		const auto bytes = json::serialize(report);
		std::ofstream stream(temporary, std::ios::binary);
		ASSERT_TRUE(stream.is_open()) << "无法创建临时结果文件";
		stream << bytes << '\n';
		stream.close();
		std::error_code cleanupError;
		if (!stream)
		{
			fs::remove(temporary, cleanupError);
			FAIL() << "写入契约结果失败";
		}
		std::error_code renameError;
		fs::rename(temporary, destination, renameError);
		if (renameError)
		{
			fs::remove(temporary, cleanupError);
			FAIL() << "无法把临时文件改为最终结果文件：" << renameError.message();
		}
	}
} // namespace

TEST(JsonContractTest, ExportsGoldenCases)
{
	std::optional<fs::path> destination;
	if (const auto* output = std::getenv("HICAL_JSON_CONTRACT_OUTPUT"); output && *output)
	{
		destination.emplace(output);
		ASSERT_FALSE(fs::exists(fs::symlink_status(*destination))) << "结果文件已经存在，请换一个新目录";
	}
	json::array cases;
	for (const auto& fieldCase : kFieldCases)
	{
		SCOPED_TRACE(fieldCase.id);
		std::optional<json::value> result;
		std::string_view failure;
		try
		{
			result = readAndWrite(fieldCase);
		}
		catch (const std::runtime_error& error)
		{
			failure = classifyFailure(error.what());
			EXPECT_NE(failure, "unexpected") << error.what();
		}
		ASSERT_EQ(result.has_value(), fieldCase.accepted);
		if (result)
		{
			EXPECT_EQ(*result, json::parse(fieldCase.expected));
		}
		else
		{
			EXPECT_EQ(failure, fieldCase.expected);
		}
		cases.push_back(json::object {{"id", fieldCase.id},
									  {"accepted", result.has_value()},
									  {"result", result ? std::move(*result) : json::value(failure)}});
	}
	auto schemas = checkAndCollectSchemas();
	if (destination && !HasFailure())
	{
		const json::object build {{"expected_reflection", kExpectedReflection},
								  {"hical_has_reflection", HICAL_HAS_REFLECTION},
								  {"cpp_impl_reflection", kImplReflection},
								  {"cpp_lib_reflection", kLibReflection}};
		writeReport(*destination,
					{{"mode", kExpectedReflection == 1 ? "native" : "macro"},
					 {"build", build},
					 {"cases", std::move(cases)},
					 {"schemas", std::move(schemas)}});
	}
}

// 两条轨对同一批「超出共同契约」的形态必须给出一致结论：抛 std::logic_error。
// 原生注解轨的递归判定和成员定制判定是独立实现，光有正向 Schema 断言盯不住它们。
TEST(JsonContractTest, RejectsShapesOutsideTheSharedContract)
{
	EXPECT_THROW((void)openapi::inputSchema<RecursiveNode>(), std::logic_error);
	EXPECT_THROW((void)openapi::outputSchema<RecursiveNode>(), std::logic_error);
	EXPECT_THROW((void)openapi::inputSchema<HolderWithCustom>(), std::logic_error);
	EXPECT_THROW((void)openapi::outputSchema<HolderWithCustom>(), std::logic_error);
}
