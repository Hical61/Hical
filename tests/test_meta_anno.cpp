/**
 * @file test_meta_anno.cpp
 * @brief C++26 反射注解框架测试（MetaAnno + MetaJson 注解路径）
 */

#include "core/MetaJson.h"

#include <gtest/gtest.h>

#include <string>
#include <cstdint>
#include <limits>
#include <vector>

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

	struct[[= hical::anno::json_snake_to_lowerCamel]] NativeRulesDto
	{
		[[= hical::anno::json_rename("room_name"),
		  = hical::anno::json_required,
		  = hical::anno::json_not_empty]] std::string room_name = "大厅";
		[[= hical::anno::json_min(1),
		  = hical::anno::json_max(100),
		  = hical::anno::json_required]] std::int32_t room_capacity = 16;
		[[= hical::anno::json_rename("other"), = hical::anno::json_ignore, = hical::anno::json_reset]] int reset_field =
			7;
		[[= hical::anno::json_ignore]] int hidden_field = 9;
	};

	struct[[= hical::anno::json_min(1), = hical::anno::json_max(100)]] NativeNumericClass
	{
		[[= hical::anno::json_min(2), = hical::anno::json_max(50), = hical::anno::json_reset]] std::int32_t capacity =
			16;
	};

	struct NativeDuplicateKeys
	{
		[[= hical::anno::json_rename("value")]] int first = 1;
		[[= hical::anno::json_rename("value")]] int second = 2;
	};

	struct NativeIgnoredDuplicateKey
	{
		[[= hical::anno::json_rename("value")]] int first = 1;
		[[= hical::anno::json_rename("value"), = hical::anno::json_ignore]] int second = 2;
	};

	struct NativeRoomLists
	{
		std::vector<NativeRulesDto> rooms;
		std::vector<std::vector<NativeRulesDto>> groups;
		std::vector<bool> flags;
	};

	struct NativeConstListItem
	{
		[[= hical::anno::json_rename("wire")]] std::int32_t value = 7;
	};

	struct NativeConstListOutput
	{
		const std::vector<NativeConstListItem> items {{}, {8}};
	};

	struct NativeIgnoredConstMember
	{
		[[= hical::anno::json_ignore]] const std::int32_t hidden = 9;
		std::int32_t visible = 1;
	};

	struct ShiftMember
	{
		static void applyAnnotationSerialize(std::int32_t value, std::optional<boost::json::value>& result)
		{
			result = value + 10;
		}

		static void applyAnnotationDeserialize(const boost::json::value& value, std::optional<std::int32_t>& result)
		{
			result = boost::json::value_to<std::int32_t>(value) - 10;
		}
	} constexpr kShiftMember {};

	struct FixOwnMember : hical::anno::ViewAnnotationHandler
	{
		template <hical::anno::IsViewType View>
		static void impl(const View& view)
		{
			if constexpr (!View::kIsSerializeDirection)
			{
				view.classValue_.capacity = 5;
			}
		}
	} constexpr kFixOwnMember {};

	struct ChangeEarlierMember : hical::anno::ViewAnnotationHandler
	{
		template <hical::anno::IsViewType View>
		static void impl(const View& view)
		{
			if constexpr (!View::kIsSerializeDirection)
			{
				view.classValue_.aCapacity = 101;
			}
		}
	} constexpr kChangeEarlierMember {};

	struct NativeCustomMember
	{
		[[= kShiftMember, = hical::anno::json_min(1), = hical::anno::json_max(100)]] std::int32_t capacity = 1;
	};

	struct NativeOwnView
	{
		[[= kFixOwnMember, = hical::anno::json_min(1), = hical::anno::json_max(100)]] mutable std::int32_t capacity = 1;
	};

	struct NativeLaterView
	{
		[[= hical::anno::json_min(1), = hical::anno::json_max(100)]] mutable std::int32_t aCapacity = 1;
		[[= kChangeEarlierMember]] int zTrigger = 0;
	};

	struct FinalChild
	{
		[[= hical::anno::json_min(1), = hical::anno::json_max(100)]] std::int32_t capacity = 7;
		[[= hical::anno::json_max(100)]] std::int32_t absent = 101;
	};

	struct UnruledChild
	{
		std::int32_t capacity = 0;
	};

	enum class ChildMutation
	{
		hRoom = 1,
		hListItem,
		hPopAndChange,
		hReverseAndChange,
		hAppendOnly,
		hRepairRoom,
		hReverseAndChangeLast,
		hAppendUnruled
	};

	struct MutateChildView : hical::anno::ViewAnnotationHandler
	{
		template <hical::anno::IsViewType View>
		static void impl(const View& view)
		{
			if constexpr (!View::kIsSerializeDirection)
			{
				switch (static_cast<ChildMutation>(view.memberValue_))
				{
					case ChildMutation::hRoom:
						view.classValue_.room.capacity = 101;
						break;
					case ChildMutation::hPopAndChange:
						view.classValue_.rooms.pop_back();
						[[fallthrough]];
					case ChildMutation::hListItem:
						view.classValue_.rooms[0].capacity = 101;
						break;
					case ChildMutation::hReverseAndChange:
						std::reverse(view.classValue_.rooms.begin(), view.classValue_.rooms.end());
						view.classValue_.rooms[0].capacity = 101;
						break;
					case ChildMutation::hAppendOnly:
						view.classValue_.rooms.push_back(FinalChild {101});
						break;
					case ChildMutation::hRepairRoom:
						view.classValue_.room.capacity = 100;
						break;
					case ChildMutation::hReverseAndChangeLast:
						std::reverse(view.classValue_.rooms.begin(), view.classValue_.rooms.end());
						view.classValue_.rooms.back().capacity = 101;
						break;
					case ChildMutation::hAppendUnruled:
						view.classValue_.extras.push_back(UnruledChild {101});
						break;
				}
			}
		}
	} constexpr kMutateChildView {};

	struct NativeParentView
	{
		mutable std::vector<UnruledChild> extras;
		mutable FinalChild room;
		mutable std::vector<FinalChild> rooms;
		[[= kMutateChildView]] int trigger = 0;
	};

	struct KeepChildView : hical::anno::ViewAnnotationHandler
	{
		template <hical::anno::IsViewType View>
		static void impl(const View&)
		{
		}
	} constexpr kKeepChildView {};

	struct ChangeRoomView : hical::anno::ViewAnnotationHandler
	{
		template <hical::anno::IsViewType View>
		static void impl(const View& view)
		{
			if constexpr (!View::kIsSerializeDirection)
			{
				view.classValue_.room.capacity = 101;
			}
		}
	} constexpr kChangeRoomView {};

	struct DecodeChild
	{
		inline static int calls_ = 0;

		static void applyAnnotationDeserialize(const boost::json::value&, std::optional<FinalChild>& result)
		{
			++calls_;
			result = FinalChild {101};
		}
	} constexpr kDecodeChild {};

	struct FallbackChild
	{
		inline static int calls_ = 0;

		static void applyAnnotationDeserialize(const boost::json::value&, std::optional<FinalChild>& result)
		{
			++calls_;
			result.reset();
		}
	} constexpr kFallbackChild {};

	struct NativeCustomChildView
	{
		[[= kDecodeChild]] FinalChild room;
		[[= kFallbackChild]] FinalChild fallback;
		[[= kKeepChildView]] int trigger = 0;
	};

	struct ChangeFallbackView : hical::anno::ViewAnnotationHandler
	{
		template <hical::anno::IsViewType View>
		static void impl(const View& view)
		{
			if constexpr (!View::kIsSerializeDirection)
			{
				view.classValue_.room.capacity = 101;
			}
		}
	} constexpr kChangeFallbackView {};

	struct NativeFallbackChildView
	{
		[[= kFallbackChild]] mutable FinalChild room;
		[[= kChangeFallbackView]] int trigger = 0;
	};

	struct DecodeChildIfScalar
	{
		inline static int calls_ = 0;

		static void applyAnnotationDeserialize(const boost::json::value& value, std::optional<FinalChild>& result)
		{
			++calls_;
			if (value.is_int64())
			{
				result = FinalChild {101};
			}
		}
	} constexpr kDecodeChildIfScalar {};

	struct ConditionallyMappedChild
	{
		[[= kDecodeChildIfScalar]] FinalChild room;
	};

	struct ConditionallyMappedList
	{
		std::vector<ConditionallyMappedChild> rooms;
		[[= kKeepChildView]] int trigger = 0;
	};
} // namespace

namespace explicit_vector
{
	struct Scalar
	{
		std::int32_t value;
	};

	void tag_invoke(boost::json::value_from_tag, boost::json::value& output, const Scalar& value)
	{
		output = value.value + 1;
	}

	Scalar tag_invoke(boost::json::value_to_tag<Scalar>, const boost::json::value& input)
	{
		return {boost::json::value_to<std::int32_t>(input) - 1};
	}

	struct ScalarEnvelope
	{
		Scalar item;
	};

	struct Item
	{
		std::int32_t value;
	};

	using Items = std::vector<Item>;

	void tag_invoke(boost::json::value_from_tag, boost::json::value& output, const Items& items)
	{
		output = boost::json::object {{"custom", items.front().value}};
	}

	Items tag_invoke(boost::json::value_to_tag<Items>, const boost::json::value& input)
	{
		return {{boost::json::value_to<std::int32_t>(input.as_object().at("custom"))}};
	}

	struct Envelope
	{
		Items entries;
	};

	struct TryItem
	{
		std::int32_t value;
	};

	using TryItems = std::vector<TryItem>;

	boost::system::result<TryItems> tag_invoke(boost::json::try_value_to_tag<TryItems>, const boost::json::value& input)
	{
		return TryItems {{boost::json::value_to<std::int32_t>(input.as_object().at("custom"))}};
	}

	struct TryEnvelope
	{
		TryItems entries;
	};

	struct NonDefaultItem
	{
		NonDefaultItem() = delete;

		explicit NonDefaultItem(std::int32_t value) : value_(value)
		{
		}

		std::int32_t value_;
	};

	NonDefaultItem tag_invoke(boost::json::value_to_tag<NonDefaultItem>, const boost::json::value& input)
	{
		return NonDefaultItem(boost::json::value_to<std::int32_t>(input));
	}

	struct NonDefaultEnvelope
	{
		std::vector<NonDefaultItem> entries;
	};

	struct TaggedChild
	{
		[[= hical::anno::json_min(1), = hical::anno::json_max(100)]] std::int32_t capacity = 7;
	};

	TaggedChild tag_invoke(boost::json::value_to_tag<TaggedChild>, const boost::json::value&)
	{
		return {101};
	}

	struct TaggedParentView
	{
		TaggedChild room;
		[[= kKeepChildView]] int trigger = 0;
	};

	struct OutputChild
	{
		[[= hical::anno::json_min(1), = hical::anno::json_max(100)]] std::int32_t capacity = 7;
	};

	void tag_invoke(boost::json::value_from_tag, boost::json::value& output, const OutputChild& value)
	{
		output = boost::json::object {{"custom", value.capacity}};
	}

	struct OutputParentView
	{
		mutable OutputChild room;
		[[= kChangeRoomView]] int trigger = 0;
	};
} // namespace explicit_vector

TEST(MetaAnnoTest, ParentViewRevalidatesNestedAndExistingListValues)
{
	for (const auto* input : {R"({"room":{"capacity":1},"trigger":1})",
							  R"({"rooms":[{"capacity":1},{"capacity":2}],"trigger":2})",
							  R"({"rooms":[{"capacity":1},{"capacity":2}],"trigger":3})",
							  R"({"rooms":[{"capacity":1},{"capacity":2}],"trigger":4})"})
	{
		SCOPED_TRACE(input);
		EXPECT_THROW((void)hical::meta::fromJson<NativeParentView>(boost::json::parse(input)), std::runtime_error);
	}
}

TEST(MetaAnnoTest, ParentViewPreservesMissingPathsAndNewListElements)
{
	const auto absent = hical::meta::fromJson<NativeParentView>(boost::json::parse(R"({"trigger":1})"));
	EXPECT_EQ(absent.room.capacity, 101);
	const auto omitted = hical::meta::fromJson<NativeParentView>(boost::json::parse(R"({"room":{},"trigger":1})"));
	EXPECT_EQ(omitted.room.capacity, 101);
	const auto appended = hical::meta::fromJson<NativeParentView>(boost::json::parse(R"({"rooms":[{}],"trigger":5})"));
	ASSERT_EQ(appended.rooms.size(), 2);
	EXPECT_EQ(appended.rooms[0].capacity, 7);
	EXPECT_EQ(appended.rooms[1].capacity, 101);
	const auto unruled =
		hical::meta::fromJson<NativeParentView>(boost::json::parse(R"({"extras":[{"capacity":-1}],"trigger":8})"));
	ASSERT_EQ(unruled.extras.size(), 2);
	EXPECT_EQ(unruled.extras[0].capacity, -1);
	EXPECT_EQ(unruled.extras[1].capacity, 101);
}

TEST(MetaAnnoTest, ParentViewCanRepairAnInputRuleBeforeItIsChecked)
{
	const auto repaired =
		hical::meta::fromJson<NativeParentView>(boost::json::parse(R"({"room":{"capacity":101},"trigger":6})"));
	EXPECT_EQ(repaired.room.capacity, 100);
	EXPECT_EQ(repaired.room.absent, 101);
	EXPECT_THROW((void)hical::meta::fromJson<NativeParentView>(boost::json::parse(R"({"room":{"capacity":101}})")),
				 std::runtime_error);
	EXPECT_THROW(
		(void)hical::meta::fromJson<NativeParentView>(boost::json::parse(R"({"room":{"capacity":1.0},"trigger":6})")),
		std::runtime_error);
}

TEST(MetaAnnoTest, ParentViewCannotMoveAnInputRuleToAnAbsentListSlot)
{
	const auto ordinary =
		hical::meta::fromJson<NativeParentView>(boost::json::parse(R"({"rooms":[{"capacity":1,"absent":1},{}]})"));
	ASSERT_EQ(ordinary.rooms.size(), 2);
	EXPECT_EQ(ordinary.rooms[1].capacity, 7);
	EXPECT_EQ(ordinary.rooms[1].absent, 101);
	for (const auto* input :
		 {R"({"rooms":[{"capacity":1},{}],"trigger":7})", R"({"rooms":[{"capacity":1}],"trigger":5})"})
	{
		SCOPED_TRACE(input);
		EXPECT_THROW((void)hical::meta::fromJson<NativeParentView>(boost::json::parse(input)), std::runtime_error);
	}
}

TEST(MetaAnnoTest, ParentViewChecksOrdinaryFallbackAfterDeclinedMemberDecode)
{
	FallbackChild::calls_ = 0;
	const auto input = boost::json::parse(R"({"room":{"capacity":1},"trigger":1})");
	EXPECT_THROW((void)hical::meta::fromJson<NativeFallbackChildView>(input), std::runtime_error);
	EXPECT_EQ(FallbackChild::calls_, 1);
}

TEST(MetaAnnoTest, ParentViewRejectsAmbiguousCustomAndOrdinaryListMapping)
{
	DecodeChildIfScalar::calls_ = 0;
	auto input = boost::json::parse(R"({"rooms":[{"room":1},{"room":{"capacity":2}}]})");
	const auto ordinary = hical::meta::fromJson<ConditionallyMappedList>(input);
	ASSERT_EQ(ordinary.rooms.size(), 2);
	EXPECT_EQ(ordinary.rooms[0].room.capacity, 101);
	EXPECT_EQ(ordinary.rooms[1].room.capacity, 2);
	EXPECT_EQ(DecodeChildIfScalar::calls_, 2);

	input.as_object()["trigger"] = 1;
	EXPECT_THROW((void)hical::meta::fromJson<ConditionallyMappedList>(input), std::runtime_error);
	EXPECT_EQ(DecodeChildIfScalar::calls_, 4);
}

TEST(MetaAnnoTest, ParentViewKeepsExplicitAndMemberDecodeBoundaries)
{
	DecodeChild::calls_ = 0;
	FallbackChild::calls_ = 0;
	const auto input = boost::json::parse(R"({"room":{"capacity":1},"fallback":{"capacity":2},"trigger":1})");
	const auto decoded = hical::meta::fromJson<NativeCustomChildView>(input);
	EXPECT_EQ(decoded.room.capacity, 101);
	EXPECT_EQ(decoded.fallback.capacity, 2);
	EXPECT_EQ(DecodeChild::calls_, 1);
	EXPECT_EQ(FallbackChild::calls_, 1);
	const auto tagged = hical::meta::fromJson<explicit_vector::TaggedParentView>(input);
	EXPECT_EQ(tagged.room.capacity, 101);
	EXPECT_THROW((void)hical::meta::fromJson<explicit_vector::OutputParentView>(input), std::runtime_error);
}

// 检查必填项缺失、空字符串和整数取值边界。
TEST(MetaAnnoTest, ValidatesPresenceStringRangeAndIntegerBounds)
{
	const auto read = [](std::string_view input)
	{
		return hical::meta::fromJson<NativeRulesDto>(boost::json::parse(input));
	};
	EXPECT_NO_THROW((void)read(R"({"room_name":"A","roomCapacity":1})"));
	EXPECT_NO_THROW((void)read(R"({"room_name":"A","roomCapacity":100})"));
	for (const auto* input : {R"({"roomCapacity":1})",
							  R"({"room_name":"A"})",
							  R"({"room_name":"","roomCapacity":1})",
							  R"({"room_name":"A","roomCapacity":0})",
							  R"({"room_name":"A","roomCapacity":101})",
							  R"({"room_name":"A","roomCapacity":2147483648})",
							  R"({"room_name":"A","roomCapacity":-2147483649})",
							  R"({"room_name":"A","roomCapacity":1.0})",
							  R"({"room_name":null,"roomCapacity":1})"})
	{
		SCOPED_TRACE(input);
		EXPECT_THROW((void)read(input), std::runtime_error);
	}
	NativeRulesDto response;
	response.room_capacity = 101;
	EXPECT_EQ(hical::meta::toJson(response).at("roomCapacity"), 101);
}

// 非空列表和嵌套列表转成 JSON 再读回来，数据应保持一致。
TEST(MetaAnnoTest, RoundTripsNonemptyAndNestedDtoLists)
{
	NativeRulesDto first;
	first.room_name = "A";
	first.room_capacity = 1;
	NativeRulesDto second;
	second.room_name = "B";
	second.room_capacity = 100;
	const NativeRoomLists original {{first, second}, {{second, first}, {first}}, {true, false}};
	auto output = hical::meta::toJson(original);
	ASSERT_EQ(output.at("rooms").as_array().size(), 2);
	EXPECT_EQ(output.at("rooms").as_array()[1].as_object().at("room_name"), "B");
	const auto restored = hical::meta::fromJson<NativeRoomLists>(output);
	ASSERT_EQ(restored.rooms.size(), 2);
	ASSERT_EQ(restored.groups.size(), 2);
	ASSERT_EQ(restored.groups[0].size(), 2);
	EXPECT_EQ(restored.rooms[0].room_capacity, 1);
	EXPECT_EQ(restored.groups[0][1].room_name, "A");
	EXPECT_EQ(restored.flags, (std::vector<bool> {true, false}));
	EXPECT_EQ(hical::meta::toJson(restored), output);
	output.at("rooms").as_array()[1].as_object()["room_name"] = "";
	EXPECT_THROW((void)hical::meta::fromJson<NativeRoomLists>(output), std::runtime_error);
}

TEST(MetaAnnoTest, SerializesConstVectorUsingElementAnnotations)
{
	const auto output = hical::meta::toJson(NativeConstListOutput {});
	const boost::json::value expected = boost::json::parse(R"({"items":[{"wire":7},{"wire":8}]})");
	EXPECT_EQ(output, expected.as_object());
}

TEST(MetaAnnoTest, IgnoresConstMemberWithoutInstantiatingItsAssignment)
{
	const auto input = boost::json::parse(R"({"hidden":100,"visible":7})");
	const auto decoded = hical::meta::fromJson<NativeIgnoredConstMember>(input);
	EXPECT_EQ(decoded.hidden, 9);
	EXPECT_EQ(decoded.visible, 7);
	EXPECT_EQ(hical::meta::toJson(decoded), (boost::json::object {{"visible", 7}}));
}

// 有显式 ADL 转换时，先用它，再考虑逐项转换容器。
TEST(MetaAnnoTest, ExplicitAdlConversionsTakePriorityOverVectorRecursion)
{
	static_assert(hical::meta::detail::HasExplicitJsonValueFrom<explicit_vector::Items>);
	static_assert(hical::meta::detail::HasExplicitJsonValueTo<explicit_vector::Items>);
	static_assert(hical::meta::detail::HasExplicitJsonValueTo<explicit_vector::TryItems>);
	static_assert(!hical::meta::detail::HasExplicitJsonValueFrom<std::vector<NativeRulesDto>>);
	static_assert(!hical::meta::detail::HasExplicitJsonValueTo<std::vector<NativeRulesDto>>);
	const explicit_vector::Envelope original {{{17}, {18}}};
	const auto output = hical::meta::toJson(original);
	EXPECT_EQ(output.at("entries").as_object().at("custom"), 17);
	const auto restored = hical::meta::fromJson<explicit_vector::Envelope>(output);
	ASSERT_EQ(restored.entries.size(), 1);
	EXPECT_EQ(restored.entries[0].value, 17);
	const auto tried = hical::meta::fromJson<explicit_vector::TryEnvelope>(output);
	ASSERT_EQ(tried.entries.size(), 1);
	EXPECT_EQ(tried.entries[0].value, 17);
	const auto scalar = hical::meta::toJson(explicit_vector::ScalarEnvelope {{41}});
	EXPECT_EQ(scalar.at("item"), 42);
	EXPECT_EQ(hical::meta::fromJson<explicit_vector::ScalarEnvelope>(scalar).item.value, 41);
}

TEST(MetaAnnoTest, ExplicitAdlDecodesVectorElementsWithoutDefaultConstructor)
{
	static_assert(!std::default_initializable<explicit_vector::NonDefaultItem>);
	const auto input = boost::json::parse(R"({"entries":[17,18]})");
	const auto decoded = hical::meta::fromJson<explicit_vector::NonDefaultEnvelope>(input);
	ASSERT_EQ(decoded.entries.size(), 2);
	EXPECT_EQ(decoded.entries[0].value_, 17);
	EXPECT_EQ(decoded.entries[1].value_, 18);
}

// 等成员转换和 View 执行完，再检查最终的字段值。
TEST(MetaAnnoTest, ValidatesFinalValuesAfterMemberConversionsAndViews)
{
	const auto input = boost::json::parse(R"({"capacity":15.0})");
	const auto decoded = hical::meta::fromJson<NativeCustomMember>(input);
	EXPECT_EQ(decoded.capacity, 5);
	EXPECT_EQ(hical::meta::toJson(decoded).at("capacity"), 15);
	EXPECT_THROW((void)hical::meta::fromJson<NativeCustomMember>(boost::json::object {{"capacity", 111}}),
				 std::runtime_error);
	const auto fixed = hical::meta::fromJson<NativeOwnView>(boost::json::object {{"capacity", 101}});
	EXPECT_EQ(fixed.capacity, 5);
	EXPECT_THROW((void)hical::meta::fromJson<NativeLaterView>(boost::json::object {{"aCapacity", 5}, {"zTrigger", 1}}),
				 std::runtime_error);
	const auto absent = hical::meta::fromJson<NativeLaterView>(boost::json::object {{"zTrigger", 1}});
	EXPECT_EQ(absent.aCapacity, 101);
}

// 检查应用注解后得到的 JSON 键名和字段规则。
TEST(MetaAnnoTest, ReadsFinalKeysAndFieldRules)
{
	constexpr auto kName = hical::anno::applyJsonFieldAnnotations<^^NativeRulesDto::room_name>();
	constexpr auto kCapacity = hical::anno::applyJsonFieldAnnotations<^^NativeRulesDto::room_capacity>();
	constexpr auto kReset = hical::anno::applyJsonFieldAnnotations<^^NativeRulesDto::reset_field>();
	static_assert(kName.name == "room_name" && kName.required && kName.notEmpty && !kName.ignored);
	static_assert(kCapacity.name == "roomCapacity" && kCapacity.required);
	static_assert(kCapacity.minimum == 1 && kCapacity.maximum == 100);
	static_assert(kReset.name == "reset_field" && !kReset.ignored);
	EXPECT_EQ(kCapacity.minimum, 1);
	EXPECT_EQ(kCapacity.maximum, 100);
}

// 类和成员都声明了数值范围时，只接受两者重叠的部分。
TEST(MetaAnnoTest, IntersectsClassAndMemberNumericRules)
{
	constexpr auto kRules = hical::anno::applyJsonFieldAnnotations<^^NativeNumericClass::capacity>();
	static_assert(kRules.minimum == 2 && kRules.maximum == 50);
	static_assert(hical::anno::json_min(std::numeric_limits<std::int32_t>::min()).value_
				  == std::numeric_limits<std::int32_t>::min());
	static_assert(hical::anno::json_max(2147483647ULL).value_ == std::numeric_limits<std::int32_t>::max());
	EXPECT_EQ(kRules.name, "capacity");
}

// 用实际的 JSON 读写入口检查重命名和重置注解。
TEST(MetaAnnoTest, PreservesRenameAndResetThroughJsonEntryPoints)
{
	const auto output = hical::meta::toJson(NativeRulesDto {});
	ASSERT_EQ(output.size(), 3);
	EXPECT_EQ(output.at("room_name"), "大厅");
	EXPECT_EQ(output.at("roomCapacity"), 16);
	EXPECT_EQ(output.at("reset_field"), 7);
	const auto input = boost::json::parse(R"({"room_name":"A","roomCapacity":32,"reset_field":8})");
	const auto dto = hical::meta::fromJson<NativeRulesDto>(input);
	EXPECT_EQ(dto.room_name, "A");
	EXPECT_EQ(dto.room_capacity, 32);
	EXPECT_EQ(dto.reset_field, 8);
	EXPECT_EQ(dto.hidden_field, 9);
}

// JSON 读写都拒绝两个可见字段使用同一个键。
TEST(MetaAnnoTest, RejectsDuplicateVisibleKeysThroughJsonEntryPoints)
{
	EXPECT_THROW((void)hical::meta::toJson(NativeDuplicateKeys {}), std::logic_error);
	EXPECT_THROW((void)hical::meta::fromJson<NativeDuplicateKeys>(boost::json::object {{"value", 3}}),
				 std::logic_error);
	EXPECT_NO_THROW((void)hical::meta::toJson(NativeIgnoredDuplicateKey {}));
	const auto dto = hical::meta::fromJson<NativeIgnoredDuplicateKey>(boost::json::object {{"value", 3}});
	EXPECT_EQ(dto.first, 3);
	EXPECT_EQ(dto.second, 2);
}

// 写 JSON 时跳过忽略项，并用重命名后的键。
TEST(MetaAnnoTest, SerializesIgnoredAndRenamedFields)
{
	AnnoDto dto;
	const auto jv = hical::meta::toJson(dto);

	EXPECT_FALSE(jv.if_contains("ignoredField_"));
	EXPECT_TRUE(jv.if_contains("userName"));
	EXPECT_TRUE(jv.if_contains("plain"));
}

// 读 JSON 时按重命名后的键找到对应成员。
TEST(MetaAnnoTest, DeserializesRenamedFields)
{
	boost::json::value jv = boost::json::parse(R"({"userName":"alice","plain":9})");
	const auto dto = hical::meta::fromJson<AnnoDto>(jv);

	EXPECT_EQ(dto.user_name, "alice");
	EXPECT_EQ(dto.plain, 9);
	EXPECT_EQ(dto.ignoredField_, 7); // 被忽略的成员保持默认值
}

// 输入省略可选字段时，可以保留默认值。
TEST(MetaAnnoTest, KeepsDefaultsForMissingOptionalFields)
{
	// 回归用例：缺字段应当跳过而不是抛异常（PATCH 语义 / 可选字段）
	boost::json::value jv = boost::json::parse(R"({"plain":1})");
	EXPECT_NO_THROW({
		const auto dto = hical::meta::fromJson<AnnoDto>(jv);
		(void)dto;
	});
}

// 输入少了必填字段时会报错。
TEST(MetaAnnoTest, RejectsMissingRequiredFields)
{
	boost::json::value jv = boost::json::parse(R"({})");
	EXPECT_THROW(hical::meta::fromJson<RequiredDto>(jv), std::exception);
}

namespace
{
	struct NativeIntegerWidthDto
	{
		std::int64_t wideId = 0;
		std::uint32_t narrowCode = 0;
		bool flag = false;
	};

	struct NativeInt64OnlyDto
	{
		std::int64_t id = 0;
	};
} // namespace

// 原生轨的整型统一走严格解码：64 位也要拒浮点写法，bool 只认 true/false。
TEST(MetaAnnoTest, RejectsFloatTokensForAllNativeIntegerWidths)
{
	const auto read = [](std::string_view input)
	{
		return hical::meta::fromJson<NativeIntegerWidthDto>(boost::json::parse(input));
	};
	EXPECT_NO_THROW((void)read(R"({"wideId":9007199254740993,"narrowCode":7,"flag":true})"));
	for (const auto* input : {R"({"wideId":1.0,"narrowCode":7,"flag":true})",
							  R"({"wideId":1,"narrowCode":7.0,"flag":true})",
							  R"({"wideId":1,"narrowCode":7,"flag":1})",
							  R"({"wideId":1,"narrowCode":-1,"flag":true})",
							  R"({"wideId":1,"narrowCode":4294967296,"flag":true})"})
	{
		SCOPED_TRACE(input);
		EXPECT_THROW((void)read(input), std::runtime_error);
	}
}

// 64 位字段的浮点写法以前会被 Boost 的 value_to 接住，1.0 直接当 1 存进去
TEST(MetaAnnoTest, RejectsFloatTokenForInt64Field)
{
	EXPECT_THROW((void)hical::meta::fromJson<NativeInt64OnlyDto>(boost::json::parse(R"({"id":1.0})")),
				 std::runtime_error);
	const auto exact = hical::meta::fromJson<NativeInt64OnlyDto>(boost::json::parse(R"({"id":1})"));
	EXPECT_EQ(exact.id, 1);
}

#else

// 当前编译配置不支持原生反射时跳过。
TEST(MetaAnnoTest, SkipsWhenReflectionIsUnavailable)
{
	GTEST_SKIP() << "当前编译器不支持 C++26 反射（HICAL_HAS_REFLECTION == 0）";
}

#endif
