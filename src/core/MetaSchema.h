//
// Created by LaoZu on 2026/9/12.
//
#pragma once
#include <boost/json.hpp>
#include <concepts>
#include <meta>
#include <ranges>
#include <variant>

namespace hical::schema {
    namespace json = boost::json;
    namespace V = std::ranges::views;

    // 类模板，用于后面的偏特化处理
    template <typename T>
    struct Schema {};

    // 快速模板变量
    template <typename T>
    inline constexpr Schema<T> kSchema;

    // 布尔类型
    template <>
    struct Schema<bool> {
        static void operator()(json::object &prop) {
            prop["type"] = "boolean";
        }
    };

    // 整数类型
    template <std::integral TInt>
    struct Schema<TInt> {
        static void operator()(json::object &prop) {
            prop["type"] = "integer";
            prop["format"] = M::display_string_of(^^TInt);
        }
    };

    // 浮点数类型
    template <std::floating_point TFloat>
    struct Schema<TFloat> {
        static void operator()(json::object &prop) {
            prop["type"] = "number";
            prop["format"] = M::display_string_of(^^TFloat);
        }
    };

    // 可选类型
    template <typename TOpt>
    requires json::is_optional_like<TOpt>::value
    struct Schema<TOpt> {
        static void operator()(json::object &prop) {
            using ValueType = TOpt::value_type;

            kSchema<ValueType>(prop);

            prop["nullable"] = true;
        }
    };

    // 字符串类型
    template <typename TStr>
    requires json::is_string_like<TStr>::value
    struct Schema<TStr> {
        static void operator()(json::object &prop) {
            prop["type"] = "string";
            prop["format"] = M::display_string_of(^^TStr);
        }
    };

    // 数组类型
    template <typename TArr>
    requires json::is_sequence_like<TArr>::value
        and (not json::is_string_like<TArr>::value)     // 除了 const char* 不是数组类型
        and (not json::is_optional_like<TArr>::value)   // cpp26 的 std::optional 提供了 begin() 和 end() 方法，所以被认为是数组类型
    struct Schema<TArr> {
        static void operator()(json::object &prop) {
            using ValueType = TArr::value_type;

            json::object items;
            kSchema<ValueType>(items);

            prop["type"] = "array";
            prop["items"] = std::move(items);
        }
    };

    // 枚举类型
    // https://swagger.org.cn/docs/specification/v3_0/data-models/enums/
    template <typename TEnum>
    requires (std::meta::is_enum_type(^^TEnum))
    struct Schema<TEnum> {
        static void operator()(json::object &prop) {
            json::array enumerators;
            template for (constexpr auto enumerator :
                std::define_static_array(M::enumerators_of(^^TEnum))
            ) {
                constexpr std::string_view name = M::identifier_of(enumerator);
                enumerators.emplace_back(name);
            }

            prop["type"] = "string";
            prop["enum"] = std::move(enumerators);
        }
    };

    // 联合体类型-类型安全的
    // https://swagger.org.cn/docs/specification/v3_0/data-models/oneof-anyof-allof-not/
    template <typename... VariantArgs>
    struct Schema<std::variant<VariantArgs...>> {
        static void operator()(json::object &prop) {
            json::array items;
            template for (constexpr int index : V::iota(std::size_t{}, sizeof...(VariantArgs))) {
                json::object item;
                kSchema<VariantArgs...[index]>(item);
                items.push_back(std::move(item));
            }

            prop["oneOf"] = std::move(items);
        }
    };

    // 联合体类型-类型不安全的，应当禁用
    template <typename TUnion>
    requires (M::is_union_type(^^TUnion))
    struct Schema<TUnion> {
        static void operator()(json::object &prop) {
            constexpr auto ctx = M::access_context::unprivileged();
            json::array items;
            template for (constexpr M::info memberInfo :
                std::define_static_array(M::nonstatic_data_members_of(^^TUnion, ctx))
            ) {
                json::object item;
                kSchema<typename [:M::type_of(memberInfo):]>(item);
                items.push_back(std::move(item));
            }

            prop["oneOf"] = std::move(items);
        }
    };
}