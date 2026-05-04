#include "core/HttpServer.h"
#include "core/Coroutine.h"
#include "core/MetaJson.h"
#include <boost/json.hpp>

using namespace hical;
namespace json = boost::json;

struct UserDTO
{
    std::string name;
    int age{0};
    std::string email;

    HICAL_JSON(UserDTO, name, age, email)
};

int main()
{
    HttpServer server(8080, 4);

    // Hello World
    server.router().get("/",
        [](const HttpRequest&) -> HttpResponse {
            return HttpResponse::ok("Hello, World!");
        });

    // JSON 响应
    server.router().get("/api/status",
        [](const HttpRequest&) -> HttpResponse {
            json::object obj;
            obj["status"] = "running";
            obj["framework"] = "hical";
            return HttpResponse::json(json::value(std::move(obj)));
        });

    // JSON 反序列化 + 序列化
    server.router().post("/api/echo",
        [](const HttpRequest& req) -> Awaitable<HttpResponse> {
            auto user = req.readJson<UserDTO>();
            co_return HttpResponse::json(meta::toJson(user));
        });

    // 路径参数
    server.router().get("/users/{id}",
        [](const HttpRequest& req) -> HttpResponse {
            json::object obj;
            obj["userId"] = req.param("id");
            obj["name"] = "User " + req.param("id");
            return HttpResponse::json(json::value(std::move(obj)));
        });

    server.start();
}
