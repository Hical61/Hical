#include "core/HttpServer.h"
#include "core/WebSocket.h"
#include <iostream>

using namespace hical;

int main(int argc, char* argv[])
{
	try
	{
		auto port = static_cast<uint16_t>(argc >= 2 ? std::atoi(argv[1]) : 8080);

		HttpServer server(port);

		// ============ 中间件 ============

		// 日志中间件
		server.use(
			[](const HttpRequest& req, MiddlewareNext next) -> Awaitable<HttpResponse>
			{
				std::cout << httpMethodToString(req.method()) << " " << req.path() << std::endl;
				auto res = co_await next(req);
				std::cout << "  -> " << static_cast<int>(res.statusCode()) << std::endl;
				co_return res;
			});

		// ============ HTTP 路由 ============

		// GET / — 首页
		server.router().get("/",
							[](const HttpRequest&) -> HttpResponse
							{
								return HttpResponse::ok("Welcome to hical!");
							});

		// GET /api/status — 状态查询
		server.router().get("/api/status",
							[](const HttpRequest&) -> HttpResponse
							{
								return HttpResponse::json(
									{{"status", "running"}, {"version", "0.2.0"}, {"framework", "hical"}});
							});

		// POST /api/echo — Echo 回写
		server.router().post("/api/echo",
							 [](const HttpRequest& req) -> HttpResponse
							 {
								 return HttpResponse::ok(req.body());
							 });

		// GET /api/hello — 带查询参数示例
		server.router().get("/api/hello",
							[](const HttpRequest& req) -> HttpResponse
							{
								auto query = req.query();
								if (query.empty())
								{
									return HttpResponse::ok("Hello, World!");
								}
								return HttpResponse::ok("Hello! query=" + query);
							});

		// GET /users/{id} — 路径参数示例
		server.router().get("/users/{id}",
							[](const HttpRequest& req) -> HttpResponse
							{
								return HttpResponse::json(
									{{"userId", req.param("id")}, {"name", "User " + req.param("id")}});
							});

		// ============ WebSocket 路由 ============

		// WebSocket Echo
		server.router().ws(
			"/ws/echo",
			[](const std::string& msg, WebSocketSession& ws) -> Awaitable<void>
			{
				co_await ws.send("Echo: " + msg);
			},
			[](WebSocketSession& ws) -> Awaitable<void>
			{
				co_await ws.send("Connected to hical WebSocket!");
			});

		// ============ 启动 ============

		std::cout << "hical HTTP Server v0.2.0" << std::endl;
		std::cout << "监听端口: " << port << std::endl;
		std::cout << "路由:" << std::endl;
		std::cout << "  GET    /              — 首页" << std::endl;
		std::cout << "  GET    /api/status    — 状态查询" << std::endl;
		std::cout << "  POST   /api/echo      — Echo 回写" << std::endl;
		std::cout << "  GET    /api/hello     — Hello 示例" << std::endl;
		std::cout << "  GET    /users/{id}    — 用户查询" << std::endl;
		std::cout << "  WS     /ws/echo       — WebSocket Echo" << std::endl;

		server.start();
	}
	catch (const std::exception& e)
	{
		std::cerr << "异常: " << e.what() << std::endl;
	}

	return 0;
}
