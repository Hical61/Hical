#include <drogon/drogon.h>
#include <json/json.h>
#include <string>

using namespace drogon;

int main()
{
	app().setLogLevel(trantor::Logger::kWarn);
	app().setThreadNum(4);
	app().addListener("0.0.0.0", 8083);

	// Hello World
	app().registerHandler("/",
						  [](const HttpRequestPtr&, std::function<void(const HttpResponsePtr&)>&& callback)
						  {
							  auto resp = HttpResponse::newHttpResponse();
							  resp->setBody("Hello, World!");
							  callback(resp);
						  },
						  {Get});

	// JSON 响应
	app().registerHandler("/api/status",
						  [](const HttpRequestPtr&, std::function<void(const HttpResponsePtr&)>&& callback)
						  {
							  Json::Value obj;
							  obj["status"] = "running";
							  obj["framework"] = "drogon";
							  auto resp = HttpResponse::newHttpJsonResponse(obj);
							  callback(resp);
						  },
						  {Get});

	// JSON 反序列化 + 序列化
	app().registerHandler("/api/echo",
						  [](const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback)
						  {
							  auto bodyPtr = req->getJsonObject();
							  if (!bodyPtr)
							  {
								  auto resp = HttpResponse::newHttpResponse();
								  resp->setStatusCode(k400BadRequest);
								  resp->setBody("invalid json");
								  callback(resp);
								  return;
							  }
							  auto resp = HttpResponse::newHttpJsonResponse(*bodyPtr);
							  callback(resp);
						  },
						  {Post});

	// 路径参数
	app().registerHandler(
		"/users/{id}",
		[](const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback, std::string id)
		{
			Json::Value obj;
			obj["userId"] = id;
			obj["name"] = "User " + id;
			auto resp = HttpResponse::newHttpJsonResponse(obj);
			callback(resp);
		},
		{Get});

	app().run();
}
