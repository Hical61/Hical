#include "core/HttpServer.h"
#include <gtest/gtest.h>
#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <atomic>
#include <chrono>
#include <string>
#include <thread>

using namespace hical;
namespace beast = boost::beast;
namespace http = beast::http;
using boost::asio::ip::tcp;

// 辅助：发送 HTTP 请求并获取响应
std::pair<unsigned int, std::string>
httpGet(const std::string& host, uint16_t port, const std::string& target)
{
    boost::asio::io_context ioCtx;
    tcp::socket socket(ioCtx);
    socket.connect(tcp::endpoint(
        boost::asio::ip::make_address(host), port));

    http::request<http::string_body> req(http::verb::get, target, 11);
    req.set(http::field::host, host);
    http::write(socket, req);

    beast::flat_buffer buffer;
    http::response<http::string_body> res;
    http::read(socket, buffer, res);

    socket.shutdown(tcp::socket::shutdown_both);
    return {res.result_int(), res.body()};
}

std::pair<unsigned int, std::string>
httpPost(const std::string& host, uint16_t port,
         const std::string& target, const std::string& body)
{
    boost::asio::io_context ioCtx;
    tcp::socket socket(ioCtx);
    socket.connect(tcp::endpoint(
        boost::asio::ip::make_address(host), port));

    http::request<http::string_body> req(http::verb::post, target, 11);
    req.set(http::field::host, host);
    req.body() = body;
    req.prepare_payload();
    http::write(socket, req);

    beast::flat_buffer buffer;
    http::response<http::string_body> res;
    http::read(socket, buffer, res);

    socket.shutdown(tcp::socket::shutdown_both);
    return {res.result_int(), res.body()};
}

// 测试 HttpServer 基本启动
TEST(HttpServerTest, StartAndStop)
{
    HttpServer server(0);  // 端口 0 = 系统分配
    server.router().get("/", [](const HttpRequest&) -> HttpResponse {
        return HttpResponse::ok("hello");
    });

    // 在后台线程启动
    std::thread serverThread([&server]() {
        server.start();
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    EXPECT_TRUE(server.isRunning());

    server.stop();
    serverThread.join();
}

// 测试 HttpServer GET 请求
TEST(HttpServerTest, GetRequest)
{
    // 使用固定端口（测试）
    uint16_t port = 18080;
    HttpServer server(port);

    server.router().get("/api/hello",
                        [](const HttpRequest&) -> HttpResponse {
                            return HttpResponse::ok("Hello from hical!");
                        });

    std::thread serverThread([&server]() {
        server.start();
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    auto [status, body] = httpGet("127.0.0.1", port, "/api/hello");
    EXPECT_EQ(status, 200);
    EXPECT_EQ(body, "Hello from hical!");

    server.stop();
    serverThread.join();
}

// 测试 HttpServer POST 请求
TEST(HttpServerTest, PostRequest)
{
    uint16_t port = 18081;
    HttpServer server(port);

    server.router().post("/api/echo",
                         [](const HttpRequest& req) -> HttpResponse {
                             return HttpResponse::ok(req.body());
                         });

    std::thread serverThread([&server]() {
        server.start();
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    auto [status, body] = httpPost(
        "127.0.0.1", port, "/api/echo", "Echo this!");
    EXPECT_EQ(status, 200);
    EXPECT_EQ(body, "Echo this!");

    server.stop();
    serverThread.join();
}

// 测试 404
TEST(HttpServerTest, NotFound)
{
    uint16_t port = 18082;
    HttpServer server(port);

    std::thread serverThread([&server]() {
        server.start();
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    auto [status, body] = httpGet("127.0.0.1", port, "/nonexistent");
    EXPECT_EQ(status, 404);

    server.stop();
    serverThread.join();
}

// 测试路径参数
TEST(HttpServerTest, PathParam)
{
    uint16_t port = 18083;
    HttpServer server(port);

    server.router().get("/users/{id}",
                        [](const HttpRequest& req) -> HttpResponse {
                            return HttpResponse::ok("User " + req.param("id"));
                        });

    std::thread serverThread([&server]() {
        server.start();
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    auto [status, body] = httpGet("127.0.0.1", port, "/users/42");
    EXPECT_EQ(status, 200);
    EXPECT_EQ(body, "User 42");

    server.stop();
    serverThread.join();
}

// 测试中间件
TEST(HttpServerTest, Middleware)
{
    uint16_t port = 18084;
    HttpServer server(port);

    server.use([](const HttpRequest& req, MiddlewareNext next)
                   -> Awaitable<HttpResponse> {
        auto res = co_await next(req);
        res.setHeader("X-Powered-By", "hical");
        co_return res;
    });

    server.router().get("/api/test",
                        [](const HttpRequest&) -> HttpResponse {
                            return HttpResponse::ok("test");
                        });

    std::thread serverThread([&server]() {
        server.start();
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // 手动检查响应头
    boost::asio::io_context ioCtx;
    tcp::socket socket(ioCtx);
    socket.connect(tcp::endpoint(
        boost::asio::ip::make_address("127.0.0.1"), port));

    http::request<http::string_body> req(
        http::verb::get, "/api/test", 11);
    req.set(http::field::host, "127.0.0.1");
    http::write(socket, req);

    beast::flat_buffer buffer;
    http::response<http::string_body> res;
    http::read(socket, buffer, res);

    EXPECT_EQ(res.result_int(), 200);
    EXPECT_EQ(std::string(res["X-Powered-By"]), "hical");

    socket.shutdown(tcp::socket::shutdown_both);
    server.stop();
    serverThread.join();
}

// 测试 JSON 响应
TEST(HttpServerTest, JsonResponse)
{
    uint16_t port = 18085;
    HttpServer server(port);

    server.router().get("/api/status",
                        [](const HttpRequest&) -> HttpResponse {
                            return HttpResponse::json(
                                {{"status", "ok"}, {"version", "0.2.0"}});
                        });

    std::thread serverThread([&server]() {
        server.start();
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    boost::asio::io_context ioCtx;
    tcp::socket socket(ioCtx);
    socket.connect(tcp::endpoint(
        boost::asio::ip::make_address("127.0.0.1"), port));

    http::request<http::string_body> req(
        http::verb::get, "/api/status", 11);
    req.set(http::field::host, "127.0.0.1");
    http::write(socket, req);

    beast::flat_buffer buffer;
    http::response<http::string_body> res;
    http::read(socket, buffer, res);

    EXPECT_EQ(res.result_int(), 200);
    EXPECT_EQ(std::string(res[http::field::content_type]),
              "application/json");

    auto json = boost::json::parse(res.body());
    EXPECT_EQ(json.at("status").as_string(), "ok");

    socket.shutdown(tcp::socket::shutdown_both);
    server.stop();
    serverThread.join();
}
