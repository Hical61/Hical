#include "httplib.h"
#include "json.hpp"
#include <functional>
#include <string>

using json = nlohmann::json;

// 模拟 N 层空操作中间件调用链（与 Crow/Oat++ 方案一致）
std::string runWithMiddleware(int layers, std::function<std::string()> handler)
{
	auto chain = std::move(handler);
	for (int i = 0; i < layers; ++i)
	{
		chain = [prev = std::move(chain)]() -> std::string
		{
			// 空操作，直接透传
			return prev();
		};
	}
	return chain();
}

int main()
{
	httplib::Server svr;

	// 设置线程池大小为 4（与其他框架一致）
	svr.new_task_queue = []
	{
		return new httplib::ThreadPool(4);
	};

	// Hello World
	svr.Get("/",
			[](const httplib::Request&, httplib::Response& res)
			{
				res.set_content("Hello, World!", "text/plain");
			});

	// JSON 响应
	svr.Get("/api/status",
			[](const httplib::Request&, httplib::Response& res)
			{
				json obj;
				obj["status"] = "running";
				obj["framework"] = "cpp-httplib";
				res.set_content(obj.dump(), "application/json");
			});

	// JSON 反序列化 + 序列化（Echo）
	svr.Post("/api/echo",
			 [](const httplib::Request& req, httplib::Response& res)
			 {
				 auto body = json::parse(req.body, nullptr, false);
				 if (body.is_discarded())
				 {
					 res.status = 400;
					 res.set_content("invalid json", "text/plain");
					 return;
				 }
				 res.set_content(body.dump(), "application/json");
			 });

	// 路径参数：cpp-httplib 使用正则捕获组
	svr.Get(R"(/users/(\w+))",
			[](const httplib::Request& req, httplib::Response& res)
			{
				auto id = req.matches[1].str();
				json obj;
				obj["userId"] = id;
				obj["name"] = "User " + id;
				res.set_content(obj.dump(), "application/json");
			});

	// ============ 中间件链测试端点 ============

	// 无中间件
	svr.Get("/middleware/0",
			[](const httplib::Request&, httplib::Response& res)
			{
				json obj;
				obj["middleware_count"] = 0;
				res.set_content(obj.dump(), "application/json");
			});

	// 3 层空操作中间件
	svr.Get("/middleware/3",
			[](const httplib::Request&, httplib::Response& res)
			{
				auto result = runWithMiddleware(3,
												[]() -> std::string
												{
													json obj;
													obj["middleware_count"] = 3;
													return obj.dump();
												});
				res.set_content(result, "application/json");
			});

	// 10 层空操作中间件
	svr.Get("/middleware/10",
			[](const httplib::Request&, httplib::Response& res)
			{
				auto result = runWithMiddleware(10,
												[]() -> std::string
												{
													json obj;
													obj["middleware_count"] = 10;
													return obj.dump();
												});
				res.set_content(result, "application/json");
			});

	// /sync-middleware — 复用同样的调用链（cpp-httplib 无异步/同步中间件区分）
	svr.Get("/sync-middleware/3",
			[](const httplib::Request&, httplib::Response& res)
			{
				auto result = runWithMiddleware(3,
												[]() -> std::string
												{
													json obj;
													obj["middleware_count"] = 3;
													obj["type"] = "sync";
													return obj.dump();
												});
				res.set_content(result, "application/json");
			});

	svr.Get("/sync-middleware/10",
			[](const httplib::Request&, httplib::Response& res)
			{
				auto result = runWithMiddleware(10,
												[]() -> std::string
												{
													json obj;
													obj["middleware_count"] = 10;
													obj["type"] = "sync";
													return obj.dump();
												});
				res.set_content(result, "application/json");
			});

	svr.listen("0.0.0.0", 8086);
}
