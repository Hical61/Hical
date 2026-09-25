#include "crow_all.h"
#include <string>

int main()
{
	crow::SimpleApp app;

	// 关闭 Crow 默认日志，减少 I/O 干扰
	app.loglevel(crow::LogLevel::Warning);

	// Hello World
	CROW_ROUTE(app, "/")
	(
		[]() -> crow::response
		{
			return crow::response(200, "Hello, World!");
		});

	// JSON 响应（不手动设 Content-Type，避免 Crow v1.2 set_header 性能 bug）
	CROW_ROUTE(app, "/api/status")
	(
		[]() -> crow::response
		{
			crow::json::wvalue obj;
			obj["status"] = "running";
			obj["framework"] = "crow";
			return crow::response(200, obj.dump());
		});

	// JSON 反序列化 + 序列化（Echo）
	CROW_ROUTE(app, "/api/echo")
		.methods(crow::HTTPMethod::Post)(
			[](const crow::request& req) -> crow::response
			{
				auto body = crow::json::load(req.body);
				if (!body)
				{
					return crow::response(400, "invalid json");
				}
				crow::json::wvalue echo(body);
				return crow::response(200, echo.dump());
			});

	// 路径参数：Crow 用 <string> 占位符
	CROW_ROUTE(app, "/users/<string>")
	(
		[](const std::string& id) -> crow::response
		{
			crow::json::wvalue obj;
			obj["userId"] = id;
			obj["name"] = "User " + id;
			return crow::response(200, obj.dump());
		});

	app.port(8084).concurrency(4).run();
}
