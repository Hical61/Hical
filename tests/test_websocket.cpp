#include "core/HttpServer.h"
#include "core/WebSocket.h"
#include <gtest/gtest.h>
#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/beast/websocket.hpp>
#include <atomic>
#include <chrono>
#include <string>
#include <thread>

using namespace hical;
namespace beast = boost::beast;
namespace ws = beast::websocket;
using boost::asio::ip::tcp;

// 测试 WebSocket Echo
TEST(WebSocketTest, EchoMessage)
{
    uint16_t port = 18090;
    HttpServer server(port);

    server.router().ws("/ws/echo",
        [](const std::string& msg, WebSocketSession& session)
            -> Awaitable<void> {
            co_await session.send("Echo: " + msg);
        });

    std::thread serverThread([&server]() {
        server.start();
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // 客户端 WebSocket 连接
    boost::asio::io_context ioCtx;
    tcp::socket socket(ioCtx);
    socket.connect(tcp::endpoint(
        boost::asio::ip::make_address("127.0.0.1"), port));

    ws::stream<tcp::socket> wsClient(std::move(socket));
    wsClient.handshake("127.0.0.1:" + std::to_string(port), "/ws/echo");

    // 发送消息
    wsClient.write(boost::asio::buffer(std::string("Hello WS")));

    // 接收回复
    beast::flat_buffer buffer;
    wsClient.read(buffer);
    std::string reply = beast::buffers_to_string(buffer.data());

    EXPECT_EQ(reply, "Echo: Hello WS");

    // 关闭
    wsClient.close(ws::close_code::normal);

    server.stop();
    serverThread.join();
}

// 测试 WebSocket 连接回调
TEST(WebSocketTest, ConnectCallback)
{
    uint16_t port = 18091;
    HttpServer server(port);

    server.router().ws("/ws/greet",
        [](const std::string& msg, WebSocketSession& session)
            -> Awaitable<void> {
            co_await session.send("Got: " + msg);
        },
        [](WebSocketSession& session) -> Awaitable<void> {
            co_await session.send("Welcome!");
        });

    std::thread serverThread([&server]() {
        server.start();
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    boost::asio::io_context ioCtx;
    tcp::socket socket(ioCtx);
    socket.connect(tcp::endpoint(
        boost::asio::ip::make_address("127.0.0.1"), port));

    ws::stream<tcp::socket> wsClient(std::move(socket));
    wsClient.handshake("127.0.0.1:" + std::to_string(port), "/ws/greet");

    // 应先收到 Welcome 消息
    beast::flat_buffer buffer;
    wsClient.read(buffer);
    std::string welcome = beast::buffers_to_string(buffer.data());
    EXPECT_EQ(welcome, "Welcome!");

    // 发送消息并接收回复
    buffer.consume(buffer.size());
    wsClient.write(boost::asio::buffer(std::string("test")));
    wsClient.read(buffer);
    std::string reply = beast::buffers_to_string(buffer.data());
    EXPECT_EQ(reply, "Got: test");

    wsClient.close(ws::close_code::normal);
    server.stop();
    serverThread.join();
}

// 测试 WebSocket 未注册路由返回 404
TEST(WebSocketTest, UnregisteredPathFallsToHttp)
{
    uint16_t port = 18092;
    HttpServer server(port);

    // 仅注册 HTTP 路由，不注册 WS 路由
    server.router().get("/", [](const HttpRequest&) -> HttpResponse {
        return HttpResponse::ok("http");
    });

    std::thread serverThread([&server]() {
        server.start();
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // 尝试 WS 握手到未注册路径，服务端应当作普通 HTTP 处理（404）
    // 但由于 Beast 的 ws::is_upgrade 检查，服务端不会升级
    // 直接发 HTTP 请求验证
    boost::asio::io_context ioCtx;
    tcp::socket socket(ioCtx);
    socket.connect(tcp::endpoint(
        boost::asio::ip::make_address("127.0.0.1"), port));

    beast::http::request<beast::http::string_body> req(
        beast::http::verb::get, "/ws/nonexist", 11);
    req.set(beast::http::field::host, "127.0.0.1");
    beast::http::write(socket, req);

    beast::flat_buffer buffer;
    beast::http::response<beast::http::string_body> res;
    beast::http::read(socket, buffer, res);

    EXPECT_EQ(res.result_int(), 404);

    socket.shutdown(tcp::socket::shutdown_both);
    server.stop();
    serverThread.join();
}
