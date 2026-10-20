/**
 * @file test_json_writer.cpp
 * @brief JsonWriter 零 DOM 增量序列化测试
 */

#include "core/HttpResponse.h"
#include "core/JsonWriter.h"
#include <gtest/gtest.h>
#include <boost/json.hpp>
#include <charconv>
#include <cmath>
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

TEST(JsonWriterTest, DoubleExtremes_RoundTripToSameValue)
{
	// shortest 表示的要求是「读回来还是那个数」，所以这里不比对文本，直接回解比数值
	const double values[] = {
		std::numeric_limits<double>::max(),
		-std::numeric_limits<double>::max(),
		std::numeric_limits<double>::min(), // 最小正规数
		-std::numeric_limits<double>::min(),
		std::numeric_limits<double>::denorm_min(), // 最小次正规数，to_chars 只给一位有效数字也要能还原
		1.0 / 3.0,
		1e-300,
		1e300,
	};

	for (const double v : values)
	{
		std::string out;
		JsonWriter w(out);
		w.value(v);

		double back = 0;
		const auto res = std::from_chars(out.data(), out.data() + out.size(), back);
		ASSERT_EQ(res.ec, std::errc {}) << "回读失败，输出是: " << out;
		ASSERT_EQ(res.ptr, out.data() + out.size()) << "输出里有多余字符: " << out;
		EXPECT_EQ(back, v) << "最短表示回读后数值变了，输出是: " << out;
	}
}

TEST(JsonWriterTest, NegativeZero_KeepsSign)
{
	// -0.0 不是 0：to_chars 得保留负号，否则下游按符号区分正负零时会对不上
	std::string out;
	JsonWriter w(out);
	w.value(-0.0);

	double back = 1.0;
	const auto res = std::from_chars(out.data(), out.data() + out.size(), back);
	ASSERT_EQ(res.ec, std::errc {}) << "输出是: " << out;
	EXPECT_EQ(back, 0.0);
	EXPECT_TRUE(std::signbit(back)) << "负号丢了，输出是: " << out;
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

TEST(JsonWriterTest, StringValue_AllControlChars_UseExactEscapeForm)
{
	// 0x00-0x1F 全表过一遍：8/9/10/12/13 必须是缩写形式，其余必须是 \u00XX。
	// 这里把期望值单独抄一份，不引用 JsonEscape.h 里那张表——引用过来的话码表改错了测试也跟着错，
	// 等于没测。
	static constexpr std::string_view kExpected[32] = {
		R"(\u0000)", R"(\u0001)", R"(\u0002)", R"(\u0003)", R"(\u0004)", R"(\u0005)", R"(\u0006)", R"(\u0007)",
		R"(\b)",     R"(\t)",     R"(\n)",     R"(\u000b)", R"(\f)",     R"(\r)",     R"(\u000e)", R"(\u000f)",
		R"(\u0010)", R"(\u0011)", R"(\u0012)", R"(\u0013)", R"(\u0014)", R"(\u0015)", R"(\u0016)", R"(\u0017)",
		R"(\u0018)", R"(\u0019)", R"(\u001a)", R"(\u001b)", R"(\u001c)", R"(\u001d)", R"(\u001e)", R"(\u001f)",
	};

	for (int c = 0; c < 32; ++c)
	{
		const char raw = static_cast<char>(c);

		std::string out;
		JsonWriter w(out);
		w.value(std::string_view(&raw, 1));

		EXPECT_EQ(out, "\"" + std::string(kExpected[c]) + "\"") << "控制字符 0x" << std::hex << c;

		// 形状对了还不够，回解得还原成原来那个字节
		const auto parsed = boost::json::parse(out);
		const std::string_view decoded = parsed.as_string();
		ASSERT_EQ(decoded.size(), 1u) << "控制字符 0x" << std::hex << c;
		EXPECT_EQ(static_cast<unsigned char>(decoded[0]), static_cast<unsigned char>(c))
			<< "控制字符 0x" << std::hex << c;
	}
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

TEST(JsonWriterTest, EmptyStringValue_ProducesEmptyQuotes)
{
	// 空 string_view 的 data() 是 nullptr，转义函数里那段指针算术得能扛住
	std::string out;
	JsonWriter w(out);
	w.beginObject();
	w.key("").value(std::string_view {});
	w.endObject();

	EXPECT_EQ(out, R"({"":""})");
	expectParsable(out);
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

TEST(JsonWriterTest, Misuse_SecondTopLevelValue_Ignored)
{
	// 顶层只认一个值。第二次写要是放过去，产出会变成 "12"，谁也解不了
	std::string out;
	JsonWriter w(out);
	w.value(1);
	w.value(2);

	EXPECT_EQ(out, "1");
	EXPECT_TRUE(w.isComplete());
	expectParsable(out);
}

TEST(JsonWriterTest, Misuse_ValueAfterRootObject_Ignored)
{
	std::string out;
	JsonWriter w(out);
	w.beginObject().key("a").value(1).endObject();
	w.value(2);

	EXPECT_EQ(out, R"({"a":1})");
	EXPECT_TRUE(w.isComplete());
	expectParsable(out);
}

TEST(JsonWriterTest, Misuse_SecondRootContainer_Ignored)
{
	// 容器当顶层值也占掉那唯一一个名额，第二个容器照样丢掉
	std::string out;
	JsonWriter w(out);
	w.beginObject().endObject();
	w.beginArray().value(1).endArray();

	EXPECT_EQ(out, "{}");
	EXPECT_TRUE(w.isComplete());
	expectParsable(out);
}

TEST(JsonWriterTest, Misuse_NestingBeyondMaxDepth_ExtraBeginIgnored)
{
	// kMaxDepth 是 32 层。第 33 次 beginArray 该被丢掉，不写字节也不涨深度，
	// 这样后面 32 次 endArray 还能正常收口，不会把已经写出来的结构搅乱。
	constexpr int kMaxDepth = 32;

	std::string out;
	JsonWriter w(out);
	for (int i = 0; i < kMaxDepth; ++i)
	{
		w.beginArray();
	}
	w.beginArray(); // 越界那一层，应该被丢掉
	w.value(1);
	w.endArray(); // 收的是真实存在的第 32 层，不是刚才那个越界的

	for (int i = 0; i < kMaxDepth; ++i)
	{
		w.endArray();
	}

	EXPECT_EQ(out, std::string(kMaxDepth, '[') + "1" + std::string(kMaxDepth, ']'));
	EXPECT_TRUE(w.isComplete());
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

TEST(JsonWriterResponseTest, JsonWriter_LargeBodyOver512_WritesCompleteAndLengthMatches)
{
	// head + body 一旦超过 512 字节，发送端就不再合并到栈缓冲区，改走 head/body 分开的
	// scatter-gather（body 零拷贝引用）。这里确认 writer 写出的 body 没被截断，
	// Content-Length 跟实际长度一致，拼出来的整包结构也对得上。
	constexpr int kItemCount = 100; // 每条 30 字节上下，总量稳稳超过 512

	HttpResponse res;
	{
		auto w = res.jsonWriter();
		w.beginArray();
		for (int i = 0; i < kItemCount; ++i)
		{
			w.beginObject().key("id").value(i).key("name").value("user").endObject();
		}
		w.endArray();
		EXPECT_TRUE(w.isComplete());
	}

	ASSERT_GT(res.body().size(), 512u);

	res.native().preparePayload();
	EXPECT_EQ(res.header("Content-Length"), std::to_string(res.body().size()));

	// 结构完整：100 个元素，逐个核对首尾的值
	const auto parsed = boost::json::parse(res.body());
	const auto& items = parsed.as_array();
	ASSERT_EQ(items.size(), static_cast<size_t>(kItemCount));
	EXPECT_EQ(items.front().at("id").as_int64(), 0);
	EXPECT_EQ(items.front().at("name").as_string(), "user");
	EXPECT_EQ(items.back().at("id").as_int64(), kItemCount - 1);
	EXPECT_EQ(items.back().at("name").as_string(), "user");

	// 整包序列化后 body 原样落在末尾，没被 head 缓冲区的大小限制切掉
	const auto wire = res.native().serialize();
	ASSERT_GE(wire.size(), res.body().size());
	EXPECT_EQ(wire.size() - res.body().size(), wire.find(res.body()));
	EXPECT_NE(wire.find("Content-Length: " + std::to_string(res.body().size())), std::string::npos);
}
