#include "oatpp/web/server/HttpConnectionHandler.hpp"
#include "oatpp/web/server/api/ApiController.hpp"
#include "oatpp/network/Server.hpp"
#include "oatpp/network/tcp/server/ConnectionProvider.hpp"
#include "oatpp/parser/json/mapping/ObjectMapper.hpp"
#include "oatpp/core/macro/codegen.hpp"
#include <thread>
#include <vector>

// ---- DTO 定义 ----
#include OATPP_CODEGEN_BEGIN(DTO)

class UserDTO : public oatpp::DTO
{
	DTO_INIT(UserDTO, DTO)
	DTO_FIELD(String, name);
	DTO_FIELD(Int32, age);
	DTO_FIELD(String, email);
};

class StatusDTO : public oatpp::DTO
{
	DTO_INIT(StatusDTO, DTO)
	DTO_FIELD(String, status);
	DTO_FIELD(String, framework);
};

class UserResponseDTO : public oatpp::DTO
{
	DTO_INIT(UserResponseDTO, DTO)
	DTO_FIELD(String, userId);
	DTO_FIELD(String, name);
};

#include OATPP_CODEGEN_END(DTO)

// ---- Controller 定义 ----
#include OATPP_CODEGEN_BEGIN(ApiController)

class BenchController : public oatpp::web::server::api::ApiController
{
public:
	BenchController(const std::shared_ptr<ObjectMapper>& objectMapper)
		: oatpp::web::server::api::ApiController(objectMapper)
	{
	}

	// Hello World
	ENDPOINT("GET", "/", hello)
	{
		return createResponse(Status::CODE_200, "Hello, World!");
	}

	// JSON 响应
	ENDPOINT("GET", "/api/status", getStatus)
	{
		auto dto = StatusDTO::createShared();
		dto->status = "running";
		dto->framework = "oatpp";
		return createDtoResponse(Status::CODE_200, dto);
	}

	// JSON 反序列化 + 序列化（Echo）
	ENDPOINT("POST", "/api/echo", echo, BODY_DTO(Object<UserDTO>, body))
	{
		// body 解析失败时 oatpp 会自动返回 400
		return createDtoResponse(Status::CODE_200, body);
	}

	// 路径参数
	ENDPOINT("GET", "/users/{id}", getUser, PATH(String, id))
	{
		auto dto = UserResponseDTO::createShared();
		dto->userId = id;
		dto->name = "User " + *id;
		return createDtoResponse(Status::CODE_200, dto);
	}
};

#include OATPP_CODEGEN_END(ApiController)

int main()
{
	oatpp::base::Environment::init();

	auto objectMapper = oatpp::parser::json::mapping::ObjectMapper::createShared();

	// 每个 Server 实例绑定独立的 ConnectionProvider 会导致端口冲突；
	// oatpp Simple API 的 ConnectionProvider 本身是多线程 accept 的，
	// 用 4 个线程各自驱动同一个 server.run() 的循环即可实现并发处理。
	auto connectionProvider =
		oatpp::network::tcp::server::ConnectionProvider::createShared({"0.0.0.0", 8085, oatpp::network::Address::IP_4});

	auto router = oatpp::web::server::HttpRouter::createShared();

	auto controller = std::make_shared<BenchController>(objectMapper);
	router->addController(controller);

	auto connectionHandler = oatpp::web::server::HttpConnectionHandler::createShared(router);

	oatpp::network::Server server(connectionProvider, connectionHandler);

	OATPP_LOGI("Bench", "Oat++ server running on port 8085");

	// oatpp Simple API 的 Server::run() 不支持多线程调用，
	// 但 ConnectionHandler 内部为每个连接创建独立协程/线程处理。
	// 单线程 accept 循环即可，并发能力由 ConnectionHandler 内部管理。
	server.run();

	oatpp::base::Environment::destroy();
	return 0;
}
