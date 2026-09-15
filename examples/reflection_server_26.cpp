//
// Created by LaoZu on 2026/9/10.
//
#include <boost/json.hpp>
#include <chrono>
#include <iostream>
#include <print>
#include <ranges>
#include <spanstream>
#include <sstream>

#include "core/HttpServer.h"
#include "core/MetaJson.h"
#include "core/MetaRoutes.h"
namespace chrono = std::chrono;
namespace json = boost::json;
namespace M = std::meta;
namespace R = std::ranges;
namespace V = std::views;

// 自定义输出格式
template <typename T>
std::ostream &operator<<(std::ostream &os, std::vector<T> const& vec) {
    os << '[';
    for (auto const& item : vec) {
        os << item << ',';
    }
    os << ']';
    return os;
}
template <typename T>
std::ostream &operator<<(std::ostream &os, std::optional<T> const& opt) {
    if (opt.has_value()) {
        os << opt.value();
    } else {
        os << "null";
    }
    return os;
}
//-----------------------------------------自定义输出格式结束-----------------------------------------------------



// 示例用户自定义的注解处理函数
namespace user_define {
    // 自定义序列化注解，
    // 例如，将 int 类型的值增加 10
    struct UserDefinedDoChange {
        static auto applyAnnotationSerialize(const std::integral auto i, std::optional<json::value> &jv_opt) {
            jv_opt = json::value_from(i + 10);
        }
        template <std::integral TInt>
        static void applyAnnotationDeserialize(json::value const& v, std::optional<TInt> &i_opt) {
            i_opt = json::value_to<TInt>(v) -  10;
        }
    }constexpr do_change{};

    // 自定义视图注解，
    // 例如，打印序列化方向、类类型、成员类型、成员值、成员 JSON 值
    struct UserDefinedDoLog : hical::anno::ViewAnnotationHandler {
        template <typename Type>
        static consteval std::string_view printDisplay() { return std::define_static_string(std::meta::display_string_of(^^Type)); }

        template <hical::anno::IsViewType ViewType>
        static void impl(ViewType const& view) {
            std::clog << "is_serialize_direction: " << std::boolalpha << ViewType::kIsSerializeDirection
            << "  ViewType: " << printDisplay<ViewType>()
            << "  ClassType: " << printDisplay<typename ViewType::ClassType>()
            << "  MemberType: " << printDisplay<typename ViewType::MemberType>()
            << "  member_value: " << view.memberValue_
            << "  member_jv: " << view.memberJv_
            << std::endl;
        }
    }constexpr do_log{};
}
//-----------------------------------------示例用户自定义注解处理函数结束-----------------------------------------------------



// 自定义类型通用转换点，而非需要向结构体添加注解才能生效
// 例如，json 序列化过程中默认将 chrono::system_clock::time_point 类型的值转换为字符串格式
namespace boost::json {
    void tag_invoke(value_from_tag const& /*unused*/, value& jv, chrono::system_clock::time_point const& tp) {
        auto &str = jv.emplace_string();
        str = std::format("{:%F %T}", tp);
    }
    chrono::system_clock::time_point tag_invoke(value_to_tag<chrono::system_clock::time_point> const& /*unused*/, const value& jv) {
        std::ispanstream iss{ jv.as_string() };
        chrono::system_clock::time_point tp;
        iss >> chrono::parse("%F %T", tp);
        return tp;
    }
}

// 现在我希望对 chrono::system_clock::time_point 类型的值进行 ISO 8601 格式化处理，而不是默认的字符串格式,以及 JSON Schema 文档生成处理
struct AsIso8601 {
    static auto applyAnnotationSerialize(const chrono::system_clock::time_point tp, std::optional<json::value> &jv_opt) {
        jv_opt = json::value_from(tp, AsIso8601{});
    }
    static void applyAnnotationDeserialize(json::value const& v, std::optional<chrono::system_clock::time_point> &tp_opt) {
        tp_opt = json::value_to<chrono::system_clock::time_point>(v, AsIso8601{});
    }
    static void applyAnnotationSchema(json::object &prop, std::type_identity<chrono::system_clock::time_point> /*unused*/) {
        prop["format"] = "date-time ISO8601";
        prop["type"] = "string";
    }
}constexpr as_iso8601{};

// 针对 chrono::system_clock::time_point 类型的值进行 ISO 8601 格式化处理的通用处理点
// 推荐特定格式化的逻辑部分在 tag_invoke 中实现，而非在 applyAnnotationSerialize 中实现
// 这更具有通用性，使得用户可以直接调用 json::value_from 和 json::value_to 进行序列化和反序列化
namespace boost::json {
    void tag_invoke(value_from_tag const& /*unused*/, value& jv, chrono::system_clock::time_point const& tp, AsIso8601 const& /*unused*/) {
        const chrono::sys_seconds secondTp{ chrono::duration_cast<chrono::seconds>(tp.time_since_epoch()) };
        const std::chrono::zoned_time curTime{ std::chrono::current_zone(), secondTp };
        auto &str = jv.emplace_string();
        str = std::format("{:%FT%T%z}", curTime);
    }
    chrono::system_clock::time_point tag_invoke(value_to_tag<chrono::system_clock::time_point> const& /*unused*/, const value& jv, AsIso8601 const& /*unused*/) {
        std::ispanstream iss{ jv.as_string() };
        chrono::sys_seconds secondTp;
        iss >> chrono::parse("%FT%T%z", secondTp);
        return secondTp;
    }
}
// 针对 chrono::system_clock::time_point 类型的值进行 JSON Schema 文档生成处理
template <>
struct hical::schema::Schema<chrono::system_clock::time_point> {
    static void operator()(json::object &prop) {
        prop["format"] = "date-time";
        prop["type"] = "string";
    }
};
//-----------------------------------------示例通用转换点结束---------------------------------------------------------------



// 响应体包装器
namespace ResponseWrapper {
    struct Base {
        [[=hical::json_rename("code")]] int httpStatus_;
        [[=hical::json_rename("msg")]] std::string message_;
    };
    template <typename DataType>
    struct Data : Base {
        template <typename... Args>
        explicit Data(Args&&... args) : data_(std::forward<Args>(args)...){}
        [[=hical::json_rename("data")]] DataType data_;
    };
    template <typename DataType>
    struct DataRef : Base {
        explicit DataRef(DataType const& data) : data_(data) {}
        [[=hical::json_rename("data")]] DataType const& data_;
    };
}
//-----------------------------------------示例通过继承实现响应体包装器结束----------------------------------------------------



//用户自定义的结构体，用于测试
//在用户的结构体添加了字段蛇形映射为json小驼峰命名，同时添加了自定义的日志打印注解
struct [[=hical::json_snake_to_lowerCamel, =user_define::do_log]] Test {
    [[=hical::json_ignore]] bool boolean;

    // 因为定义了 std::integral 概念，所以会进行处理
    [[=hical::json_rename("test"), =user_define::do_change]]
    int id;
    // 因为定义了 std::integral 概念，所以会进行处理
    [[=user_define::do_change]]
    long long long_id;
    // 因为 user_define::do_change 未定义 std::string 相关重载函数，不会进行处理
    [[=user_define::do_change]] std::string test_name;

    // 示例 json_reset 重置原始 key 值
    [[=hical::json_ignore]]
    [[=hical::json_rename("do_raw")]]
    [[=hical::json_reset]]
    char raw_char;

    // 原始输出时间
    chrono::system_clock::time_point raw_tp = chrono::system_clock::now();
    // 格式化 ISO8601 输出时间
    [[=as_iso8601]] chrono::system_clock::time_point iso_tp = chrono::system_clock::now();

    // 示例可选字段和数组字段的嵌套处理
    std::optional<std::vector<int>> opt_vec_int;
}
const test{ .boolean = true, .id = 19, .long_id = 1009, .test_name = "has_str_test", .raw_char = 'r', .opt_vec_int = std::vector{1, 2, 3} };
// 测试数据引用的响应体包装器
const auto dataRef = [] {
    // 使用响应包装器
    ResponseWrapper::DataRef dataRef{ test };
    dataRef.httpStatus_ = 200;
    dataRef.message_ = "success";
    return dataRef;
}();
//-----------------------------------------示例在结构体上的应用结束-----------------------------------------------------



//api接口开始-----------------------------------------------------
struct ApiHandler {
    // route api
    [[=hical::anno::route("/api/route", "POST")]]
    static hical::HttpResponse postRoute(const hical::HttpRequest& /*unused*/) {
        return hical::HttpResponse::json(hical::meta::toJson(dataRef));
    }
    // 不添加 hical::route 注解，不会被注册为路由
    static hical::HttpResponse testRoute(const hical::HttpRequest& /*unused*/) {
        throw std::logic_error{ "testRoute" };
    }

};
struct ApiHandler2 {
    // 简化 get api
    [[=hical::anno::route.get("/simple/test")]]
    static hical::HttpResponse getTest(const hical::HttpRequest& /*unused*/) {
        return hical::HttpResponse::json(hical::meta::toJson(dataRef));
    }
    // 简化 patch api
    [[=hical::anno::route.patch("/simple/long/name/test")]]
    static hical::HttpResponse patchTest(const hical::HttpRequest& /*unused*/) {
        return hical::HttpResponse::json(hical::meta::toJson(dataRef));
    }
    [[=hical::anno::route.get("/schema")]]
    static hical::HttpResponse getSchema(const hical::HttpRequest& /*unused*/) {
        return hical::HttpResponse::json(hical::meta::jsonSchema<Test>());
    }
};
//api接口结束-----------------------------------------------------

int main () {
    // 序列化部分的json输出
    json::value jv = hical::meta::toJson(test);
    std::cout << std::endl << "json::value:" << std::endl << jv << std::endl << std::endl;
    // 反序列化部分的json输出
    Test testOut = hical::meta::fromJson<Test>(jv);
    std::cout << std::endl << "member::value:" << std::endl;
    template for (const auto& memberValue: testOut) {
        std::cout << memberValue << std::endl;
    }

    // 打印json schema
    std::cout << std::endl << "Schema:" << std::endl;
    std::cout << hical::meta::jsonSchema<Test>() << std::endl << std::endl;

    // 服务器部分
    hical::HttpServer server(8080);
    ApiHandler handler;
    ApiHandler2 handler2;
    hical::meta::registerRoutes(server.router(), handler);
    hical::meta::registerRoutes(server.router(), handler2);
    std::cout << std::endl << "describeHandlerRoutes:" << std::endl;
    template for (constexpr auto& info : std::define_static_array(hical::meta::describeHandlerRoutes({ ^^ApiHandler, ^^ApiHandler2 }))) {
        std::cout << info << std::endl;
    }
    server.start();
}