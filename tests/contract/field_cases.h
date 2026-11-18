/**
 * @file field_cases.h
 * @brief 宏轨和原生轨共用的输入和预期结果
 */

#pragma once

#include <array>
#include <string_view>

namespace hical::test::contract
{

	enum class FieldCaseKind
	{
		hRoomInput,
		hNestedInput,
		hListInput,
		hRoomOutput
	};

	struct FieldCase
	{
		std::string_view id;
		FieldCaseKind kind;
		std::string_view input;
		bool accepted;
		std::string_view expected;
	};

	// 成功时核对实际输出的 JSON，失败时核对错误分类。两轨都用这里的固定预期。
	inline constexpr std::array kFieldCases {
		FieldCase {"valid_room",
				   FieldCaseKind::hRoomInput,
				   R"({"room_name":"大厅","capacity":16})",
				   true,
				   R"({"room_name":"大厅","capacity":16})"},
		FieldCase {"missing_name", FieldCaseKind::hRoomInput, R"({"capacity":16})", false, "missing"},
		FieldCase {"empty_name", FieldCaseKind::hRoomInput, R"({"room_name":"","capacity":16})", false, "not-empty"},
		FieldCase {"missing_capacity", FieldCaseKind::hRoomInput, R"({"room_name":"大厅"})", false, "missing"},
		FieldCase {"below_minimum",
				   FieldCaseKind::hRoomInput,
				   R"({"room_name":"大厅","capacity":0})",
				   false,
				   "minimum"},
		FieldCase {"above_maximum",
				   FieldCaseKind::hRoomInput,
				   R"({"room_name":"大厅","capacity":101})",
				   false,
				   "maximum"},
		FieldCase {"signed_overflow",
				   FieldCaseKind::hRoomInput,
				   R"({"room_name":"大厅","capacity":2147483648})",
				   false,
				   "integer-range"},
		FieldCase {"unsigned_overflow",
				   FieldCaseKind::hRoomInput,
				   R"({"room_name":"大厅","capacity":18446744073709551615})",
				   false,
				   "integer-range"},
		FieldCase {"double_token", FieldCaseKind::hRoomInput, R"({"room_name":"大厅","capacity":1.0})", false, "type"},
		FieldCase {"null_name", FieldCaseKind::hRoomInput, R"({"room_name":null,"capacity":16})", false, "type"},
		FieldCase {"unknown_key",
				   FieldCaseKind::hRoomInput,
				   R"({"room_name":"大厅","capacity":16,"extra":true})",
				   true,
				   R"({"room_name":"大厅","capacity":16})"},
		FieldCase {"ignored_field",
				   FieldCaseKind::hRoomInput,
				   R"({"room_name":"大厅","capacity":16,"hidden":"输入值"})",
				   true,
				   R"({"room_name":"大厅","capacity":16})"},
		FieldCase {"nested_room",
				   FieldCaseKind::hNestedInput,
				   R"({"room":{"room_name":"大厅","capacity":16}})",
				   true,
				   R"({"room":{"room_name":"大厅","capacity":16}})"},
		FieldCase {"nonempty_room_list",
				   FieldCaseKind::hListInput,
				   R"({"rooms":[{"room_name":"大厅","capacity":16},{"room_name":"侧厅","capacity":32}],"total":2})",
				   true,
				   R"({"rooms":[{"room_name":"大厅","capacity":16},{"room_name":"侧厅","capacity":32}],"total":2})"},
		FieldCase {"output_outside_input_range",
				   FieldCaseKind::hRoomOutput,
				   {},
				   true,
				   R"({"room_name":"大厅","capacity":101})"}};

} // namespace hical::test::contract
