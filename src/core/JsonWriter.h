/**
 * @file JsonWriter.h
 * @brief 零 DOM 的增量 JSON writer，边遍历边把 wire 字节拼到外部 std::string 上
 * 对标 Glaze 的动态用法，但只依赖标准库：不建 boost::json::object，也就没有建树 +
 * serialize 那两遍开销。标量直接 to_chars 写出去，不产生中间 std::string。
 * 用法：
 *   std::string body;
 *   JsonWriter w(body);
 *   w.beginObject();
 *   w.key("id").value(42);
 *   w.key("tags").beginArray().value("a").value("b").endArray();
 *   w.endObject();
 * 规矩：
 *   - 对象里必须先 key() 再 value()，数组里直接 value()；
 *   - 空对象自然产出 {}，空数组产出 []，不用特殊判断；
 *   - 误用（end 不配对、对象里漏 key）Debug 下 assert 挂掉，Release 下当没调或者
 *     补个 null，保证不会踩内存、不会把已写出的内容搅乱；
 *   - 单线程使用，别把同一个 writer 拿到多个线程上写。
 */

#pragma once

#include "detail/JsonEscape.h"
#include <array>
#include <cassert>
#include <charconv>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <string>
#include <string_view>
#include <type_traits>

namespace hical
{

	/**
	 * @brief 增量 JSON writer，只往外部传入的 std::string 上追加
	 * 对象/数组的嵌套状态记在一个固定大小的小数组里（kMaxDepth 层），不碰堆也不碰 PMR，
	 * 所以 writer 本身可以随便放栈上。超过 kMaxDepth 层属于设计外用法，Debug 下 assert。
	 */
	class JsonWriter
	{
	public:
		/**
		 * @brief 绑定输出目标
		 * @param out 输出字符串，writer 只追加不拥有，调用方保证它活得比 writer 久
		 */
		explicit JsonWriter(std::string& out) noexcept : out_(&out)
		{
		}

		JsonWriter(const JsonWriter&) = delete;
		JsonWriter& operator=(const JsonWriter&) = delete;

		JsonWriter& beginObject()
		{
			return beginContainer(true);
		}

		JsonWriter& endObject()
		{
			return endContainer(true);
		}

		JsonWriter& beginArray()
		{
			return beginContainer(false);
		}

		JsonWriter& endArray()
		{
			return endContainer(false);
		}

		/**
		 * @brief 写对象成员名，后面必须紧跟一个 value
		 * @param name 成员名，内部会做转义，只在本次调用期间用
		 */
		JsonWriter& key(std::string_view name)
		{
			if (depth_ == 0 || !frames_[depth_ - 1].isObject)
			{
				assert(false && "JsonWriter: key() 只能在对象里调用");
				return *this;
			}

			if (pendingKey_)
			{
				// 上一个 key 忘了给值，补个 null，至少输出还是合法 JSON
				assert(false && "JsonWriter: 上一个 key() 没有跟 value()");
				out_->append("null", 4);
				pendingKey_ = false;
			}

			Frame& frame = frames_[depth_ - 1];
			if (!frame.empty)
			{
				out_->push_back(',');
			}
			frame.empty = false;

			out_->push_back('"');
			detail::appendJsonString(*out_, name);
			out_->push_back('"');
			out_->push_back(':');
			pendingKey_ = true;
			return *this;
		}

		JsonWriter& value(std::nullptr_t)
		{
			if (!beforeValue())
			{
				return *this;
			}
			out_->append("null", 4);
			return *this;
		}

		/**
		 * @brief 写字符串值，内部转义并加引号
		 * @param str 值内容，只在本次调用期间用，不持有
		 */
		JsonWriter& value(std::string_view str)
		{
			if (!beforeValue())
			{
				return *this;
			}
			out_->push_back('"');
			detail::appendJsonString(*out_, str);
			out_->push_back('"');
			return *this;
		}

		/**
		 * @brief 写整数/布尔值
		 * 窄整型统一升到 long long / unsigned long long 再格式化，避免给每种宽度都开一份模板实例。
		 */
		template <std::integral T>
		JsonWriter& value(T val)
		{
			if (!beforeValue())
			{
				return *this;
			}

			if constexpr (std::same_as<T, bool>)
			{
				if (val)
				{
					out_->append("true", 4);
				}
				else
				{
					out_->append("false", 5);
				}
			}
			else if constexpr (std::is_signed_v<T>)
			{
				appendInteger(static_cast<long long>(val));
			}
			else
			{
				appendInteger(static_cast<unsigned long long>(val));
			}
			return *this;
		}

		template <std::floating_point T>
		JsonWriter& value(T val)
		{
			if (!beforeValue())
			{
				return *this;
			}
			appendDouble(static_cast<double>(val));
			return *this;
		}

		/**
		 * @brief 所有容器都闭合了没有，写响应前用它兜底检查
		 */
		[[nodiscard]] bool isComplete() const noexcept
		{
			return depth_ == 0 && !pendingKey_;
		}

	private:
		// JSON 嵌套深度上限，正常接口响应远到不了；超了算调用方写错
		static constexpr size_t kMaxDepth = 32;
		// 数字格式化用的栈缓冲：double 最短表示最多 24 字节左右，32 够用
		static constexpr size_t kNumberBufSize = 32;

		struct Frame
		{
			bool isObject; // 这个容器是对象还是数组
			bool empty;    // 里面还没写过成员/元素
		};

		JsonWriter& beginContainer(bool isObject)
		{
			if (depth_ >= kMaxDepth)
			{
				assert(false && "JsonWriter: 嵌套层数超过 kMaxDepth");
				return *this;
			}
			if (!beforeValue())
			{
				return *this;
			}

			out_->push_back(isObject ? '{' : '[');
			frames_[depth_++] = Frame {isObject, true};
			return *this;
		}

		JsonWriter& endContainer(bool isObject)
		{
			if (depth_ == 0 || frames_[depth_ - 1].isObject != isObject)
			{
				assert(false && "JsonWriter: endObject/endArray 与 begin 不配对");
				return *this;
			}

			if (isObject && pendingKey_)
			{
				// key 了没给 value，补个 null，别让输出缺一块
				assert(false && "JsonWriter: 上一个 key() 没有跟 value()");
				out_->append("null", 4);
				pendingKey_ = false;
			}

			out_->push_back(isObject ? '}' : ']');
			--depth_;
			return *this;
		}

		/**
		 * @brief 写一个值之前调，负责补逗号、翻容器状态
		 * @return false 表示当前是误用（对象里没 key 就写值），调用方应直接放弃这次写
		 */
		bool beforeValue()
		{
			if (pendingKey_)
			{
				// 刚写完 "key":，值直接接上去，不要逗号
				pendingKey_ = false;
				return true;
			}

			if (depth_ == 0)
			{
				// 顶层单值，没有分隔符概念
				return true;
			}

			Frame& frame = frames_[depth_ - 1];
			if (frame.isObject)
			{
				assert(false && "JsonWriter: 对象里必须先 key() 再 value()");
				return false;
			}

			if (!frame.empty)
			{
				out_->push_back(',');
			}
			frame.empty = false;
			return true;
		}

		template <typename T>
		void appendInteger(T val)
		{
			char buf[kNumberBufSize];
			const auto res = std::to_chars(buf, buf + kNumberBufSize, val);
			assert(res.ec == std::errc {} && "JsonWriter: 整数格式化缓冲区不够");
			out_->append(buf, static_cast<size_t>(res.ptr - buf));
		}

		void appendDouble(double val)
		{
			// JSON 没有 nan/inf 字面量，跟 boost::json 一样落成 null
			if (!std::isfinite(val))
			{
				out_->append("null", 4);
				return;
			}

			char buf[kNumberBufSize];
			const auto res = std::to_chars(buf, buf + kNumberBufSize, val, std::chars_format::general);
			assert(res.ec == std::errc {} && "JsonWriter: 浮点格式化缓冲区不够");
			out_->append(buf, static_cast<size_t>(res.ptr - buf));
		}

		std::string* out_;
		std::array<Frame, kMaxDepth> frames_ {};
		size_t depth_ {0};
		bool pendingKey_ {false};
	};

} // namespace hical
