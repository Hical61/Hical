/**
 * @brief OpenAPI 自动生成完整示例
 * 演示 Hical 反射层与 OpenAPI 3.0 的集成：
 *   - DTO 定义 + HICAL_JSON + HICAL_SCHEMA_NAME
 *   - Handler 定义 + HICAL_HANDLER + HICAL_API + HICAL_ROUTES_WITH_API
 *   - 自动注册路由 + 收集 OpenAPI 元数据
 *   - 暴露 /openapi.json 和 /docs 端点
 * 运行后访问：
 *   http://localhost:8080/openapi.json  → OpenAPI 3.0 JSON spec
 *   http://localhost:8080/docs          → Swagger UI 页面
 */

#include "core/HttpServer.h"
#include "core/MetaJson.h"
#include "core/MetaRoutes.h"
#include "core/OpenApiSchema.h"
#include "core/OpenApiRegistry.h"
#include "core/OpenApiDocument.h"
#include "core/OpenApiEndpoint.h"

#include <iostream>
#include <vector>

using namespace hical;
using namespace hical::meta;
using namespace hical::meta::openapi;

// ============ DTO 定义 ============

struct UserDTO
{
	std::string name;
	int age;
	std::string email;
	HICAL_JSON(UserDTO, REQUIRED(name), age, email)
};
HICAL_SCHEMA_NAME(UserDTO, "UserDTO")

struct CreateUserRequest
{
	std::string name;
	int age;
	std::string email;
	HICAL_JSON(CreateUserRequest, REQUIRED(name), REQUIRED(age), email)
};
HICAL_SCHEMA_NAME(CreateUserRequest, "CreateUserRequest")

struct ErrorResponse
{
	int code;
	std::string message;
	HICAL_JSON(ErrorResponse, code, message)
};
HICAL_SCHEMA_NAME(ErrorResponse, "ErrorResponse")

struct StatusDTO
{
	std::string status;
	std::string version;
	std::string framework;
	HICAL_JSON(StatusDTO, status, version, framework)
};
HICAL_SCHEMA_NAME(StatusDTO, "StatusDTO")

// ============ Handler 定义 ============

struct ApiHandler
{
	// GET /api/status — 获取服务状态
	HttpResponse getStatus(const HttpRequest& /*req*/)
	{
		StatusDTO status {"running", "1.0.0", "hical"};
		return HttpResponse::json(toJson(status));
	}
	HICAL_HANDLER(Get, "/api/status", getStatus)
	HICAL_API(getStatus, builder::summary(info, "Get service status"); builder::tags(info, {"system"});
			  builder::response<StatusDTO>(info, 200, "Service status"))

	// GET /api/users — 列出所有用户
	HttpResponse listUsers(const HttpRequest& /*req*/)
	{
		boost::json::array users;
		users.push_back(toJson(UserDTO {"Alice", 30, "alice@example.com"}));
		users.push_back(toJson(UserDTO {"Bob", 25, "bob@example.com"}));
		return HttpResponse::json({{"users", users}, {"total", 2}});
	}
	HICAL_HANDLER(Get, "/api/users", listUsers)
	HICAL_API(listUsers, builder::summary(info, "List all users"); builder::tags(info, {"users"});
			  builder::response<UserDTO>(info, 200, "User list"))

	// POST /api/users — 创建用户
	HttpResponse createUser(const HttpRequest& req)
	{
		auto input = req.readJson<CreateUserRequest>();
		UserDTO user {input.name, input.age, input.email};
		auto res = HttpResponse::json(toJson(user));
		res.setStatus(HttpStatusCode::hCreated);
		return res;
	}
	HICAL_HANDLER(Post, "/api/users", createUser)
	HICAL_API(createUser, builder::summary(info, "Create a new user"); builder::tags(info, {"users"});
			  builder::request<CreateUserRequest>(info, "User data", true);
			  builder::response<UserDTO>(info, 201, "Created");
			  builder::responseDesc(info, 400, "Invalid request body"))

	// GET /api/users/{id} — 获取用户详情
	HttpResponse getUser(const HttpRequest& req)
	{
		auto id = req.param("id");
		UserDTO user {"User " + id, 20, id + "@example.com"};
		return HttpResponse::json(toJson(user));
	}
	HICAL_HANDLER(Get, "/api/users/{id}", getUser)
	HICAL_API(getUser, builder::summary(info, "Get user by ID"); builder::tags(info, {"users"});
			  builder::pathParam(info, "id", "integer", "User ID");
			  builder::response<UserDTO>(info, 200, "User details");
			  builder::responseDesc(info, 404, "User not found"))

	// DELETE /api/users/{id} — 删除用户
	HttpResponse deleteUser(const HttpRequest& req)
	{
		auto id = req.param("id");
		return HttpResponse::json({{"deleted", id}});
	}
	HICAL_HANDLER(Delete, "/api/users/{id}", deleteUser)
	HICAL_API(deleteUser, builder::summary(info, "Delete a user"); builder::tags(info, {"users"});
			  builder::pathParam(info, "id", "integer", "User ID");
			  builder::responseDesc(info, 204, "Deleted");
			  builder::responseDesc(info, 404, "User not found"))

	HICAL_ROUTES_WITH_API(ApiHandler, getStatus, listUsers, createUser, getUser, deleteUser)
};

// ============ main ============

int main()
{
	HttpServer server(8080);
	auto registry = std::make_shared<OpenApiRegistry>();

	// 注册所有 Schema
	std::unordered_map<std::string, boost::json::object> schemas;
	registerSchemas<UserDTO, CreateUserRequest, ErrorResponse, StatusDTO>(schemas);
	for (auto& [name, schema] : schemas)
	{
		registry->addSchema(name, std::move(schema));
	}

	// 注册路由 + 收集 OpenAPI 元数据
	ApiHandler handler;
	registerRoutesWithOpenApi(server.router(), handler, *registry);

	// 暴露 OpenAPI 端点
	auto doc = std::make_shared<OpenApiDocument>(
		registry,
		OpenApiConfig {.title = "Hical OpenAPI Demo",
					   .version = "1.0.0",
					   .description = "Demonstration of Hical's automatic OpenAPI generation",
					   .servers = {{"http://localhost:8080", "Local development server"}}});
	serveOpenApi(server.router(), doc);

	std::cout << "OpenAPI server running on http://localhost:8080" << std::endl;
	std::cout << "  API spec: http://localhost:8080/openapi.json" << std::endl;
	std::cout << "  Swagger UI: http://localhost:8080/docs" << std::endl;

	server.start();
	return 0;
}
