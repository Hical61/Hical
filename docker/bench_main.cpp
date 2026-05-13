#include "core/HttpServer.h"
#include "core/Coroutine.h"
#include "core/MetaJson.h"
#include "core/RouteGroup.h"
#include <boost/json.hpp>

using namespace hical;
namespace json = boost::json;

struct UserDTO
{
	std::string name;
	int age {0};
	std::string email;

	HICAL_JSON(UserDTO, name, age, email)
};

int main()
{
	HttpServer server(8080, 4);

	// Hello World
	server.router().get("/",
						[](const HttpRequest&) -> HttpResponse
						{
							return HttpResponse::ok("Hello, World!");
						});

	// JSON 响应
	server.router().get("/api/status",
						[](const HttpRequest&) -> HttpResponse
						{
							json::object obj;
							obj["status"] = "running";
							obj["framework"] = "hical";
							return HttpResponse::json(json::value(std::move(obj)));
						});

	// JSON 反序列化 + 序列化
	server.router().post("/api/echo",
						 [](const HttpRequest& req) -> Awaitable<HttpResponse>
						 {
							 auto user = req.readJson<UserDTO>();
							 co_return HttpResponse::json(meta::toJson(user));
						 });

	// 路径参数
	server.router().get("/users/{id}",
						[](const HttpRequest& req) -> HttpResponse
						{
							json::object obj;
							obj["userId"] = req.param("id");
							obj["name"] = "User " + req.param("id");
							return HttpResponse::json(json::value(std::move(obj)));
						});

	// ============ 中间件链测试端点 ============

	// 空操作洋葱中间件
	auto passthrough = [](HttpRequest& req, MiddlewareNext next) -> Awaitable<HttpResponse>
	{
		co_return co_await next(req);
	};

	// /middleware/0 — 无中间件
	server.router().get("/middleware/0",
						[](const HttpRequest&) -> HttpResponse
						{
							json::object obj;
							obj["middleware_count"] = 0;
							return HttpResponse::json(json::value(std::move(obj)));
						});

	// /middleware/3 — 3 层空操作中间件
	{
		auto g3 = server.router().group("");
		for (int i = 0; i < 3; ++i)
		{
			g3.use(passthrough);
		}
		g3.get("/middleware/3",
			   [](const HttpRequest&) -> HttpResponse
			   {
				   json::object obj;
				   obj["middleware_count"] = 3;
				   return HttpResponse::json(json::value(std::move(obj)));
			   });
	}

	// /middleware/10 — 10 层空操作中间件
	{
		auto g10 = server.router().group("");
		for (int i = 0; i < 10; ++i)
		{
			g10.use(passthrough);
		}
		g10.get("/middleware/10",
				[](const HttpRequest&) -> HttpResponse
				{
					json::object obj;
					obj["middleware_count"] = 10;
					return HttpResponse::json(json::value(std::move(obj)));
				});
	}

	// ============ 同步中间件链测试端点（SyncMiddleware 快速路径） ============

	SyncBeforeHandler passthroughSync = [](HttpRequest&) -> SyncMiddlewareResult
	{
		return std::nullopt; // 继续执行
	};

	// /sync-middleware/3 — 3 层同步中间件
	{
		auto gs3 = server.router().group("");
		for (int i = 0; i < 3; ++i)
		{
			gs3.use(passthroughSync);
		}
		gs3.get("/sync-middleware/3",
				[](const HttpRequest&) -> HttpResponse
				{
					json::object obj;
					obj["middleware_count"] = 3;
					obj["type"] = "sync";
					return HttpResponse::json(json::value(std::move(obj)));
				});
	}

	// /sync-middleware/10 — 10 层同步中间件（仅 1 个协程帧）
	{
		auto gs10 = server.router().group("");
		for (int i = 0; i < 10; ++i)
		{
			gs10.use(passthroughSync);
		}
		gs10.get("/sync-middleware/10",
				 [](const HttpRequest&) -> HttpResponse
				 {
					 json::object obj;
					 obj["middleware_count"] = 10;
					 obj["type"] = "sync";
					 return HttpResponse::json(json::value(std::move(obj)));
				 });
	}

	server.start();
}
