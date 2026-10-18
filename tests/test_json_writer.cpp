/**
 * @file test_json_writer.cpp
 * @brief JsonWriter 零 DOM 增量序列化测试
 */

#include "core/HttpResponse.h"
#include "core/JsonWriter.h"
#include <gtest/gtest.h>
#include <boost/json.hpp>
#include <limits>
#include <string>
#include <string_view>
#include <type_traits>

using namespace hical;

namespace
{

	/**
	 * @brief 用 boost::json 重新解析一遍，确认拼出来的确实能当 JSON 用
	 * 单纯比字符串只能保证"跟预期逐字节一致"，这里多一道结构校验。
	 */
	void expectParsable(const std::string& text)
	{
		boost::system::error_code ec;
		boost::json::parse(text, ec);
		EXPECT_EQ(ec.value(), 0) << "拼出来的不是合法 JSON: " << text;
	}

	// jsonWriter() 挂了 `&` 限定：从左值响应上取得到，从临时响应上取不到。
	// 临时对象在整条表达式结束时就没了，writer 里那个指向 body 的指针会当场悬空。
	static_assert(std::is_invocable_v<decltype(&HttpResponse::jsonWriter), HttpResponse&>);
	static_assert(!std::is_invocable_v<decltype(&HttpResponse::jsonWriter), HttpResponse&&>);

} // namespace

// ============ 空容器 ============

TEST(JsonWriterTest, EmptyObject_ProducesEmptyBraces)
{
	std::string out;
	JsonWriter w(out);
	w.beginObject();
	w.endObject();

	EXPECT_EQ(out, "{}");
}

TEST(JsonWriterTest, EmptyArray_ProducesEmptyBrackets)
{
	std::string out;
	JsonWriter w(out);
	w.beginArray();
	w.endArray();

	EXPECT_EQ(out, "[]");
}

TEST(JsonWriterTest, EmptyNestedObject_NoExtraComma)
{
	std::string out;
	JsonWriter w(out);
	w.beginObject();
	w.key("a").beginObject().endObject();
	w.key("b").value(1);
	w.endObject();

	EXPECT_EQ(out, R"({"a":{},"b":1})");
}

// ============ 标量 ============

TEST(JsonWriterTest, AllScalarTypes_ProducesExpectedJson)
{
	std::string out;
	JsonWriter w(out);
	w.beginObject();
	w.key("name").value("Alice");
	w.key("age").value(30);
	w.key("score").value(1.5);
	w.key("vip").value(true);
	w.key("extra").value(nullptr);
	w.endObject();

	EXPECT_EQ(out, R"({"name":"Alice","age":30,"score":1.5,"vip":true,"extra":null})");
	expectParsable(out);
}

TEST(JsonWriterTest, BoolValues_BothBranches)
{
	std::string out;
	JsonWriter w(out);
	w.beginArray();
	w.value(true);
	w.value(false);
	w.endArray();

	EXPECT_EQ(out, "[true,false]");
}

TEST(JsonWriterTest, IntegerBoundaries_SignedAndUnsigned)
{
	std::string out;
	JsonWriter w(out);
	w.beginArray();
	w.value(std::numeric_limits<long long>::min());
	w.value(std::numeric_limits<long long>::max());
	w.value(std::numeric_limits<unsigned long long>::max());
	w.value(0);
	w.value(-0);
	w.endArray();

	EXPECT_EQ(out, "[-9223372036854775808,9223372036854775807,18446744073709551615,0,0]");
}

TEST(JsonWriterTest, NarrowIntegers_AllFormattedAsNumbers)
{
	std::string out;
	JsonWriter w(out);
	w.beginArray();
	w.value(static_cast<signed char>(-7));
	w.value(static_cast<unsigned char>(200));
	w.value(static_cast<short>(-300));
	w.value(static_cast<unsigned long>(4000000000UL));
	w.endArray();

	EXPECT_EQ(out, "[-7,200,-300,4000000000]");
}

TEST(JsonWriterTest, DoubleValues_UseShortestForm)
{
	std::string out;
	JsonWriter w(out);
	w.beginArray();
	w.value(2.0);
	w.value(3.14);
	w.value(-0.5);
	w.endArray();

	EXPECT_EQ(out, "[2,3.14,-0.5]");
}

TEST(JsonWriterTest, LargeDouble_RoundTripsThroughParse)
{
	// 科学计数法各实现写法略有差异，只要求解析回来数值一致
	std::string out;
	JsonWriter w(out);
	w.value(1.0e100);

	const auto parsed = boost::json::parse(out);
	EXPECT_EQ(parsed.as_double(), 1.0e100);
}

TEST(JsonWriterTest, FloatValue_SerializedAsDouble)
{
	std::string out;
	JsonWriter w(out);
	w.value(1.5f);

	EXPECT_EQ(out, "1.5");
}

TEST(JsonWriterTest, NonFiniteDouble_FallsBackToNull)
{
	std::string out;
	JsonWriter w(out);
	w.beginArray();
	w.value(std::numeric_limits<double>::quiet_NaN());
	w.value(std::numeric_limits<double>::infinity());
	w.value(-std::numeric_limits<double>::infinity());
	w.endArray();

	EXPECT_EQ(out, "[null,null,null]");
	expectParsable(out);
}

// ============ 字符串转义（规则跟 CompileTimeJson 那套保持一致）============

TEST(JsonWriterTest, StringValue_EscapesQuoteBackslashSlash)
{
	std::string out;
	JsonWriter w(out);
	w.value("a\"b\\c/d");

	EXPECT_EQ(out, R"("a\"b\\c\/d")");
}

TEST(JsonWriterTest, StringValue_EscapesControlChars)
{
	std::string out;
	JsonWriter w(out);
	w.value(std::string_view("\x01\x08\x09\x0a\x0c\x0d\x1f", 7));

	EXPECT_EQ(out, R"("\u0001\b\t\n\f\r\u001f")");
}

TEST(JsonWriterTest, StringValue_EmbeddedNull_Escaped)
{
	std::string raw("a\0b", 3);

	std::string out;
	JsonWriter w(out);
	w.value(raw);

	EXPECT_EQ(out, R"("a\u0000b")");
	expectParsable(out);
}

TEST(JsonWriterTest, StringValue_Utf8PassedThrough)
{
	std::string out;
	JsonWriter w(out);
	w.value("\xE4\xBD\xA0\xE5\xA5\xBD"); // UTF-8 "你好"

	EXPECT_EQ(out, "\"\xE4\xBD\xA0\xE5\xA5\xBD\"");
}

TEST(JsonWriterTest, KeyWithSpecialChars_Escaped)
{
	std::string out;
	JsonWriter w(out);
	w.beginObject();
	w.key("a\"b").value(1);
	w.endObject();

	EXPECT_EQ(out, R"({"a\"b":1})");
	expectParsable(out);
}

// ============ 嵌套 ============

TEST(JsonWriterTest, NestedObjectAndArray_ProducesValidJson)
{
	std::string out;
	JsonWriter w(out);

	w.beginObject();
	w.key("users").beginArray();
	w.beginObject();
	w.key("id").value(1);
	w.key("tags").beginArray().value("a").value("b").endArray();
	w.endObject();
	w.beginObject();
	w.key("id").value(2);
	w.key("tags").beginArray().endArray();
	w.endObject();
	w.endArray();
	w.endObject();

	EXPECT_EQ(out, R"({"users":[{"id":1,"tags":["a","b"]},{"id":2,"tags":[]}]})");
	expectParsable(out);
}

TEST(JsonWriterTest, ArrayOfArrays_BracketsBalanced)
{
	std::string out;
	JsonWriter w(out);
	w.beginArray();
	w.beginArray().value(1).endArray();
	w.beginArray().endArray();
	w.beginArray().beginArray().value(2).endArray().endArray();
	w.endArray();

	EXPECT_EQ(out, "[[1],[],[[2]]]");
	expectParsable(out);
}

TEST(JsonWriterTest, DeepNesting_UpToLimitWorks)
{
	constexpr int kDepth = 16;

	std::string out;
	JsonWriter w(out);
	for (int i = 0; i < kDepth; ++i)
	{
		w.beginArray();
	}
	for (int i = 0; i < kDepth; ++i)
	{
		w.endArray();
	}

	EXPECT_EQ(out, std::string(kDepth, '[') + std::string(kDepth, ']'));
	EXPECT_TRUE(w.isComplete());
	expectParsable(out);
}

// ============ 状态查询 ============

TEST(JsonWriterTest, IsComplete_ReflectsUnclosedState)
{
	std::string out;
	JsonWriter w(out);

	EXPECT_TRUE(w.isComplete());

	w.beginObject();
	EXPECT_FALSE(w.isComplete());

	w.key("a");
	EXPECT_FALSE(w.isComplete());

	w.value(1);
	w.endObject();
	EXPECT_TRUE(w.isComplete());
}

// ============ 误用保护（Debug 构建下 assert 直接挂，所以只在 NDEBUG 下验兜底行为）============

#ifdef NDEBUG

TEST(JsonWriterTest, Misuse_EndWithoutBegin_DoesNothing)
{
	std::string out;
	JsonWriter w(out);

	w.endObject();
	w.endArray();

	EXPECT_EQ(out, "");
	EXPECT_TRUE(w.isComplete());
}

TEST(JsonWriterTest, Misuse_EndArrayInsideObject_Ignored)
{
	std::string out;
	JsonWriter w(out);
	w.beginObject();
	w.key("a").value(1);
	w.endArray(); // 类型不配对
	w.endObject();

	EXPECT_EQ(out, R"({"a":1})");
	expectParsable(out);
}

TEST(JsonWriterTest, Misuse_KeyOutsideObject_Ignored)
{
	std::string out;
	JsonWriter w(out);
	w.key("a");
	w.value(1);

	EXPECT_EQ(out, "1");
}

TEST(JsonWriterTest, Misuse_ValueInObjectWithoutKey_Ignored)
{
	std::string out;
	JsonWriter w(out);
	w.beginObject();
	w.key("a").value(1);
	w.value(2); // 漏了 key
	w.endObject();

	EXPECT_EQ(out, R"({"a":1})");
	expectParsable(out);
}

TEST(JsonWriterTest, Misuse_KeyWithoutValue_PadsNull)
{
	std::string out;
	JsonWriter w(out);
	w.beginObject().key("a");
	w.endObject();

	EXPECT_EQ(out, R"({"a":null})");
	expectParsable(out);
}

TEST(JsonWriterTest, Misuse_TwoKeysInARow_PadsNull)
{
	std::string out;
	JsonWriter w(out);
	w.beginObject();
	w.key("a");
	w.key("b").value(2);
	w.endObject();

	EXPECT_EQ(out, R"({"a":null,"b":2})");
	expectParsable(out);
}

#endif // NDEBUG

// ============ HttpResponse 集成 ============

TEST(JsonWriterResponseTest, JsonWriter_WritesIntoResponseBody)
{
	HttpResponse res;
	{
		auto w = res.jsonWriter();
		w.beginObject();
		w.key("ok").value(true);
		w.endObject();
	}

	EXPECT_EQ(res.body(), R"({"ok":true})");
	expectParsable(res.body());
}

TEST(JsonWriterResponseTest, JsonWriter_SetsJsonContentType)
{
	HttpResponse res;
	{
		auto w = res.jsonWriter();
		w.beginObject().endObject();
	}

	// 跟 json()/jsonFrom() 那两条路保持一致，不额外带 charset
	HttpResponse ref;
	ref.setJsonBody(boost::json::value {1});
	EXPECT_EQ(res.header("Content-Type"), "application/json");
	EXPECT_EQ(res.header("Content-Type"), ref.header("Content-Type"));
}

TEST(JsonWriterResponseTest, JsonWriter_PreparePayloadContentLengthMatchesBody)
{
	HttpResponse res;
	{
		auto w = res.jsonWriter();
		w.beginArray().value(1).value(2).value(3).endArray();
	}
	res.native().preparePayload();

	EXPECT_EQ(res.header("Content-Length"), std::to_string(res.body().size()));
}

TEST(JsonWriterResponseTest, JsonWriter_OverwritesBodyPreparedBySetBody)
{
	// setBody 那步已经把 Content-Length 算成 1 了，writer 写完得能重新算
	HttpResponse res;
	res.setBody("x");
	ASSERT_EQ(res.header("Content-Length"), "1");

	{
		auto w = res.jsonWriter();
		w.beginObject();
		w.key("n").value(12345);
		w.endObject();
	}
	res.native().preparePayload();

	EXPECT_EQ(res.body(), R"({"n":12345})");
	EXPECT_EQ(res.header("Content-Length"), std::to_string(res.body().size()));
}

TEST(JsonWriterResponseTest, JsonWriter_RuntimeShapedArray_MatchesExpectedWire)
{
	// 运行时才知道有几条，这正是 writer 存在的理由：jsonFrom 那种编译期形状套不上
	constexpr int kItemCount = 2;
	const int ids[kItemCount] = {1, 2};

	HttpResponse res;
	{
		auto w = res.jsonWriter();
		w.beginObject();
		w.key("count").value(kItemCount);
		w.key("items").beginArray();
		for (int id : ids)
		{
			w.beginObject().key("id").value(id).endObject();
		}
		w.endArray();
		w.endObject();
		EXPECT_TRUE(w.isComplete());
	}
	res.native().preparePayload();

	EXPECT_EQ(res.body(), R"({"count":2,"items":[{"id":1},{"id":2}]})");
	EXPECT_EQ(res.header("Content-Length"), std::to_string(res.body().size()));
	expectParsable(res.body());
}
