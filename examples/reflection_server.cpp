#include "core/HttpServer.h"
#include "core/MetaJson.h"
#include "core/MetaRoutes.h"
#include <iostream>

using namespace hical;

// ============ DTO 结构体（自动 JSON 序列化） ============

struct UserDTO
{
	std::string name;
	int age;
	std::string email;

	HICAL_JSON(UserDTO, name, age, email)
};

struct StatusDTO
{
	std::string status;
	std::string version;
	std::string framework;

	HICAL_JSON(StatusDTO, status, version, framework)
};

// ============ Handler（自动路由注册） ============

struct ApiHandler
{
	// GET /api/status — 状态查询（返回 DTO 自动序列化为 JSON）
	HttpResponse getStatus(const HttpRequest&)
	{
		StatusDTO status {"running", "0.2.0", "hical"};
		return HttpResponse::json(meta::toJson(status));
	}
	HICAL_HANDLER(Get, "/api/status", getStatus)

	// GET /api/users — 用户列表
	HttpResponse listUsers(const HttpRequest&)
	{
		return HttpResponse::json({{"users",
									boost::json::array {boost::json::object {{"name", "Alice"}, {"age", 30}},
														boost::json::object {{"name", "Bob"}, {"age", 25}}}}});
	}
	HICAL_HANDLER(Get, "/api/users", listUsers)

	// GET /api/users/{id} — 查询单个用户（路径参数）
	HttpResponse getUser(const HttpRequest& req)
	{
		UserDTO user {"User " + req.param("id"), 20, req.param("id") + "@example.com"};
		return HttpResponse::json(meta::toJson(user));
	}
	HICAL_HANDLER(Get, "/api/users/{id}", getUser)

	// POST /api/users — 创建用户（JSON 请求体自动反序列化）
	HttpResponse createUser(const HttpRequest& req)
	{
		auto user = req.readJson<UserDTO>();
		return HttpResponse::json(
			{{"message", "User created"}, {"name", user.name}, {"age", user.age}, {"email", user.email}});
	}
	HICAL_HANDLER(Post, "/api/users", createUser)

	// 收集所有路由
	HICAL_ROUTES(ApiHandler, getStatus, listUsers, getUser, createUser)
};

// ============ 主函数 ============

int main(int argc, char* argv[])
{
	try
	{
		auto port = static_cast<uint16_t>(argc >= 2 ? std::atoi(argv[1]) : 8080);

		HttpServer server(port);

		// 日志中间件
		server.use(
			[](HttpRequest& req, MiddlewareNext next) -> Awaitable<HttpResponse>
			{
				std::cout << httpMethodToString(req.method()) << " " << req.path() << std::endl;
				auto res = co_await next(req);
				std::cout << "  -> " << static_cast<int>(res.statusCode()) << std::endl;
				co_return res;
			});

		// 使用反射自动注册所有路由
		ApiHandler handler;
		meta::registerRoutes(server.router(), handler);

		// 首页（手动注册，与反射路由共存）
		server.router().get("/",
							[](const HttpRequest&) -> HttpResponse
							{
								return HttpResponse::ok("hical reflection server v0.2.0");
							});

		std::cout << "hical Reflection Server v0.2.0" << std::endl;
		std::cout << "Port: " << port << std::endl;
		std::cout << "Routes (auto-registered via reflection):" << std::endl;
		std::cout << "  GET    /              — Homepage" << std::endl;
		std::cout << "  GET    /api/status    — Status (DTO auto-serialized)" << std::endl;
		std::cout << "  GET    /api/users     — User list" << std::endl;
		std::cout << "  GET    /api/users/{id} — Get user (path param + DTO)" << std::endl;
		std::cout << "  POST   /api/users     — Create user (JSON auto-deserialized)" << std::endl;

		server.start();
	}
	catch (const std::exception& e)
	{
		std::cerr << "Error: " << e.what() << std::endl;
	}

	return 0;
}
