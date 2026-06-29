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

TEST(MetaRoutesTest, UnregisteredMethodReturns405)
{
	AsioEventLoop loop;
	Router router;
	TestHandler handler;

	meta::registerRoutes(router, handler);

	// /api/users 存在 GET 和 POST，DELETE 应返回 405
	HttpRequest req;
	req.setMethod(HttpMethod::hDelete);
	req.setTarget("/api/users");

	auto result = runCoroutine(loop,
							   [&]()
							   {
								   return router.dispatch(req);
							   });

	ASSERT_TRUE(result.has_value());
	EXPECT_EQ(result->statusCode(), HttpStatusCode::hMethodNotAllowed);
}

TEST(MetaRoutesTest, NonExistentPathReturns404)
{
	AsioEventLoop loop;
	Router router;
	TestHandler handler;

	meta::registerRoutes(router, handler);

	// 完全不存在的路径应返回 404
	HttpRequest req;
	req.setMethod(HttpMethod::hGet);
	req.setTarget("/api/nonexistent");

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

// ============ 字段别名测试 ============

struct AliasDTO
{
	std::string userName;
	int userAge;
	std::string email;

	HICAL_JSON(AliasDTO, ALIAS(userName, "user_name"), ALIAS(userAge, "user_age"), email)
};

TEST(MetaJsonTest, AliasSerialize)
{
	AliasDTO dto {"Alice", 30, "alice@test.com"};
	auto json = meta::toJson(dto);

	// 别名字段使用自定义 key
	EXPECT_EQ(json["user_name"].as_string(), "Alice");
	EXPECT_EQ(json["user_age"].as_int64(), 30);
	// 裸字段使用原名
	EXPECT_EQ(json["email"].as_string(), "alice@test.com");
	// 原名不应出现
	EXPECT_FALSE(json.contains("userName"));
	EXPECT_FALSE(json.contains("userAge"));
}

TEST(MetaJsonTest, AliasDeserialize)
{
	boost::json::value json = boost::json::object {{"user_name", "Bob"}, {"user_age", 25}, {"email", "bob@test.com"}};
	auto dto = meta::fromJson<AliasDTO>(json);

	EXPECT_EQ(dto.userName, "Bob");
	EXPECT_EQ(dto.userAge, 25);
	EXPECT_EQ(dto.email, "bob@test.com");
}

TEST(MetaJsonTest, AliasRoundTrip)
{
	AliasDTO original {"Charlie", 42, "c@test.com"};
	boost::json::value json = meta::toJson(original);
	auto restored = meta::fromJson<AliasDTO>(json);

	EXPECT_EQ(restored.userName, original.userName);
	EXPECT_EQ(restored.userAge, original.userAge);
	EXPECT_EQ(restored.email, original.email);
}

// ============ 必需字段测试 ============

struct RequiredDTO
{
	std::string id;
	std::string name;
	int age;
	std::string bio;

	HICAL_JSON(RequiredDTO, REQUIRED(id), REQUIRED(name), age, bio)
};

TEST(MetaJsonTest, RequiredFieldsPresent)
{
	boost::json::value json = boost::json::object {{"id", "123"}, {"name", "Dave"}, {"age", 35}, {"bio", "dev"}};
	auto dto = meta::fromJson<RequiredDTO>(json);

	EXPECT_EQ(dto.id, "123");
	EXPECT_EQ(dto.name, "Dave");
	EXPECT_EQ(dto.age, 35);
	EXPECT_EQ(dto.bio, "dev");
}

TEST(MetaJsonTest, RequiredFieldMissing)
{
	// id 缺失应抛异常
	boost::json::value json = boost::json::object {{"name", "Eve"}, {"age", 28}};
	EXPECT_THROW(
		{
			try
			{
				meta::fromJson<RequiredDTO>(json);
			}
			catch (const std::runtime_error& e)
			{
				EXPECT_NE(std::string(e.what()).find("id"), std::string::npos);
				throw;
			}
		},
		std::runtime_error);
}

TEST(MetaJsonTest, OptionalFieldMissing)
{
	// age 和 bio 可选，缺失保留默认值
	boost::json::value json = boost::json::object {{"id", "456"}, {"name", "Frank"}};
	auto dto = meta::fromJson<RequiredDTO>(json);

	EXPECT_EQ(dto.id, "456");
	EXPECT_EQ(dto.name, "Frank");
	EXPECT_EQ(dto.age, 0);
	EXPECT_EQ(dto.bio, "");
}

// ============ 必需 + 别名组合测试 ============

struct RequiredAliasDTO
{
	std::string userId;
	std::string userName;
	int age;

	HICAL_JSON(RequiredAliasDTO, REQUIRED_ALIAS(userId, "user_id"), REQUIRED_ALIAS(userName, "user_name"), age)
};

TEST(MetaJsonTest, RequiredAliasPresent)
{
	boost::json::value json = boost::json::object {{"user_id", "789"}, {"user_name", "Grace"}, {"age", 22}};
	auto dto = meta::fromJson<RequiredAliasDTO>(json);

	EXPECT_EQ(dto.userId, "789");
	EXPECT_EQ(dto.userName, "Grace");
	EXPECT_EQ(dto.age, 22);
}

TEST(MetaJsonTest, RequiredAliasMissing)
{
	// user_id 缺失应抛异常
	boost::json::value json = boost::json::object {{"user_name", "Helen"}};
	EXPECT_THROW(meta::fromJson<RequiredAliasDTO>(json), std::runtime_error);
}

TEST(MetaJsonTest, RequiredAliasSerialize)
{
	RequiredAliasDTO dto {"abc", "Ivan", 30};
	auto json = meta::toJson(dto);

	EXPECT_EQ(json["user_id"].as_string(), "abc");
	EXPECT_EQ(json["user_name"].as_string(), "Ivan");
	EXPECT_EQ(json["age"].as_int64(), 30);
	EXPECT_FALSE(json.contains("userId"));
}

// ============ 忽略字段测试 ============

struct IgnoreDTO
{
	std::string name;
	std::string email;
	std::string passwordHash;
	std::string internalToken;

	HICAL_JSON(IgnoreDTO, name, email, HICAL_IGNORE(passwordHash), HICAL_IGNORE(internalToken))
};

TEST(MetaJsonTest, IgnoreSerialize)
{
	IgnoreDTO dto {"Jack", "j@test.com", "hash123", "token456"};
	auto json = meta::toJson(dto);

	// 只输出非忽略字段
	EXPECT_EQ(json.size(), 2);
	EXPECT_EQ(json["name"].as_string(), "Jack");
	EXPECT_EQ(json["email"].as_string(), "j@test.com");
	EXPECT_FALSE(json.contains("passwordHash"));
	EXPECT_FALSE(json.contains("internalToken"));
}

TEST(MetaJsonTest, IgnoreDeserialize)
{
	// 即使 JSON 中有被忽略字段的 key，也不会读取
	boost::json::value json = boost::json::object {{"name", "Kate"},
												   {"email", "k@test.com"},
												   {"passwordHash", "should_be_ignored"},
												   {"internalToken", "also_ignored"}};
	auto dto = meta::fromJson<IgnoreDTO>(json);

	EXPECT_EQ(dto.name, "Kate");
	EXPECT_EQ(dto.email, "k@test.com");
	EXPECT_EQ(dto.passwordHash, "");  // 保持默认值
	EXPECT_EQ(dto.internalToken, ""); // 保持默认值
}

// ============ 混合装饰器测试 ============

struct MixedDTO
{
	std::string requestId;
	int statusCode;
	std::string message;
	std::string debugInfo;
	std::string traceId;

	HICAL_JSON(MixedDTO,
			   REQUIRED_ALIAS(requestId, "request_id"),
			   REQUIRED(statusCode),
			   ALIAS(message, "status_message"),
			   debugInfo,
			   HICAL_IGNORE(traceId))
};

TEST(MetaJsonTest, MixedSerialize)
{
	MixedDTO dto {"req-001", 200, "OK", "debug", "trace-abc"};
	auto json = meta::toJson(dto);

	EXPECT_EQ(json["request_id"].as_string(), "req-001");
	EXPECT_EQ(json["statusCode"].as_int64(), 200);
	EXPECT_EQ(json["status_message"].as_string(), "OK");
	EXPECT_EQ(json["debugInfo"].as_string(), "debug");
	EXPECT_FALSE(json.contains("traceId")); // ignored
	EXPECT_EQ(json.size(), 4);
}

TEST(MetaJsonTest, MixedDeserialize)
{
	boost::json::value json = boost::json::object {{"request_id", "req-002"},
												   {"statusCode", 404},
												   {"status_message", "Not Found"},
												   {"debugInfo", "info"}};
	auto dto = meta::fromJson<MixedDTO>(json);

	EXPECT_EQ(dto.requestId, "req-002");
	EXPECT_EQ(dto.statusCode, 404);
	EXPECT_EQ(dto.message, "Not Found");
	EXPECT_EQ(dto.debugInfo, "info");
	EXPECT_EQ(dto.traceId, ""); // ignored, 保持默认
}

TEST(MetaJsonTest, MixedRequiredMissing)
{
	// request_id 缺失应抛异常
	boost::json::value json = boost::json::object {{"statusCode", 500}};
	EXPECT_THROW(meta::fromJson<MixedDTO>(json), std::runtime_error);
}

// ============ 大字段数测试（突破旧 16 字段限制）============

struct LargeDTO
{
	int f1, f2, f3, f4, f5, f6, f7, f8, f9, f10;
	int f11, f12, f13, f14, f15, f16, f17, f18, f19, f20;

	HICAL_JSON(LargeDTO, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17, f18, f19, f20)
};

TEST(MetaJsonTest, LargeDTOSerialize)
{
	LargeDTO dto {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20};
	auto json = meta::toJson(dto);

	EXPECT_EQ(json.size(), 20);
	EXPECT_EQ(json["f1"].as_int64(), 1);
	EXPECT_EQ(json["f17"].as_int64(), 17);
	EXPECT_EQ(json["f20"].as_int64(), 20);
}

TEST(MetaJsonTest, LargeDTORoundTrip)
{
	LargeDTO original {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20};
	boost::json::value json = meta::toJson(original);
	auto restored = meta::fromJson<LargeDTO>(json);

	EXPECT_EQ(restored.f1, 1);
	EXPECT_EQ(restored.f10, 10);
	EXPECT_EQ(restored.f17, 17);
	EXPECT_EQ(restored.f20, 20);
}

// ============ HasJsonFields 对新类型的检测 ============

TEST(MetaTraitsTest, HasJsonFieldsNewTypes)
{
	EXPECT_TRUE(meta::HasJsonFields<AliasDTO>::value);
	EXPECT_TRUE(meta::HasJsonFields<RequiredDTO>::value);
	EXPECT_TRUE(meta::HasJsonFields<IgnoreDTO>::value);
	EXPECT_TRUE(meta::HasJsonFields<MixedDTO>::value);
	EXPECT_TRUE(meta::HasJsonFields<LargeDTO>::value);
}

// ============ 向后兼容性验证 ============

TEST(MetaJsonTest, BackwardCompatibility)
{
	// SimpleDTO 使用原始 HICAL_JSON(T, f1, f2) 语法，必须仍然工作
	SimpleDTO dto {"Zara", 99};
	auto json = meta::toJson(dto);
	EXPECT_EQ(json["name"].as_string(), "Zara");
	EXPECT_EQ(json["age"].as_int64(), 99);

	// 缺失字段仍保留默认值（不抛异常）
	boost::json::value partial = boost::json::object {{"name", "Yuki"}};
	auto restored = meta::fromJson<SimpleDTO>(partial);
	EXPECT_EQ(restored.name, "Yuki");
	EXPECT_EQ(restored.age, 0);
}

// ============ uint64_t 大整数 round-trip 测试 ============

struct Uint64DTO
{
	uint64_t snowflakeId;
	uint64_t timestamp;
	int32_t status;

	HICAL_JSON(Uint64DTO, snowflakeId, timestamp, status)
};

TEST(MetaJsonTest, Uint64Serialize)
{
	// 超出 int64_t 范围的值
	Uint64DTO dto {18446744073709551615ULL, 9223372036854775808ULL, -1};
	auto json = meta::toJson(dto);

	EXPECT_EQ(json["snowflakeId"].as_uint64(), 18446744073709551615ULL);
	EXPECT_EQ(json["timestamp"].as_uint64(), 9223372036854775808ULL);
	EXPECT_EQ(json["status"].as_int64(), -1);
}

TEST(MetaJsonTest, Uint64Deserialize)
{
	boost::json::value json = boost::json::object {{"snowflakeId", 18446744073709551615ULL},
												   {"timestamp", 9223372036854775808ULL},
												   {"status", -1}};
	auto dto = meta::fromJson<Uint64DTO>(json);

	EXPECT_EQ(dto.snowflakeId, 18446744073709551615ULL);
	EXPECT_EQ(dto.timestamp, 9223372036854775808ULL);
	EXPECT_EQ(dto.status, -1);
}

TEST(MetaJsonTest, Uint64RoundTrip)
{
	Uint64DTO original {18446744073709551615ULL, 9223372036854775808ULL, 42};
	boost::json::value json = meta::toJson(original);
	auto restored = meta::fromJson<Uint64DTO>(json);

	EXPECT_EQ(restored.snowflakeId, original.snowflakeId);
	EXPECT_EQ(restored.timestamp, original.timestamp);
	EXPECT_EQ(restored.status, original.status);
}

// ============ DTO 校验测试 ============

struct MinMaxDTO
{
	int age;
	double score;
	int16_t level;

	HICAL_JSON(MinMaxDTO, MIN(age, 0), MAX(age, 200), MIN(score, 0.0), MAX(score, 100.0), MIN(level, 1), MAX(level, 99))
};

struct NotEmptyDTO
{
	std::string name;
	std::string email;

	HICAL_JSON(NotEmptyDTO, NOT_EMPTY(name), NOT_EMPTY(email))
};

struct PatternDTO
{
	std::string email;
	std::string phone;

	HICAL_JSON(PatternDTO,
			   PATTERN(email, R"(^[a-zA-Z0-9._%+-]+@[a-zA-Z0-9.-]+\.[a-zA-Z]{2,}$)"),
			   PATTERN(phone, R"(^\d{11}$)"))
};

struct LengthDTO
{
	std::string username;
	std::string bio;

	HICAL_JSON(LengthDTO, LENGTH(username, 3, 20), LENGTH(bio, 0, 500))
};

struct AllValidationsDTO
{
	std::string username;
	std::string email;
	int age;
	std::string bio;

	HICAL_JSON(AllValidationsDTO,
			   NOT_EMPTY(username),
			   PATTERN(email, R"(^[a-zA-Z0-9._%+-]+@[a-zA-Z0-9.-]+\.[a-zA-Z]{2,}$)"),
			   MIN(age, 0),
			   MAX(age, 200),
			   LENGTH(bio, 0, 500))
};

// ---- MIN / MAX 校验测试 ----

TEST(MetaJsonValidationTest, MinOk)
{
	boost::json::value json = boost::json::object {{"age", 25}, {"score", 50.0}, {"level", 50}};
	auto dto = meta::fromJson<MinMaxDTO>(json);
	EXPECT_EQ(dto.age, 25);
	EXPECT_DOUBLE_EQ(dto.score, 50.0);
	EXPECT_EQ(dto.level, 50);
}

TEST(MetaJsonValidationTest, MinUnderThrows)
{
	boost::json::value json = boost::json::object {{"age", -1}, {"score", 50.0}, {"level", 50}};
	EXPECT_THROW(meta::fromJson<MinMaxDTO>(json), std::runtime_error);
}

TEST(MetaJsonValidationTest, MaxOverThrows)
{
	boost::json::value json = boost::json::object {{"age", 201}, {"score", 50.0}, {"level", 50}};
	EXPECT_THROW(meta::fromJson<MinMaxDTO>(json), std::runtime_error);
}

TEST(MetaJsonValidationTest, MinMaxAtBoundary)
{
	// 边界值应通过
	boost::json::value json = boost::json::object {{"age", 0}, {"score", 100.0}, {"level", 1}};
	auto dto = meta::fromJson<MinMaxDTO>(json);
	EXPECT_EQ(dto.age, 0);
	EXPECT_DOUBLE_EQ(dto.score, 100.0);
	EXPECT_EQ(dto.level, 1);
}

TEST(MetaJsonValidationTest, MinMaxScoreUnderThrows)
{
	boost::json::value json = boost::json::object {{"age", 25}, {"score", -0.1}, {"level", 50}};
	EXPECT_THROW(meta::fromJson<MinMaxDTO>(json), std::runtime_error);
}

TEST(MetaJsonValidationTest, MinMaxLevelUnderThrows)
{
	boost::json::value json = boost::json::object {{"age", 25}, {"score", 50.0}, {"level", 0}};
	EXPECT_THROW(meta::fromJson<MinMaxDTO>(json), std::runtime_error);
}

// ---- NOT_EMPTY 校验测试 ----

TEST(MetaJsonValidationTest, NotEmptyOk)
{
	boost::json::value json = boost::json::object {{"name", "Alice"}, {"email", "a@b.com"}};
	auto dto = meta::fromJson<NotEmptyDTO>(json);
	EXPECT_EQ(dto.name, "Alice");
	EXPECT_EQ(dto.email, "a@b.com");
}

TEST(MetaJsonValidationTest, NotEmptyNameEmptyThrows)
{
	boost::json::value json = boost::json::object {{"name", ""}, {"email", "a@b.com"}};
	EXPECT_THROW(meta::fromJson<NotEmptyDTO>(json), std::runtime_error);
}

TEST(MetaJsonValidationTest, NotEmptyEmailEmptyThrows)
{
	boost::json::value json = boost::json::object {{"name", "Bob"}, {"email", ""}};
	EXPECT_THROW(meta::fromJson<NotEmptyDTO>(json), std::runtime_error);
}

// ---- PATTERN 校验测试 ----

TEST(MetaJsonValidationTest, PatternOk)
{
	boost::json::value json = boost::json::object {{"email", "test@example.com"}, {"phone", "13800138000"}};
	auto dto = meta::fromJson<PatternDTO>(json);
	EXPECT_EQ(dto.email, "test@example.com");
	EXPECT_EQ(dto.phone, "13800138000");
}

TEST(MetaJsonValidationTest, PatternEmailInvalidThrows)
{
	boost::json::value json = boost::json::object {{"email", "not-an-email"}, {"phone", "13800138000"}};
	EXPECT_THROW(meta::fromJson<PatternDTO>(json), std::runtime_error);
}

TEST(MetaJsonValidationTest, PatternPhoneInvalidThrows)
{
	boost::json::value json = boost::json::object {{"email", "valid@test.com"}, {"phone", "12345"}};
	EXPECT_THROW(meta::fromJson<PatternDTO>(json), std::runtime_error);
}

// ---- LENGTH 校验测试 ----

TEST(MetaJsonValidationTest, LengthOk)
{
	boost::json::value json = boost::json::object {{"username", "alice"}, {"bio", "hello"}};
	auto dto = meta::fromJson<LengthDTO>(json);
	EXPECT_EQ(dto.username, "alice");
	EXPECT_EQ(dto.bio, "hello");
}

TEST(MetaJsonValidationTest, LengthTooShortThrows)
{
	boost::json::value json = boost::json::object {{"username", "ab"}, {"bio", "hello"}};
	EXPECT_THROW(meta::fromJson<LengthDTO>(json), std::runtime_error);
}

TEST(MetaJsonValidationTest, LengthTooLongThrows)
{
	boost::json::value json = boost::json::object {{"username", "a-very-long-username-xxx"}, {"bio", "hello"}};
	EXPECT_THROW(meta::fromJson<LengthDTO>(json), std::runtime_error);
}

TEST(MetaJsonValidationTest, LengthAtBoundary)
{
	boost::json::value json = boost::json::object {{"username", "abc"}, {"bio", ""}};
	auto dto = meta::fromJson<LengthDTO>(json);
	EXPECT_EQ(dto.username, "abc");
	EXPECT_EQ(dto.bio, "");
}

// ---- 全约束组合测试 ----

TEST(MetaJsonValidationTest, AllValidationsOk)
{
	boost::json::value json = boost::json::object {{"username", "alice"},
												   {"email", "alice@example.com"},
												   {"age", 30},
												   {"bio", "hello world"}};
	auto dto = meta::fromJson<AllValidationsDTO>(json);
	EXPECT_EQ(dto.username, "alice");
	EXPECT_EQ(dto.email, "alice@example.com");
	EXPECT_EQ(dto.age, 30);
	EXPECT_EQ(dto.bio, "hello world");
}

TEST(MetaJsonValidationTest, AllValidationsMinUnderThrows)
{
	boost::json::value json =
		boost::json::object {{"username", "alice"}, {"email", "alice@example.com"}, {"age", -1}, {"bio", "hello"}};
	EXPECT_THROW(meta::fromJson<AllValidationsDTO>(json), std::runtime_error);
}

// ---- 校验与现有装饰器组合测试 ----

struct ValidatedAliasDTO
{
	std::string userName;
	int userAge;

	HICAL_JSON(ValidatedAliasDTO, ALIAS(userName, "user_name"), MIN(userAge, 0), MAX(userAge, 150))
};

TEST(MetaJsonValidationTest, ValidatedAliasOk)
{
	boost::json::value json = boost::json::object {{"user_name", "Alice"}, {"userAge", 30}};
	auto dto = meta::fromJson<ValidatedAliasDTO>(json);
	EXPECT_EQ(dto.userName, "Alice");
	EXPECT_EQ(dto.userAge, 30);
}

TEST(MetaJsonValidationTest, ValidatedAliasMinUnderThrows)
{
	boost::json::value json = boost::json::object {{"user_name", "Bob"}, {"userAge", -5}};
	EXPECT_THROW(meta::fromJson<ValidatedAliasDTO>(json), std::runtime_error);
}

// ---- 校验不改变 toJson 行为 ----

TEST(MetaJsonValidationTest, ValidationDoesNotAffectSerialize)
{
	MinMaxDTO dto {30, 75.5, 50};
	auto json = meta::toJson(dto);
	EXPECT_EQ(json["age"].as_int64(), 30);
	EXPECT_DOUBLE_EQ(json["score"].as_double(), 75.5);
	EXPECT_EQ(json["level"].as_int64(), 50);
}

// ---- 现有装饰器依然正常工作 ----

TEST(MetaJsonValidationTest, ExistingDecoratorsStillWork)
{
	// 验证 REQUIRED 仍正常工作
	boost::json::value json = boost::json::object {{"id", "user-001"}, {"name", "Eve"}, {"age", 28}};
	auto dto = meta::fromJson<RequiredDTO>(json);
	EXPECT_EQ(dto.id, "user-001");
	EXPECT_EQ(dto.name, "Eve");

	// 验证缺失 required 字段仍抛异常
	boost::json::value noId = boost::json::object {{"name", "Eve"}};
	EXPECT_THROW(meta::fromJson<RequiredDTO>(noId), std::runtime_error);

	// 验证 IGNORE 仍正常工作
	IgnoreDTO ignoreDto {"Jack", "j@test.com", "hash", "token"};
	auto ignoreJson = meta::toJson(ignoreDto);
	EXPECT_FALSE(ignoreJson.contains("passwordHash"));
}
