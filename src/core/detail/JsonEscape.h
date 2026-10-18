/**
 * @file detail/JsonEscape.h
 * @brief JSON 字符串转义规则（框架内部），JsonWriter 和 CompileTimeJson 共用同一套
 */

#pragma once

#include <cstddef>
#include <string_view>

namespace hical::detail
{

	/**
	 * @brief JSON 转义表，索引为控制字符 0x00-0x1F
	 * 8/9/10/12/13 用缩写形式，其余用 \u00XX
	 */
	inline constexpr std::string_view kJsonEscapes[32] = {
		"\\u0000", "\\u0001", "\\u0002", "\\u0003", "\\u0004", "\\u0005", "\\u0006", "\\u0007",
		"\\b",     "\\t",     "\\n",     "\\u000b", "\\f",     "\\r",     "\\u000e", "\\u000f",
		"\\u0010", "\\u0011", "\\u0012", "\\u0013", "\\u0014", "\\u0015", "\\u0016", "\\u0017",
		"\\u0018", "\\u0019", "\\u001a", "\\u001b", "\\u001c", "\\u001d", "\\u001e", "\\u001f",
	};

	/**
	 * @brief 判断某个字节要不要转义
	 * 要转义的是 `"`、`\`、`/` 和 0x00-0x1F 的控制字符。
	 * 斜杠本身合法，但 boost::json::serialize 一直转义它，为了跟老路径的产出逐字节对得上，这里照样转。
	 */
	inline constexpr bool needsJsonEscape(unsigned char c) noexcept
	{
		return c < 0x20 || c == '"' || c == '\\' || c == '/';
	}

	/**
	 * @brief 把 string_view 按 JSON 字符串规则转义后追加到 sink
	 * sink 只要有 append(const char*, size_t) 和 append(std::string_view) 就够，
	 * FixedBuffer 和 std::string 都满足，所以两条序列化路径能共用这里。
	 * 正常字符成段拷贝，遇到要转义的字节才打断，不逐字符 append。
	 * @param sink 输出端（FixedBuffer 或 std::string）
	 * @param str 待转义的内容，不负责加两侧引号
	 */
	template <typename Sink>
	void appendJsonString(Sink& sink, std::string_view str)
	{
		const char* p = str.data();
		const char* end = p + str.size();
		const char* spanStart = p;

		while (p < end)
		{
			const unsigned char c = static_cast<unsigned char>(*p);
			if (!needsJsonEscape(c))
			{
				++p;
				continue;
			}

			// 先把前面攒的正常字符刷出去
			if (spanStart < p)
			{
				sink.append(spanStart, static_cast<size_t>(p - spanStart));
			}

			switch (c)
			{
				case '"':
					sink.append("\\\"", 2);
					break;
				case '\\':
					sink.append("\\\\", 2);
					break;
				case '/':
					sink.append("\\/", 2);
					break;
				default:
					sink.append(kJsonEscapes[c]);
					break;
			}

			spanStart = ++p;
		}

		// 收尾：把末尾那段正常字符刷出去
		if (spanStart < p)
		{
			sink.append(spanStart, static_cast<size_t>(p - spanStart));
		}
	}

} // namespace hical::detail
