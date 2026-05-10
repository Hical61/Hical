#include "cinatra/coro_http_server.hpp"
#include "iguana/json_reader.hpp"
#include "iguana/json_writer.hpp"
#include "iguana/reflection.hpp"
#include <functional>
#include <string>

using namespace cinatra;

// DTO 定义 + iguana 反射
struct UserDTO
{
    std::string name;
    int age{0};
    std::string email;
};
REFLECTION(UserDTO, name, age, email);

struct StatusDTO
{
    std::string status;
    std::string framework;
};
REFLECTION(StatusDTO, status, framework);

struct UserResponseDTO
{
    std::string userId;
    std::string name;
};
REFLECTION(UserResponseDTO, userId, name);

struct MiddlewareDTO
{
    int middleware_count{0};
};
REFLECTION(MiddlewareDTO, middleware_count);

// 模拟 N 层空操作中间件调用链（与 Crow/Oat++/cpp-httplib 方案一致）
std::string runWithMiddleware(int layers, std::function<std::string()> handler)
{
    auto chain = std::move(handler);
    for (int i = 0; i < layers; ++i)
    {
        chain = [prev = std::move(chain)]() -> std::string
        {
            return prev();
        };
    }
    return chain();
}

int main()
{
    coro_http_server server(4, 8087);

    // Hello World
    server.set_http_handler<GET>("/",
        [](coro_http_request& req, coro_http_response& res)
        {
            res.set_status_and_content(status_type::ok, "Hello, World!");
        });

    // JSON 响应
    server.set_http_handler<GET>("/api/status",
        [](coro_http_request& req, coro_http_response& res)
        {
            StatusDTO dto{"running", "cinatra"};
            std::string json;
            iguana::to_json(dto, json);
            res.add_header("Content-Type", "application/json");
            res.set_status_and_content(status_type::ok, std::move(json));
        });

    // JSON 反序列化 + 序列化（Echo）
    server.set_http_handler<POST>("/api/echo",
        [](coro_http_request& req, coro_http_response& res)
        {
            auto body = req.get_body();
            UserDTO user;
            iguana::from_json(user, body);
            std::string json;
            iguana::to_json(user, json);
            res.add_header("Content-Type", "application/json");
            res.set_status_and_content(status_type::ok, std::move(json));
        });

    // 路径参数
    server.set_http_handler<GET>("/users/:id",
        [](coro_http_request& req, coro_http_response& res)
        {
            auto id = req.get_query_value("id");
            UserResponseDTO dto{std::string(id), "User " + std::string(id)};
            std::string json;
            iguana::to_json(dto, json);
            res.add_header("Content-Type", "application/json");
            res.set_status_and_content(status_type::ok, std::move(json));
        });

    // ============ 中间件链测试端点 ============

    // 无中间件
    server.set_http_handler<GET>("/middleware/0",
        [](coro_http_request& req, coro_http_response& res)
        {
            MiddlewareDTO dto{0};
            std::string json;
            iguana::to_json(dto, json);
            res.add_header("Content-Type", "application/json");
            res.set_status_and_content(status_type::ok, std::move(json));
        });

    // 3 层空操作中间件
    server.set_http_handler<GET>("/middleware/3",
        [](coro_http_request& req, coro_http_response& res)
        {
            auto result = runWithMiddleware(3, []() -> std::string
                {
                    MiddlewareDTO dto{3};
                    std::string json;
                    iguana::to_json(dto, json);
                    return json;
                });
            res.add_header("Content-Type", "application/json");
            res.set_status_and_content(status_type::ok, std::move(result));
        });

    // 10 层空操作中间件
    server.set_http_handler<GET>("/middleware/10",
        [](coro_http_request& req, coro_http_response& res)
        {
            auto result = runWithMiddleware(10, []() -> std::string
                {
                    MiddlewareDTO dto{10};
                    std::string json;
                    iguana::to_json(dto, json);
                    return json;
                });
            res.add_header("Content-Type", "application/json");
            res.set_status_and_content(status_type::ok, std::move(result));
        });

    // /sync-middleware — 复用同样的调用链（Cinatra 无异步/同步中间件区分）
    server.set_http_handler<GET>("/sync-middleware/3",
        [](coro_http_request& req, coro_http_response& res)
        {
            auto result = runWithMiddleware(3, []() -> std::string
                {
                    MiddlewareDTO dto{3};
                    std::string json;
                    iguana::to_json(dto, json);
                    return json;
                });
            res.add_header("Content-Type", "application/json");
            res.set_status_and_content(status_type::ok, std::move(result));
        });

    server.set_http_handler<GET>("/sync-middleware/10",
        [](coro_http_request& req, coro_http_response& res)
        {
            auto result = runWithMiddleware(10, []() -> std::string
                {
                    MiddlewareDTO dto{10};
                    std::string json;
                    iguana::to_json(dto, json);
                    return json;
                });
            res.add_header("Content-Type", "application/json");
            res.set_status_and_content(status_type::ok, std::move(result));
        });

    server.sync_start();
    return 0;
}
