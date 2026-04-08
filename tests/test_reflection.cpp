#include "core/MetaJson.h"
#include "core/MetaRoutes.h"
#include "core/HttpRequest.h"
#include "core/HttpResponse.h"
#include "core/Router.h"
#include "test_helpers.h"
#include <gtest/gtest.h>
#include <string>
#include <vector>

using namespace hical;
using hical::test::runCoroutine;

// ============ 测试用 DTO 结构体 ============

struct SimpleDTO
{
    std::string name;
    int age;

    HICAL_JSON(SimpleDTO, name, age)
};

struct NestedAddress
{
    std::string city;
    std::string street;

    HICAL_JSON(NestedAddress, city, street)
};

struct UserWithAddress
{
    std::string name;
    int age;
    NestedAddress address;

    HICAL_JSON(UserWithAddress, name, age, address)
};

struct WithVector
{
    std::string name;
    std::vector<int> scores;

    HICAL_JSON(WithVector, name, scores)
};

struct WithDouble
{
    double value;
    bool active;

    HICAL_JSON(WithDouble, value, active)
};

// ============ JSON 序列化测试 ============

TEST(MetaJsonTest, SerializeSimple)
{
    SimpleDTO dto {"Alice", 30};
    auto json = meta::toJson(dto);

    EXPECT_EQ(json["name"].as_string(), "Alice");
    EXPECT_EQ(json["age"].as_int64(), 30);
}

TEST(MetaJsonTest, DeserializeSimple)
{
    boost::json::value json = boost::json::object {{"name", "Bob"}, {"age", 25}};
    auto dto = meta::fromJson<SimpleDTO>(json);

    EXPECT_EQ(dto.name, "Bob");
    EXPECT_EQ(dto.age, 25);
}

TEST(MetaJsonTest, RoundTrip)
{
    SimpleDTO original {"Charlie", 42};
    boost::json::value json = meta::toJson(original);
    auto restored = meta::fromJson<SimpleDTO>(json);

    EXPECT_EQ(restored.name, original.name);
    EXPECT_EQ(restored.age, original.age);
}

TEST(MetaJsonTest, NestedStruct)
{
    UserWithAddress user {"Dave", 35, {"Beijing", "Chang'an Street"}};
    auto json = meta::toJson(user);

    EXPECT_EQ(json["name"].as_string(), "Dave");
    EXPECT_EQ(json["age"].as_int64(), 35);

    ASSERT_TRUE(json["address"].is_object());
    auto& addr = json["address"].as_object();
    EXPECT_EQ(addr["city"].as_string(), "Beijing");
    EXPECT_EQ(addr["street"].as_string(), "Chang'an Street");

    // fromJson 接受 json::value，object 可以隐式转换
    boost::json::value jsonVal = json;
    auto restored = meta::fromJson<UserWithAddress>(jsonVal);
    EXPECT_EQ(restored.name, "Dave");
    EXPECT_EQ(restored.address.city, "Beijing");
}

TEST(MetaJsonTest, VectorField)
{
    WithVector obj {"Eve", {90, 85, 92}};
    auto json = meta::toJson(obj);

    auto& arr = json["scores"].as_array();
    EXPECT_EQ(arr.size(), 3);
    EXPECT_EQ(arr[0].as_int64(), 90);
    EXPECT_EQ(arr[1].as_int64(), 85);
    EXPECT_EQ(arr[2].as_int64(), 92);

    boost::json::value jsonVal = json;
    auto restored = meta::fromJson<WithVector>(jsonVal);
    EXPECT_EQ(restored.scores.size(), 3);
    EXPECT_EQ(restored.scores[0], 90);
}

TEST(MetaJsonTest, DoubleAndBool)
{
    WithDouble obj {3.14, true};
    auto json = meta::toJson(obj);

    EXPECT_DOUBLE_EQ(json["value"].as_double(), 3.14);
    EXPECT_EQ(json["active"].as_bool(), true);

    boost::json::value jsonVal = json;
    auto restored = meta::fromJson<WithDouble>(jsonVal);
    EXPECT_DOUBLE_EQ(restored.value, 3.14);
    EXPECT_EQ(restored.active, true);
}

TEST(MetaJsonTest, MissingFieldKeepsDefault)
{
    boost::json::value json = boost::json::object {{"name", "Frank"}};
    auto dto = meta::fromJson<SimpleDTO>(json);

    EXPECT_EQ(dto.name, "Frank");
    EXPECT_EQ(dto.age, 0); // 默认初始化
}

TEST(MetaJsonTest, ExtraFieldIgnored)
{
    boost::json::value json = boost::json::object {{"name", "Grace"}, {"age", 28}, {"extra", "ignored"}};
    auto dto = meta::fromJson<SimpleDTO>(json);

    EXPECT_EQ(dto.name, "Grace");
    EXPECT_EQ(dto.age, 28);
}

// ============ HttpRequest readJson 测试 ============

TEST(MetaJsonTest, HttpRequestReadJson)
{
    HttpRequest req;
    req.setBody(R"({"name":"Helen","age":22})");
    req.setHeader("Content-Type", "application/json");

    auto dto = req.readJson<SimpleDTO>();

    EXPECT_EQ(dto.name, "Helen");
    EXPECT_EQ(dto.age, 22);
}

// ============ 路由自动注册测试 ============

struct TestHandler
{
    HttpResponse listUsers(const HttpRequest&)
    {
        return HttpResponse::json({{"action", "list"}});
    }
    HICAL_HANDLER(Get, "/api/users", listUsers)

    HttpResponse getUser(const HttpRequest& req)
    {
        return HttpResponse::json({{"userId", req.param("id")}});
    }
    HICAL_HANDLER(Get, "/api/users/{id}", getUser)

    HttpResponse createUser(const HttpRequest& req)
    {
        auto dto = req.readJson<SimpleDTO>();
        return HttpResponse::json({{"created", dto.name}});
    }
    HICAL_HANDLER(Post, "/api/users", createUser)

    HICAL_ROUTES(TestHandler, listUsers, getUser, createUser)
};

TEST(MetaRoutesTest, RegisterRoutes)
{
    Router router;
    TestHandler handler;

    meta::registerRoutes(router, handler);

    // 应注册 3 个路由（2 静态 + 1 参数）
    EXPECT_EQ(router.routeCount(), 3);
}

TEST(MetaRoutesTest, DispatchGetRoute)
{
    AsioEventLoop loop;
    Router router;
    TestHandler handler;

    meta::registerRoutes(router, handler);

    HttpRequest req;
    req.setMethod(HttpMethod::hGet);
    req.setTarget("/api/users");

    auto result = runCoroutine(loop,
                               [&]()
                               {
                                   return router.dispatch(req);
                               });

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->statusCode(), HttpStatusCode::hOk);

    auto json = boost::json::parse(result->body());
    EXPECT_EQ(json.at("action").as_string(), "list");
}

TEST(MetaRoutesTest, DispatchParamRoute)
{
    AsioEventLoop loop;
    Router router;
    TestHandler handler;

    meta::registerRoutes(router, handler);

    HttpRequest req;
    req.setMethod(HttpMethod::hGet);
    req.setTarget("/api/users/42");

    auto result = runCoroutine(loop,
                               [&]()
                               {
                                   return router.dispatch(req);
                               });

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->statusCode(), HttpStatusCode::hOk);

    auto json = boost::json::parse(result->body());
    EXPECT_EQ(json.at("userId").as_string(), "42");
}

TEST(MetaRoutesTest, DispatchPostWithJsonBody)
{
    AsioEventLoop loop;
    Router router;
    TestHandler handler;

    meta::registerRoutes(router, handler);

    HttpRequest req;
    req.setMethod(HttpMethod::hPost);
    req.setTarget("/api/users");
    req.setBody(R"({"name":"Ivan","age":30})");

    auto result = runCoroutine(loop,
                               [&]()
                               {
                                   return router.dispatch(req);
                               });

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->statusCode(), HttpStatusCode::hOk);

    auto json = boost::json::parse(result->body());
    EXPECT_EQ(json.at("created").as_string(), "Ivan");
}

TEST(MetaRoutesTest, RouteNotFoundReturns404)
{
    AsioEventLoop loop;
    Router router;
    TestHandler handler;

    meta::registerRoutes(router, handler);

    HttpRequest req;
    req.setMethod(HttpMethod::hDelete);
    req.setTarget("/api/users");

    auto result = runCoroutine(loop,
                               [&]()
                               {
                                   return router.dispatch(req);
                               });

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->statusCode(), HttpStatusCode::hNotFound);
}

// ============ HICAL_HANDLER 宏生成 RouteInfo 测试 ============

TEST(MetaRoutesTest, RouteInfoGeneration)
{
    EXPECT_EQ(TestHandler::hicalRouteInfo_listUsers.method, HttpMethod::hGet);
    EXPECT_EQ(TestHandler::hicalRouteInfo_listUsers.path, "/api/users");
    EXPECT_EQ(TestHandler::hicalRouteInfo_listUsers.handlerName, "listUsers");

    EXPECT_EQ(TestHandler::hicalRouteInfo_getUser.method, HttpMethod::hGet);
    EXPECT_EQ(TestHandler::hicalRouteInfo_getUser.path, "/api/users/{id}");

    EXPECT_EQ(TestHandler::hicalRouteInfo_createUser.method, HttpMethod::hPost);
    EXPECT_EQ(TestHandler::hicalRouteInfo_createUser.path, "/api/users");
}

// ============ HasJsonFields / HasRouteTable 编译期检测 ============

TEST(MetaTraitsTest, HasJsonFieldsDetection)
{
    EXPECT_TRUE(meta::HasJsonFields<SimpleDTO>::value);
    EXPECT_TRUE(meta::HasJsonFields<NestedAddress>::value);
    EXPECT_FALSE(meta::HasJsonFields<int>::value);
    EXPECT_FALSE(meta::HasJsonFields<std::string>::value);
}

TEST(MetaTraitsTest, HasRouteTableDetection)
{
    EXPECT_TRUE(meta::HasRouteTable<TestHandler>::value);
    EXPECT_FALSE(meta::HasRouteTable<SimpleDTO>::value);
}
