/**
 * @file main.cpp
 * @brief libuvcpp 对比压测服务（4 个端点，响应体与其它框架保持一致）
 */

#include <webapp/uvcpp_web_app.h>
#include <webapp/uvcpp_web_json.h>
#include <string>

using namespace uvcpp;

int main()
{
	uvcpp_web_app app;

	// 和其它六家对齐：访问日志默认是开的（会挂一层日志中间件）关掉；日志默认
	// INFO，压测下会刷屏，压到 WARN
	app.set_host("0.0.0.0").set_port(8088).set_access_log(false).set_log_level(log_level::WARN);

	// 4 条事件循环，对齐其它框架的 4 线程。Linux 上走内核分流（SO_REUSEPORT）：
	// 4 条循环各自 bind 同一端口，内核按四元组分——和 Hical 的多 acceptor 同架构。
	// （Windows 才回落成"1 接受者 + n-1 工作循环"的转手模式）
	// 返回值刻意不是链式的：0 成功，UV_EINVAL 参数越界，UV_EBUSY 是启动过之后才调
	int rc = app.set_loops(4);
	if (rc != 0)
	{
		return rc;
	}

	// Hello World
	app.get("/",
			[](uvcpp_web_request& req, uvcpp_web_response& resp, uvcpp_web_next next)
			{
				resp.text("Hello, World!");
				resp.end();
			});

	// JSON 响应：拿 uvcpp_json 构造再序列化，和别家一个口径（不走 json_str 拼字符串，
	// 那样这个场景就没测到 JSON 库）
	app.get("/api/status",
			[](uvcpp_web_request& req, uvcpp_web_response& resp, uvcpp_web_next next)
			{
				uvcpp_json obj;
				obj["status"] = "running";
				obj["framework"] = "libuvcpp";
				resp.json(obj);
				resp.end();
			});

	// JSON 反序列化 + 序列化（Echo）
	app.post("/api/echo",
			 [](uvcpp_web_request& req, uvcpp_web_response& resp, uvcpp_web_next next)
			 {
				 uvcpp_json body;
				 if (!req.json(body))
				 {
					 resp.status(400).text("invalid json");
					 resp.end();
					 return;
				 }
				 resp.json(body);
				 resp.end();
			 });

	// 路径参数：param() 返回指针，参数不在时是 nullptr
	app.get("/users/:id",
			[](uvcpp_web_request& req, uvcpp_web_response& resp, uvcpp_web_next next)
			{
				const std::string* id = req.param("id");
				std::string userId = id ? *id : std::string();
				uvcpp_json obj;
				obj["userId"] = userId;
				obj["name"] = "User " + userId;
				resp.json(obj);
				resp.end();
			});

	// start() 起后台线程跑事件循环，bind 有结果才返回；主线程在 join() 上等
	if (app.start() != 0)
	{
		return 1;
	}
	app.join();
	return 0;
}
