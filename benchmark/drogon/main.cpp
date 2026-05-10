#include <drogon/drogon.h>
#include <json/json.h>
#include <string>

using namespace drogon;

// 空操作 Filter，用于中间件层数测试
class Filter1 : public HttpFilter<Filter1>
{
public:
    void doFilter(const HttpRequestPtr&, FilterCallback&&, FilterChainCallback&& nextCb) override
    {
        nextCb();
    }
};

class Filter2 : public HttpFilter<Filter2>
{
public:
    void doFilter(const HttpRequestPtr&, FilterCallback&&, FilterChainCallback&& nextCb) override
    {
        nextCb();
    }
};

class Filter3 : public HttpFilter<Filter3>
{
public:
    void doFilter(const HttpRequestPtr&, FilterCallback&&, FilterChainCallback&& nextCb) override
    {
        nextCb();
    }
};

class Filter4 : public HttpFilter<Filter4>
{
public:
    void doFilter(const HttpRequestPtr&, FilterCallback&&, FilterChainCallback&& nextCb) override
    {
        nextCb();
    }
};

class Filter5 : public HttpFilter<Filter5>
{
public:
    void doFilter(const HttpRequestPtr&, FilterCallback&&, FilterChainCallback&& nextCb) override
    {
        nextCb();
    }
};

class Filter6 : public HttpFilter<Filter6>
{
public:
    void doFilter(const HttpRequestPtr&, FilterCallback&&, FilterChainCallback&& nextCb) override
    {
        nextCb();
    }
};

class Filter7 : public HttpFilter<Filter7>
{
public:
    void doFilter(const HttpRequestPtr&, FilterCallback&&, FilterChainCallback&& nextCb) override
    {
        nextCb();
    }
};

class Filter8 : public HttpFilter<Filter8>
{
public:
    void doFilter(const HttpRequestPtr&, FilterCallback&&, FilterChainCallback&& nextCb) override
    {
        nextCb();
    }
};

class Filter9 : public HttpFilter<Filter9>
{
public:
    void doFilter(const HttpRequestPtr&, FilterCallback&&, FilterChainCallback&& nextCb) override
    {
        nextCb();
    }
};

class Filter10 : public HttpFilter<Filter10>
{
public:
    void doFilter(const HttpRequestPtr&, FilterCallback&&, FilterChainCallback&& nextCb) override
    {
        nextCb();
    }
};

int main()
{
    app().setLogLevel(trantor::Logger::kWarn);
    app().setThreadNum(4);
    app().addListener("0.0.0.0", 8083);

    // Hello World
    app().registerHandler(
        "/",
        [](const HttpRequestPtr&, std::function<void(const HttpResponsePtr&)>&& callback)
        {
            auto resp = HttpResponse::newHttpResponse();
            resp->setBody("Hello, World!");
            callback(resp);
        },
        {Get});

    // JSON 响应
    app().registerHandler(
        "/api/status",
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
    app().registerHandler(
        "/api/echo",
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

    // 无中间件
    app().registerHandler(
        "/middleware/0",
        [](const HttpRequestPtr&, std::function<void(const HttpResponsePtr&)>&& callback)
        {
            Json::Value obj;
            obj["middleware_count"] = 0;
            auto resp = HttpResponse::newHttpJsonResponse(obj);
            callback(resp);
        },
        {Get});

    // 3 层空操作中间件
    app().registerHandler(
        "/middleware/3",
        [](const HttpRequestPtr&, std::function<void(const HttpResponsePtr&)>&& callback)
        {
            Json::Value obj;
            obj["middleware_count"] = 3;
            auto resp = HttpResponse::newHttpJsonResponse(obj);
            callback(resp);
        },
        {Get, "Filter1", "Filter2", "Filter3"});

    // 10 层空操作中间件
    app().registerHandler(
        "/middleware/10",
        [](const HttpRequestPtr&, std::function<void(const HttpResponsePtr&)>&& callback)
        {
            Json::Value obj;
            obj["middleware_count"] = 10;
            auto resp = HttpResponse::newHttpJsonResponse(obj);
            callback(resp);
        },
        {Get, "Filter1", "Filter2", "Filter3", "Filter4", "Filter5",
         "Filter6", "Filter7", "Filter8", "Filter9", "Filter10"});

    // /sync-middleware — 复用同样的 Filter 链（Drogon 无异步/同步中间件区分）
    app().registerHandler(
        "/sync-middleware/3",
        [](const HttpRequestPtr&, std::function<void(const HttpResponsePtr&)>&& callback)
        {
            Json::Value obj;
            obj["middleware_count"] = 3;
            obj["type"] = "sync";
            auto resp = HttpResponse::newHttpJsonResponse(obj);
            callback(resp);
        },
        {Get, "Filter1", "Filter2", "Filter3"});

    app().registerHandler(
        "/sync-middleware/10",
        [](const HttpRequestPtr&, std::function<void(const HttpResponsePtr&)>&& callback)
        {
            Json::Value obj;
            obj["middleware_count"] = 10;
            obj["type"] = "sync";
            auto resp = HttpResponse::newHttpJsonResponse(obj);
            callback(resp);
        },
        {Get, "Filter1", "Filter2", "Filter3", "Filter4", "Filter5",
         "Filter6", "Filter7", "Filter8", "Filter9", "Filter10"});

    app().run();
}
