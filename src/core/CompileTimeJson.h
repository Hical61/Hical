/**
 * @file CompileTimeJson.h
 * @brief 编译期 JSON 直序列化，绕开 boost::json::object 直接在栈缓冲上拼接 wire bytes
 * 核心思路：利用 HICAL_JSON 提供的编译期字段信息，逐个字段往 FixedBuffer<512> 上拼
 * `{"key":value,...}`，省掉 boost::json::object 的创建和 lookup 开销。
 * 小 DTO（<512B wire）零堆分配，大 DTO 自动溢出到堆。
 * 用法：
 *   #include "core/CompileTimeJson.h"
 *   std::string json = hical::meta::compileTimeToJson(myDto);
 * 与 toJson() 的产出在语义上等价，区别在于跳过 boost::json 中间层。
 * 字符串转义规则放在 detail/JsonEscape.h，跟 JsonWriter 共用同一套。
 */

#pragma once

#include "FixedBuffer.h"
#include "MetaJson.h"
#include "detail/JsonEscape.h"
#include <string>

namespace hical::meta
{

	// 前向声明（必须在 meta 命名空间），嵌套 DTO 递归时需要
	template <typename T>
	std::string compileTimeToJson(const T& obj);

	namespace detail
	{

		/**
		 * @brief 将单个 C++ 值序列化为 JSON 片段写入缓冲区
		 * 支持类型：int / int64_t / double / bool / std::string / std::vector<T> / 嵌套 DTO
		 */
		template <size_t N, typename T>
		void valueToJsonBuffer(FixedBuffer<N>& buf, const T& val)
		{
			if constexpr (std::is_same_v<T, std::string>)
			{
				buf << '"';
				hical::detail::appendJsonString(buf, val);
				buf << '"';
			}
			else if constexpr (std::is_same_v<T, bool>)
			{
				buf << val;
			}
			else if constexpr (std::is_integral_v<T>)
			{
				if constexpr (std::is_unsigned_v<T>)
				{
					buf << static_cast<unsigned long long>(val);
				}
				else
				{
					buf << static_cast<long long>(val);
				}
			}
			else if constexpr (std::is_floating_point_v<T>)
			{
				buf << static_cast<double>(val);
			}
			else if constexpr (IsVector<T>::value)
			{
				buf << '[';
				bool first = true;
				for (const auto& item : val)
				{
					if (!first)
					{
						buf << ',';
					}
					first = false;
					// std::vector<bool> 迭代器返回代理类不是 bool&，需要显式转换
					if constexpr (std::is_same_v<typename T::value_type, bool>)
					{
						valueToJsonBuffer(buf, static_cast<bool>(item));
					}
					else
					{
						valueToJsonBuffer(buf, item);
					}
				}
				buf << ']';
			}
			else if constexpr (HasJsonFields<T>::value)
			{
				// 嵌套 DTO：递归序列化为完整 JSON 对象，再拼入父缓冲区
				auto innerJson = meta::compileTimeToJson(val);
				buf.append(innerJson);
			}
			else
			{
				static_assert(sizeof(T) == 0, "Unsupported type for compile-time JSON serialization");
			}
		}

	} // namespace detail

	/**
	 * @brief 编译期 JSON 直序列化入口
	 * 从 HICAL_JSON 标注的编译期字段信息出发，逐个字段拼 `"key":value`，用
	 * FixedBuffer<512> 做栈缓冲。字段不超过 512B 时不触发堆分配。
	 * @tparam T 已标注 HICAL_JSON 的结构体类型
	 * @param obj 要序列化的对象
	 * @return JSON 字符串，语义等价于 boost::json::serialize(toJson(obj))
	 */
	template <typename T>
	std::string compileTimeToJson(const T& obj)
	{
		static_assert(HasJsonFields<T>::value, "compileTimeToJson requires HICAL_JSON() annotation on the type");

		FixedBuffer<512> buf;
		buf << '{';

		const auto& fields = T::hicalJsonFields();
		constexpr auto fieldCount = std::tuple_size_v<std::remove_cvref_t<decltype(fields)>>;

		bool first = true;
		auto emitField = [&](const auto& field)
		{
			if (!field.ignored)
			{
				if (!first)
				{
					buf << ',';
				}
				first = false;

				buf << '"';
				buf.append(field.name);
				buf << '"' << ':';
				detail::valueToJsonBuffer(buf, obj.*field.pointer);
			}
		};

		[&]<size_t... I>(std::index_sequence<I...>)
		{
			(emitField(std::get<I>(fields)), ...);
		}(std::make_index_sequence<fieldCount> {});

		buf << '}';
		return std::string(buf.view());
	}

} // namespace hical::meta
