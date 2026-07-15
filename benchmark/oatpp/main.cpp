#include "oatpp/web/server/HttpConnectionHandler.hpp"
#include "oatpp/web/server/api/ApiController.hpp"
#include "oatpp/network/Server.hpp"
#include "oatpp/network/tcp/server/ConnectionProvider.hpp"
#include "oatpp/parser/json/mapping/ObjectMapper.hpp"
#include "oatpp/core/macro/codegen.hpp"
#include <functional>
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

class MiddlewareDTO : public oatpp::DTO
{
	DTO_INIT(MiddlewareDTO, DTO)
	DTO_FIELD(Int32, middleware_count);
};

class UserResponseDTO : public oatpp::DTO
{
	DTO_INIT(UserResponseDTO, DTO)
	DTO_FIELD(String, userId);
	DTO_FIELD(String, name);
};

#include OATPP_CODEGEN_END(DTO)

// ---- 空操作中间件调用链（handler 内部构造，与 Crow 方案一致）----
// 与其他框架保持相同的测试语义：模拟 N 层洋葱模型开销
oatpp::Object<MiddlewareDTO> runWithMiddleware(int layers, std::function<oatpp::Object<MiddlewareDTO>()> handler)
{
	auto chain = std::move(handler);
	for (int i = 0; i < layers; ++i)
	{
		chain = [prev = std::move(chain)]() -> oatpp::Object<MiddlewareDTO>
		{
			// 空操作，直接透传
			return prev();
		};
	}
	return chain();
}

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

	// 无中间件
	ENDPOINT("GET", "/middleware/0", mw0)
	{
		auto dto = MiddlewareDTO::createShared();
		dto->middleware_count = 0;
		return createDtoResponse(Status::CODE_200, dto);
	}

	// 3 层空操作中间件
	ENDPOINT("GET", "/middleware/3", mw3)
	{
		auto result = runWithMiddleware(3,
										[]() -> oatpp::Object<MiddlewareDTO>
										{
											auto dto = MiddlewareDTO::createShared();
											dto->middleware_count = 3;
											return dto;
										});
		return createDtoResponse(Status::CODE_200, result);
	}

	// 10 层空操作中间件
	ENDPOINT("GET", "/middleware/10", mw10)
	{
		auto result = runWithMiddleware(10,
										[]() -> oatpp::Object<MiddlewareDTO>
										{
											auto dto = MiddlewareDTO::createShared();
											dto->middleware_count = 10;
											return dto;
										});
		return createDtoResponse(Status::CODE_200, result);
	}

	// /sync-filter — 模拟同步函数调用链
	ENDPOINT("GET", "/sync-filter/3", syncMw3)
	{
		auto result = runWithMiddleware(3,
										[]() -> oatpp::Object<MiddlewareDTO>
										{
											auto dto = MiddlewareDTO::createShared();
											dto->middleware_count = 3;
											return dto;
										});
		return createDtoResponse(Status::CODE_200, result);
	}

	ENDPOINT("GET", "/sync-filter/10", syncMw10)
	{
		auto result = runWithMiddleware(10,
										[]() -> oatpp::Object<MiddlewareDTO>
										{
											auto dto = MiddlewareDTO::createShared();
											dto->middleware_count = 10;
											return dto;
										});
		return createDtoResponse(Status::CODE_200, result);
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
