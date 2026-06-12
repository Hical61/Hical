/**
 * @brief OpenAPI 3.0 自动生成全链路测试
 * 覆盖：Schema 生成、Registry、文档组装、端点暴露
 */

#include <gtest/gtest.h>

#include "core/MetaJson.h"
#include "core/MetaRoutes.h"
#include "core/OpenApiSchema.h"
#include "core/OpenApiRegistry.h"
#include "core/OpenApiDocument.h"
#include "core/OpenApiEndpoint.h"

using namespace hical;
using namespace hical::meta;
using namespace hical::meta::openapi;

// ============ 测试 DTO 定义 ============

struct SimpleDTO
{
	std::string name;
	int age;
	HICAL_JSON(SimpleDTO, name, age)
};

struct RequiredDTO
{
	std::string id;
	int code;
	HICAL_JSON(RequiredDTO, REQUIRED(id), REQUIRED(code))
};

struct AliasDTO
{
	std::string requestId;
	std::string statusMessage;
	HICAL_JSON(AliasDTO, ALIAS(requestId, "request_id"), ALIAS(statusMessage, "status_message"))
};

struct IgnoreDTO
{
	std::string visible;
	std::string hidden;
	HICAL_JSON(IgnoreDTO, visible, HICAL_IGNORE(hidden))
};

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

struct WithVector
{
	std::string name;
	std::vector<int> scores;
	HICAL_JSON(WithVector, name, scores)
};

struct Uint64DTO
{
	uint64_t bigId;
	HICAL_JSON(Uint64DTO, bigId)
};

struct WithDouble
{
	double price;
	float rating;
	HICAL_JSON(WithDouble, price, rating)
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
HICAL_SCHEMA_NAME(NestedAddress, "NestedAddress")

struct NamedDTO
{
	std::string value;
	HICAL_JSON(NamedDTO, value)
};
HICAL_SCHEMA_NAME(NamedDTO, "NamedDTO")

struct WithNamedNested
{
	std::string title;
	NamedDTO nested;
	HICAL_JSON(WithNamedNested, title, nested)
};

struct WithVectorOfNamed
{
	std::vector<NamedDTO> items;
	HICAL_JSON(WithVectorOfNamed, items)
};

// ============ Phase 1: Schema 生成测试 ============

TEST(OpenApiSchemaTest, StringField)
{
	auto schema = jsonSchema<SimpleDTO>();
	EXPECT_EQ(schema["type"], "object");

	auto& props = schema["properties"].as_object();
	EXPECT_EQ(props["name"].as_object()["type"], "string");
}

TEST(OpenApiSchemaTest, IntegerField)
{
	auto schema = jsonSchema<SimpleDTO>();
	auto& props = schema["properties"].as_object();
	auto& ageProp = props["age"].as_object();
	EXPECT_EQ(ageProp["type"], "integer");
	EXPECT_EQ(ageProp["format"], "int32");
}

TEST(OpenApiSchemaTest, Uint64Field)
{
	auto schema = jsonSchema<Uint64DTO>();
	auto& props = schema["properties"].as_object();
	auto& bigIdProp = props["bigId"].as_object();
	EXPECT_EQ(bigIdProp["type"], "integer");
	EXPECT_EQ(bigIdProp["format"], "int64");
	EXPECT_EQ(bigIdProp["minimum"], 0);
}

TEST(OpenApiSchemaTest, DoubleField)
{
	auto schema = jsonSchema<WithDouble>();
	auto& props = schema["properties"].as_object();
	EXPECT_EQ(props["price"].as_object()["type"], "number");
	EXPECT_EQ(props["price"].as_object()["format"], "double");
	EXPECT_EQ(props["rating"].as_object()["type"], "number");
	EXPECT_EQ(props["rating"].as_object()["format"], "float");
}

TEST(OpenApiSchemaTest, BoolField)
{
	// bool 类型需要一个专门的 DTO
	struct BoolDTO
	{
		bool active;
		HICAL_JSON(BoolDTO, active)
	};

	auto schema = jsonSchema<BoolDTO>();
	auto& props = schema["properties"].as_object();
	EXPECT_EQ(props["active"].as_object()["type"], "boolean");
}

TEST(OpenApiSchemaTest, VectorField)
{
	auto schema = jsonSchema<WithVector>();
	auto& props = schema["properties"].as_object();
	auto& scoresProp = props["scores"].as_object();
	EXPECT_EQ(scoresProp["type"], "array");
	EXPECT_EQ(scoresProp["items"].as_object()["type"], "integer");
}

TEST(OpenApiSchemaTest, RequiredFields)
{
	auto schema = jsonSchema<RequiredDTO>();
	ASSERT_TRUE(schema.contains("required"));
	auto& required = schema["required"].as_array();
	EXPECT_EQ(required.size(), 2u);

	// 检查 required 数组包含 "id" 和 "code"
	bool hasId = false;
	bool hasCode = false;
	for (const auto& r : required)
	{
		if (r.as_string() == "id")
		{
			hasId = true;
		}
		if (r.as_string() == "code")
		{
			hasCode = true;
		}
	}
	EXPECT_TRUE(hasId);
	EXPECT_TRUE(hasCode);
}

TEST(OpenApiSchemaTest, IgnoredFields)
{
	auto schema = jsonSchema<IgnoreDTO>();
	auto& props = schema["properties"].as_object();
	EXPECT_TRUE(props.contains("visible"));
	EXPECT_FALSE(props.contains("hidden"));
}

TEST(OpenApiSchemaTest, AliasFields)
{
	auto schema = jsonSchema<AliasDTO>();
	auto& props = schema["properties"].as_object();
	EXPECT_TRUE(props.contains("request_id"));
	EXPECT_TRUE(props.contains("status_message"));
	EXPECT_FALSE(props.contains("requestId"));
	EXPECT_FALSE(props.contains("statusMessage"));
}

TEST(OpenApiSchemaTest, MixedDecorators)
{
	auto schema = jsonSchema<MixedDTO>();
	auto& props = schema["properties"].as_object();

	// 别名字段
	EXPECT_TRUE(props.contains("request_id"));
	EXPECT_TRUE(props.contains("status_message"));
	// 普通字段
	EXPECT_TRUE(props.contains("statusCode"));
	EXPECT_TRUE(props.contains("debugInfo"));
	// 忽略字段
	EXPECT_FALSE(props.contains("traceId"));

	// required
	auto& required = schema["required"].as_array();
	EXPECT_EQ(required.size(), 2u);
}

TEST(OpenApiSchemaTest, NestedStructInline)
{
	auto schema = jsonSchema<UserWithAddress>();
	auto& props = schema["properties"].as_object();
	auto& addrProp = props["address"].as_object();
	// NestedAddress 有 SchemaName → 应生成 $ref
	EXPECT_TRUE(addrProp.contains("$ref"));
	EXPECT_EQ(addrProp["$ref"], "#/components/schemas/NestedAddress");
}

TEST(OpenApiSchemaTest, NestedStructWithRef)
{
	auto schema = jsonSchema<WithNamedNested>();
	auto& props = schema["properties"].as_object();
	auto& nestedProp = props["nested"].as_object();
	EXPECT_TRUE(nestedProp.contains("$ref"));
	EXPECT_EQ(nestedProp["$ref"], "#/components/schemas/NamedDTO");
}

TEST(OpenApiSchemaTest, VectorOfNamedRef)
{
	auto schema = jsonSchema<WithVectorOfNamed>();
	auto& props = schema["properties"].as_object();
	auto& itemsProp = props["items"].as_object();
	EXPECT_EQ(itemsProp["type"], "array");
	auto& itemsItems = itemsProp["items"].as_object();
	EXPECT_TRUE(itemsItems.contains("$ref"));
	EXPECT_EQ(itemsItems["$ref"], "#/components/schemas/NamedDTO");
}

TEST(OpenApiSchemaTest, CollectSchemas)
{
	std::unordered_map<std::string, boost::json::object> schemas;
	collectSchemas<UserWithAddress>(schemas);
	// UserWithAddress 没有 SchemaName → 不在 map 中
	// 但嵌套的 NestedAddress 有 → 应在 map 中
	EXPECT_TRUE(schemas.find("NestedAddress") != schemas.end());
	EXPECT_TRUE(schemas.find("UserWithAddress") == schemas.end());
}

TEST(OpenApiSchemaTest, CollectSchemasNamed)
{
	std::unordered_map<std::string, boost::json::object> schemas;
	collectSchemas<NamedDTO>(schemas);
	EXPECT_TRUE(schemas.find("NamedDTO") != schemas.end());
}

TEST(OpenApiSchemaTest, RegisterMultipleSchemas)
{
	std::unordered_map<std::string, boost::json::object> schemas;
	registerSchemas<NamedDTO, NestedAddress>(schemas);
	EXPECT_EQ(schemas.size(), 2u);
	EXPECT_TRUE(schemas.find("NamedDTO") != schemas.end());
	EXPECT_TRUE(schemas.find("NestedAddress") != schemas.end());
}

// ============ Phase 2: Registry 测试 ============

TEST(OpenApiRegistryTest, AddRouteBasic)
{
	OpenApiRegistry registry;
	registry.addRoute(HttpMethod::hGet, "/api/test", "testHandler");
	auto routes = registry.routes();
	EXPECT_EQ(routes.size(), 1u);
	EXPECT_EQ(routes[0].path, "/api/test");
	EXPECT_EQ(routes[0].handlerName, "testHandler");
	EXPECT_EQ(routes[0].method, HttpMethod::hGet);
}

TEST(OpenApiRegistryTest, AddSchema)
{
	OpenApiRegistry registry;
	auto schema = jsonSchema<SimpleDTO>();
	registry.addSchema("SimpleDTO", schema);
	EXPECT_TRUE(registry.hasSchema("SimpleDTO"));
	EXPECT_FALSE(registry.hasSchema("NonExistent"));
}

// 测试 Handler 无 HICAL_API 标注
struct BasicHandler
{
	HttpResponse listItems(const HttpRequest& /*req*/)
	{
		return HttpResponse::ok();
	}
	HICAL_HANDLER(Get, "/api/items", listItems)
	HICAL_ROUTES(BasicHandler, listItems)
};

TEST(OpenApiRegistryTest, RegisterWithoutAnnotations)
{
	Router router;
	BasicHandler handler;
	OpenApiRegistry registry;

	registerRoutesWithOpenApi(router, handler, registry);

	auto routes = registry.routes();
	EXPECT_EQ(routes.size(), 1u);
	EXPECT_EQ(routes[0].path, "/api/items");
	EXPECT_EQ(routes[0].method, HttpMethod::hGet);
	EXPECT_EQ(routes[0].handlerName, "listItems");
}

// 测试 Handler 有 HICAL_API 标注
HICAL_SCHEMA_NAME(SimpleDTO, "SimpleDTO")

struct AnnotatedHandler
{
	HttpResponse listUsers(const HttpRequest& /*req*/)
	{
		return HttpResponse::ok();
	}
	HICAL_HANDLER(Get, "/api/users", listUsers)
	HICAL_API(listUsers, builder::summary(info, "List all users"); builder::tags(info, {"users"});
			  builder::response<SimpleDTO>(info, 200, "User list"))

	HttpResponse createUser(const HttpRequest& /*req*/)
	{
		return HttpResponse::ok();
	}
	HICAL_HANDLER(Post, "/api/users", createUser)
	HICAL_API(createUser, builder::summary(info, "Create user"); builder::request<SimpleDTO>(info, "User data", true);
			  builder::response<SimpleDTO>(info, 201, "Created");
			  builder::responseDesc(info, 400, "Validation error"))

	HttpResponse getUser(const HttpRequest& /*req*/)
	{
		return HttpResponse::ok();
	}
	HICAL_HANDLER(Get, "/api/users/{id}", getUser)
	HICAL_API(getUser, builder::summary(info, "Get user by ID"); builder::pathParam(info, "id", "integer", "User ID"))

	HICAL_ROUTES_WITH_API(AnnotatedHandler, listUsers, createUser, getUser)
};

TEST(OpenApiRegistryTest, RegisterWithAnnotations)
{
	Router router;
	AnnotatedHandler handler;
	OpenApiRegistry registry;

	registerRoutesWithOpenApi(router, handler, registry);

	auto routes = registry.routes();
	EXPECT_EQ(routes.size(), 3u);

	// 检查 listUsers
	const auto& listRoute = routes[0];
	EXPECT_EQ(listRoute.apiInfo.summary, "List all users");
	EXPECT_EQ(listRoute.apiInfo.tags.size(), 1u);
	EXPECT_EQ(listRoute.apiInfo.tags[0], "users");
	EXPECT_TRUE(listRoute.apiInfo.responses.find(200) != listRoute.apiInfo.responses.end());

	// 检查 createUser
	const auto& createRoute = routes[1];
	EXPECT_EQ(createRoute.apiInfo.summary, "Create user");
	EXPECT_TRUE(createRoute.apiInfo.requestBodySchema != nullptr);
	EXPECT_TRUE(createRoute.apiInfo.requestBodyRequired);
	EXPECT_TRUE(createRoute.apiInfo.responses.find(201) != createRoute.apiInfo.responses.end());
	EXPECT_TRUE(createRoute.apiInfo.responses.find(400) != createRoute.apiInfo.responses.end());

	// 检查 getUser
	const auto& getRoute = routes[2];
	EXPECT_EQ(getRoute.apiInfo.summary, "Get user by ID");
	EXPECT_EQ(getRoute.apiInfo.parameters.size(), 1u);
	EXPECT_EQ(getRoute.apiInfo.parameters[0].name, "id");
	EXPECT_EQ(getRoute.apiInfo.parameters[0].schemaType, "integer");
}

// 测试混合标注（部分路由有标注、部分无标注）
struct MixedHandler
{
	HttpResponse annotated(const HttpRequest& /*req*/)
	{
		return HttpResponse::ok();
	}
	HICAL_HANDLER(Get, "/annotated", annotated)
	HICAL_API(annotated, builder::summary(info, "Has annotation"))

	HttpResponse plain(const HttpRequest& /*req*/)
	{
		return HttpResponse::ok();
	}
	HICAL_HANDLER(Get, "/plain", plain)
	HICAL_API_DEFAULT(plain)

	HICAL_ROUTES_WITH_API(MixedHandler, annotated, plain)
};

TEST(OpenApiRegistryTest, MixedAnnotations)
{
	Router router;
	MixedHandler handler;
	OpenApiRegistry registry;

	registerRoutesWithOpenApi(router, handler, registry);

	auto routes = registry.routes();
	EXPECT_EQ(routes.size(), 2u);
	EXPECT_EQ(routes[0].apiInfo.summary, "Has annotation");
	EXPECT_EQ(routes[1].apiInfo.summary, "");          // 无标注 → 默认空
	EXPECT_EQ(routes[1].apiInfo.operationId, "plain"); // 自动生成
}

// ============ Phase 3: 文档组装测试 ============

TEST(OpenApiDocumentTest, MinimalDocument)
{
	auto registry = std::make_shared<OpenApiRegistry>();
	registry->addRoute(HttpMethod::hGet, "/health", "healthCheck");

	OpenApiDocument doc(registry);
	const auto& json = doc.generateString();
	auto parsed = boost::json::parse(json).as_object();

	EXPECT_EQ(parsed["openapi"], "3.0.3");
	EXPECT_TRUE(parsed.contains("info"));
	EXPECT_TRUE(parsed.contains("paths"));
}

TEST(OpenApiDocumentTest, InfoSection)
{
	auto registry = std::make_shared<OpenApiRegistry>();
	OpenApiConfig config {.title = "Test API", .version = "2.0.0", .description = "A test", .servers = {}};
	OpenApiDocument doc(registry, config);
	const auto& json = doc.generateString();
	auto parsed = boost::json::parse(json).as_object();

	auto& info = parsed["info"].as_object();
	EXPECT_EQ(info["title"], "Test API");
	EXPECT_EQ(info["version"], "2.0.0");
	EXPECT_EQ(info["description"], "A test");
}

TEST(OpenApiDocumentTest, PathsFromRegistry)
{
	auto registry = std::make_shared<OpenApiRegistry>();
	RouteApiInfo apiInfo;
	apiInfo.operationId = "listUsers";
	apiInfo.summary = "List users";
	registry->addRoute(HttpMethod::hGet, "/api/users", "listUsers", std::move(apiInfo));

	OpenApiDocument doc(registry);
	const auto& json = doc.generateString();
	auto parsed = boost::json::parse(json).as_object();

	auto& paths = parsed["paths"].as_object();
	EXPECT_TRUE(paths.contains("/api/users"));
	auto& pathItem = paths["/api/users"].as_object();
	EXPECT_TRUE(pathItem.contains("get"));
	auto& op = pathItem["get"].as_object();
	EXPECT_EQ(op["operationId"], "listUsers");
	EXPECT_EQ(op["summary"], "List users");
}

TEST(OpenApiDocumentTest, SamePathDifferentMethods)
{
	auto registry = std::make_shared<OpenApiRegistry>();
	registry->addRoute(HttpMethod::hGet, "/api/users", "listUsers");
	registry->addRoute(HttpMethod::hPost, "/api/users", "createUser");

	OpenApiDocument doc(registry);
	const auto& json = doc.generateString();
	auto parsed = boost::json::parse(json).as_object();

	auto& pathItem = parsed["paths"].as_object()["/api/users"].as_object();
	EXPECT_TRUE(pathItem.contains("get"));
	EXPECT_TRUE(pathItem.contains("post"));
}

TEST(OpenApiDocumentTest, PathParamsAutoExtracted)
{
	auto registry = std::make_shared<OpenApiRegistry>();
	registry->addRoute(HttpMethod::hGet, "/api/users/{id}", "getUser");

	OpenApiDocument doc(registry);
	const auto& json = doc.generateString();
	auto parsed = boost::json::parse(json).as_object();

	auto& op = parsed["paths"].as_object()["/api/users/{id}"].as_object()["get"].as_object();
	ASSERT_TRUE(op.contains("parameters"));
	auto& params = op["parameters"].as_array();
	EXPECT_EQ(params.size(), 1u);
	EXPECT_EQ(params[0].as_object()["name"], "id");
	EXPECT_EQ(params[0].as_object()["in"], "path");
	EXPECT_EQ(params[0].as_object()["required"], true);
}

TEST(OpenApiDocumentTest, ComponentsSchemas)
{
	auto registry = std::make_shared<OpenApiRegistry>();
	registry->addSchema("SimpleDTO", jsonSchema<SimpleDTO>());

	OpenApiDocument doc(registry);
	const auto& json = doc.generateString();
	auto parsed = boost::json::parse(json).as_object();

	ASSERT_TRUE(parsed.contains("components"));
	auto& schemas = parsed["components"].as_object()["schemas"].as_object();
	EXPECT_TRUE(schemas.contains("SimpleDTO"));
	auto& simpleSchema = schemas["SimpleDTO"].as_object();
	EXPECT_EQ(simpleSchema["type"], "object");
}

TEST(OpenApiDocumentTest, CachingBehavior)
{
	auto registry = std::make_shared<OpenApiRegistry>();
	registry->addRoute(HttpMethod::hGet, "/test", "test");

	OpenApiDocument doc(registry);
	auto first = doc.generateString();
	auto second = doc.generateString();
	// 缓存命中，内容一致
	EXPECT_EQ(first, second);
}

TEST(OpenApiDocumentTest, InvalidateCache)
{
	auto registry = std::make_shared<OpenApiRegistry>();
	registry->addRoute(HttpMethod::hGet, "/test", "test");

	OpenApiDocument doc(registry);
	const auto& first = doc.generateString();
	EXPECT_FALSE(first.empty());

	doc.invalidate();
	auto second = doc.generateString();
	EXPECT_FALSE(second.empty());
	EXPECT_EQ(first, second); // 内容一样，但已经是新字符串了
}

TEST(OpenApiDocumentTest, DefaultResponse)
{
	auto registry = std::make_shared<OpenApiRegistry>();
	registry->addRoute(HttpMethod::hGet, "/test", "test");

	OpenApiDocument doc(registry);
	const auto& json = doc.generateString();
	auto parsed = boost::json::parse(json).as_object();

	auto& op = parsed["paths"].as_object()["/test"].as_object()["get"].as_object();
	auto& responses = op["responses"].as_object();
	EXPECT_TRUE(responses.contains("200"));
	EXPECT_EQ(responses["200"].as_object()["description"], "OK");
}

TEST(OpenApiDocumentTest, RequestBodyInDocument)
{
	auto registry = std::make_shared<OpenApiRegistry>();
	RouteApiInfo apiInfo;
	apiInfo.operationId = "createUser";
	builder::request<SimpleDTO>(apiInfo, "User payload", true);
	registry->addRoute(HttpMethod::hPost, "/api/users", "createUser", std::move(apiInfo));

	OpenApiDocument doc(registry);
	const auto& json = doc.generateString();
	auto parsed = boost::json::parse(json).as_object();

	auto& op = parsed["paths"].as_object()["/api/users"].as_object()["post"].as_object();
	ASSERT_TRUE(op.contains("requestBody"));
	auto& reqBody = op["requestBody"].as_object();
	EXPECT_EQ(reqBody["required"], true);
	EXPECT_EQ(reqBody["description"], "User payload");
	EXPECT_TRUE(reqBody["content"].as_object().contains("application/json"));
}

TEST(OpenApiDocumentTest, ServersSection)
{
	auto registry = std::make_shared<OpenApiRegistry>();
	OpenApiConfig config;
	config.servers = {{"http://localhost:8080", "Local"}, {"https://api.example.com", "Production"}};
	OpenApiDocument doc(registry, config);
	const auto& json = doc.generateString();
	auto parsed = boost::json::parse(json).as_object();

	ASSERT_TRUE(parsed.contains("servers"));
	auto& servers = parsed["servers"].as_array();
	EXPECT_EQ(servers.size(), 2u);
	EXPECT_EQ(servers[0].as_object()["url"], "http://localhost:8080");
	EXPECT_EQ(servers[1].as_object()["url"], "https://api.example.com");
}

// ============ Phase 4: 端点暴露测试 ============

TEST(OpenApiEndpointTest, ServeOpenApiRegistersRoutes)
{
	Router router;
	auto registry = std::make_shared<OpenApiRegistry>();
	auto doc = std::make_shared<OpenApiDocument>(registry);

	serveOpenApi(router, doc);

	// 应注册 2 个路由：/openapi.json + /docs
	EXPECT_EQ(router.routeCount(), 2u);
}

TEST(OpenApiEndpointTest, CustomPaths)
{
	Router router;
	auto registry = std::make_shared<OpenApiRegistry>();
	auto doc = std::make_shared<OpenApiDocument>(registry);

	serveOpenApi(router, doc, "/api/spec", "/api/docs");

	EXPECT_EQ(router.routeCount(), 2u);
}

// ============ 端到端集成测试 ============

TEST(OpenApiIntegrationTest, FullWorkflow)
{
	// 1. 设置 Registry
	auto registry = std::make_shared<OpenApiRegistry>();
	registry->addSchema("SimpleDTO", jsonSchema<SimpleDTO>());

	// 2. 注册带标注的 Handler
	Router router;
	AnnotatedHandler handler;
	registerRoutesWithOpenApi(router, handler, *registry);

	// 3. 生成文档
	OpenApiConfig config {.title = "Integration Test API",
						  .version = "1.0.0",
						  .description = "Full integration test",
						  .servers = {}};
	OpenApiDocument doc(registry, config);
	const auto& json = doc.generateString();

	// 4. 验证文档结构
	auto parsed = boost::json::parse(json).as_object();
	EXPECT_EQ(parsed["openapi"], "3.0.3");
	EXPECT_EQ(parsed["info"].as_object()["title"], "Integration Test API");

	auto& paths = parsed["paths"].as_object();
	EXPECT_TRUE(paths.contains("/api/users"));
	EXPECT_TRUE(paths.contains("/api/users/{id}"));

	// /api/users 应有 get 和 post
	auto& usersPath = paths["/api/users"].as_object();
	EXPECT_TRUE(usersPath.contains("get"));
	EXPECT_TRUE(usersPath.contains("post"));

	// get 操作应有 summary
	EXPECT_EQ(usersPath["get"].as_object()["summary"], "List all users");

	// post 操作应有 requestBody
	EXPECT_TRUE(usersPath["post"].as_object().contains("requestBody"));

	// /api/users/{id} 应有 parameters
	auto& userIdOp = paths["/api/users/{id}"].as_object()["get"].as_object();
	EXPECT_TRUE(userIdOp.contains("parameters"));

	// components/schemas 应有 SimpleDTO
	auto& schemas = parsed["components"].as_object()["schemas"].as_object();
	EXPECT_TRUE(schemas.contains("SimpleDTO"));
}
