#include "cinatra/coro_http_server.hpp"
#include "iguana/json_reader.hpp"
#include "iguana/json_writer.hpp"
#include "iguana/reflection.hpp"
#include <string>

using namespace cinatra;

// DTO 定义 + iguana 反射
struct UserDTO
{
	std::string name;
	int age {0};
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
									 StatusDTO dto {"running", "cinatra"};
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
									 UserResponseDTO dto {std::string(id), "User " + std::string(id)};
									 std::string json;
									 iguana::to_json(dto, json);
									 res.add_header("Content-Type", "application/json");
									 res.set_status_and_content(status_type::ok, std::move(json));
								 });

	server.sync_start();
	return 0;
}
