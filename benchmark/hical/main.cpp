#include <hical/core/HttpServer.h>
#include <hical/core/Coroutine.h>
#include <hical/core/MetaJson.h>
#include <hical/core/RouteGroup.h>
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
	server.setIdleTimeout(0); // benchmark 无需空闲检测，省掉 IdleScanner 的注册/注销和活跃时间戳写入

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
	// 不参与六框架横评（其余五家没有可比的原生运行时中间件机制），
	// 只留给 Hical 自己跟踪中间件开销

	// 空操作洋葱中间件（纯透传，测量框架中间件调度开销）
	auto passthrough = [](HttpRequest& req, MiddlewareNext next) -> Awaitable<HttpResponse>
	{
		co_return co_await next(req);
	};

	// /middleware/0 — 无中间件（直接注册到 router）
	server.router().get("/middleware/0",
						[](const HttpRequest&) -> HttpResponse
						{
							json::object obj;
							obj["middleware_count"] = 0;
							return HttpResponse::json(json::value(std::move(obj)));
						});

	// /middleware/10 — 10 层空操作中间件（RouteGroup 路由级中间件）
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

	server.start();
}
