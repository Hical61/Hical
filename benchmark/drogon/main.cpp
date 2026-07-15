#include <drogon/drogon.h>
#include <json/json.h>
#include <string>

using namespace drogon;

// ============ HttpFilter 空操作（同步过滤链） ============
// HttpFilter 在同 EventLoop 线程上做纯同步递归调用，不创建协程帧
class SyncFilter1 : public HttpFilter<SyncFilter1>
{
public:
	void doFilter(const HttpRequestPtr&, FilterCallback&&, FilterChainCallback&& nextCb) override
	{
		nextCb();
	}
};

class SyncFilter2 : public HttpFilter<SyncFilter2>
{
public:
	void doFilter(const HttpRequestPtr&, FilterCallback&&, FilterChainCallback&& nextCb) override
	{
		nextCb();
	}
};

class SyncFilter3 : public HttpFilter<SyncFilter3>
{
public:
	void doFilter(const HttpRequestPtr&, FilterCallback&&, FilterChainCallback&& nextCb) override
	{
		nextCb();
	}
};

class SyncFilter4 : public HttpFilter<SyncFilter4>
{
public:
	void doFilter(const HttpRequestPtr&, FilterCallback&&, FilterChainCallback&& nextCb) override
	{
		nextCb();
	}
};

class SyncFilter5 : public HttpFilter<SyncFilter5>
{
public:
	void doFilter(const HttpRequestPtr&, FilterCallback&&, FilterChainCallback&& nextCb) override
	{
		nextCb();
	}
};

class SyncFilter6 : public HttpFilter<SyncFilter6>
{
public:
	void doFilter(const HttpRequestPtr&, FilterCallback&&, FilterChainCallback&& nextCb) override
	{
		nextCb();
	}
};

class SyncFilter7 : public HttpFilter<SyncFilter7>
{
public:
	void doFilter(const HttpRequestPtr&, FilterCallback&&, FilterChainCallback&& nextCb) override
	{
		nextCb();
	}
};

class SyncFilter8 : public HttpFilter<SyncFilter8>
{
public:
	void doFilter(const HttpRequestPtr&, FilterCallback&&, FilterChainCallback&& nextCb) override
	{
		nextCb();
	}
};

class SyncFilter9 : public HttpFilter<SyncFilter9>
{
public:
	void doFilter(const HttpRequestPtr&, FilterCallback&&, FilterChainCallback&& nextCb) override
	{
		nextCb();
	}
};

class SyncFilter10 : public HttpFilter<SyncFilter10>
{
public:
	void doFilter(const HttpRequestPtr&, FilterCallback&&, FilterChainCallback&& nextCb) override
	{
		nextCb();
	}
};

// ============ HttpCoroMiddleware 空操作（协程洋葱链） ============
// 每层 co_await next 创建协程帧 + 挂起/恢复，与 Hical AsyncMiddleware 语义对齐
class CoroMw1 : public HttpCoroMiddleware<CoroMw1>
{
public:
	Task<HttpResponsePtr> invoke(const HttpRequestPtr&, MiddlewareNextAwaiter&& next) override
	{
		co_return co_await std::move(next);
	}
};

class CoroMw2 : public HttpCoroMiddleware<CoroMw2>
{
public:
	Task<HttpResponsePtr> invoke(const HttpRequestPtr&, MiddlewareNextAwaiter&& next) override
	{
		co_return co_await std::move(next);
	}
};

class CoroMw3 : public HttpCoroMiddleware<CoroMw3>
{
public:
	Task<HttpResponsePtr> invoke(const HttpRequestPtr&, MiddlewareNextAwaiter&& next) override
	{
		co_return co_await std::move(next);
	}
};

class CoroMw4 : public HttpCoroMiddleware<CoroMw4>
{
public:
	Task<HttpResponsePtr> invoke(const HttpRequestPtr&, MiddlewareNextAwaiter&& next) override
	{
		co_return co_await std::move(next);
	}
};

class CoroMw5 : public HttpCoroMiddleware<CoroMw5>
{
public:
	Task<HttpResponsePtr> invoke(const HttpRequestPtr&, MiddlewareNextAwaiter&& next) override
	{
		co_return co_await std::move(next);
	}
};

class CoroMw6 : public HttpCoroMiddleware<CoroMw6>
{
public:
	Task<HttpResponsePtr> invoke(const HttpRequestPtr&, MiddlewareNextAwaiter&& next) override
	{
		co_return co_await std::move(next);
	}
};

class CoroMw7 : public HttpCoroMiddleware<CoroMw7>
{
public:
	Task<HttpResponsePtr> invoke(const HttpRequestPtr&, MiddlewareNextAwaiter&& next) override
	{
		co_return co_await std::move(next);
	}
};

class CoroMw8 : public HttpCoroMiddleware<CoroMw8>
{
public:
	Task<HttpResponsePtr> invoke(const HttpRequestPtr&, MiddlewareNextAwaiter&& next) override
	{
		co_return co_await std::move(next);
	}
};

class CoroMw9 : public HttpCoroMiddleware<CoroMw9>
{
public:
	Task<HttpResponsePtr> invoke(const HttpRequestPtr&, MiddlewareNextAwaiter&& next) override
	{
		co_return co_await std::move(next);
	}
};

class CoroMw10 : public HttpCoroMiddleware<CoroMw10>
{
public:
	Task<HttpResponsePtr> invoke(const HttpRequestPtr&, MiddlewareNextAwaiter&& next) override
	{
		co_return co_await std::move(next);
	}
};

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

	// ============ 协程洋葱中间件链（与 Hical AsyncMiddleware 同能力层级） ============

	// /middleware/0 — 无中间件基线
	app().registerHandler("/middleware/0",
						  [](const HttpRequestPtr&, std::function<void(const HttpResponsePtr&)>&& callback)
						  {
							  Json::Value obj;
							  obj["middleware_count"] = 0;
							  auto resp = HttpResponse::newHttpJsonResponse(obj);
							  callback(resp);
						  },
						  {Get});

	// /middleware/3 — 3 层协程洋葱中间件
	app().registerHandler("/middleware/3",
						  [](const HttpRequestPtr&, std::function<void(const HttpResponsePtr&)>&& callback)
						  {
							  Json::Value obj;
							  obj["middleware_count"] = 3;
							  auto resp = HttpResponse::newHttpJsonResponse(obj);
							  callback(resp);
						  },
						  {Get, "CoroMw1", "CoroMw2", "CoroMw3"});

	// /middleware/10 — 10 层协程洋葱中间件
	app().registerHandler("/middleware/10",
						  [](const HttpRequestPtr&, std::function<void(const HttpResponsePtr&)>&& callback)
						  {
							  Json::Value obj;
							  obj["middleware_count"] = 10;
							  auto resp = HttpResponse::newHttpJsonResponse(obj);
							  callback(resp);
						  },
						  {Get,
						   "CoroMw1",
						   "CoroMw2",
						   "CoroMw3",
						   "CoroMw4",
						   "CoroMw5",
						   "CoroMw6",
						   "CoroMw7",
						   "CoroMw8",
						   "CoroMw9",
						   "CoroMw10"});

	// ============ 同步过滤链（HttpFilter 纯函数递归，与 Hical SyncMiddleware 同能力层级） ============

	// /sync-filter/3 — 3 层同步 Filter
	app().registerHandler("/sync-filter/3",
						  [](const HttpRequestPtr&, std::function<void(const HttpResponsePtr&)>&& callback)
						  {
							  Json::Value obj;
							  obj["middleware_count"] = 3;
							  obj["type"] = "sync";
							  auto resp = HttpResponse::newHttpJsonResponse(obj);
							  callback(resp);
						  },
						  {Get, "SyncFilter1", "SyncFilter2", "SyncFilter3"});

	// /sync-filter/10 — 10 层同步 Filter
	app().registerHandler("/sync-filter/10",
						  [](const HttpRequestPtr&, std::function<void(const HttpResponsePtr&)>&& callback)
						  {
							  Json::Value obj;
							  obj["middleware_count"] = 10;
							  obj["type"] = "sync";
							  auto resp = HttpResponse::newHttpJsonResponse(obj);
							  callback(resp);
						  },
						  {Get,
						   "SyncFilter1",
						   "SyncFilter2",
						   "SyncFilter3",
						   "SyncFilter4",
						   "SyncFilter5",
						   "SyncFilter6",
						   "SyncFilter7",
						   "SyncFilter8",
						   "SyncFilter9",
						   "SyncFilter10"});

	app().run();
}
