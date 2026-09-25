#include "httplib.h"
#include "json.hpp"
#include <string>

using json = nlohmann::json;

int main()
{
	httplib::Server svr;

	// 必须开 TCP_NODELAY：cpp-httplib 的服务端默认是 false，而它的响应分「响应头」和
	// 「body」两次 write 发出去，Nagle 会把第二个小包压住等前一个包的 ACK，客户端又
	// 延迟 ACK 拖着不回——每个请求稳定卡 ~40ms。不关掉它，压出来的是 Nagle 不是框架。
	svr.set_tcp_nodelay(true);

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

	svr.listen("0.0.0.0", 8086);
}
