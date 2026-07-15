#include "crow_all.h"
#include <functional>
#include <string>

// 模拟 N 层空操作中间件调用链
crow::response runWithMiddleware(int layers, std::function<crow::response()> handler)
{
	std::function<crow::response()> chain = std::move(handler);
	for (int i = 0; i < layers; ++i)
	{
		chain = [prev = std::move(chain)]() -> crow::response
		{
			// 空操作，直接透传
			return prev();
		};
	}
	return chain();
}

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

	// 无中间件
	CROW_ROUTE(app, "/middleware/0")
	(
		[]() -> crow::response
		{
			crow::json::wvalue obj;
			obj["middleware_count"] = 0;
			return crow::response(200, obj.dump());
		});

	// 3 层空操作中间件（handler 内部构造调用链）
	CROW_ROUTE(app, "/middleware/3")
	(
		[]() -> crow::response
		{
			return runWithMiddleware(3,
									 []() -> crow::response
									 {
										 crow::json::wvalue obj;
										 obj["middleware_count"] = 3;
										 return crow::response(200, obj.dump());
									 });
		});

	// 10 层空操作中间件（handler 内部构造调用链）
	CROW_ROUTE(app, "/middleware/10")
	(
		[]() -> crow::response
		{
			return runWithMiddleware(10,
									 []() -> crow::response
									 {
										 crow::json::wvalue obj;
										 obj["middleware_count"] = 10;
										 return crow::response(200, obj.dump());
									 });
		});

	// /sync-filter — 模拟同步函数调用链
	CROW_ROUTE(app, "/sync-filter/3")
	(
		[]() -> crow::response
		{
			return runWithMiddleware(3,
									 []() -> crow::response
									 {
										 crow::json::wvalue obj;
										 obj["middleware_count"] = 3;
										 obj["type"] = "sync";
										 return crow::response(200, obj.dump());
									 });
		});

	CROW_ROUTE(app, "/sync-filter/10")
	(
		[]() -> crow::response
		{
			return runWithMiddleware(10,
									 []() -> crow::response
									 {
										 crow::json::wvalue obj;
										 obj["middleware_count"] = 10;
										 obj["type"] = "sync";
										 return crow::response(200, obj.dump());
									 });
		});

	app.port(8084).concurrency(4).run();
}
