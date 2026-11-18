/**
 * @file main.cpp
 * @brief 用已安装的 Hical 包启动房间示例
 */

#include "app.h"
#include "core/HttpServer.h"
#include "core/Log.h"
#include "core/OpenApiDocument.h"
#include <boost/asio/post.hpp>
#include <charconv>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

namespace
{
	namespace contract = hical::example::contract;
	namespace openapi = hical::meta::openapi;

	std::string roomOpenApi(openapi::OpenApiDocument& document)
	{
		auto json = boost::json::parse(document.generateString());
		auto& parameters = json.at("paths").at(contract::kGetRoomPath).at("get").at("parameters").as_array();
		for (auto& parameter : parameters)
		{
			const auto& name = parameter.at("name").as_string();
			if (std::string_view(name.data(), name.size()) == contract::RoomId::kParam)
			{
				// ParamInfo 只记录基本类型，在启动时把解析器使用的取值范围补进文档。
				auto& schema = parameter.at("schema").as_object();
				schema["format"] = "int32";
				schema["minimum"] = contract::RoomId::kMinimum;
				schema["maximum"] = contract::RoomId::kMaximum;
				return boost::json::serialize(json);
			}
		}
		throw std::logic_error("OpenAPI 未登记房间编号参数");
	}
} // namespace

int main(int argc, char** argv)
{
	if (argc > 2)
	{
		HICAL_LOG_ERROR("用法：api_contract_server [端口]");
		return 1;
	}
	std::uint16_t port = 0;
	if (argc == 2)
	{
		const std::string_view text(argv[1]);
		const auto end = text.data() + text.size();
		const auto [next, error] = std::from_chars(text.data(), end, port);
		if (error != std::errc {} || next != end)
		{
			HICAL_LOG_ERROR("端口必须是 0 到 65535 之间的整数");
			return 1;
		}
	}

	try
	{
		contract::RoomStore store;
		hical::HttpServer server;
		server.listen("127.0.0.1", port);
		auto registry = std::make_shared<openapi::OpenApiRegistry>();
		contract::registerRoomEndpoints(server.router(), *registry, store);
		openapi::OpenApiDocument document(registry, {.title = "Hical Room API"});
		server.router().get("/openapi.json",
							[json = roomOpenApi(document)](const hical::HttpRequest&)
							{
								hical::HttpResponse response;
								response.setBody(json, "application/json; charset=utf-8");
								return response;
							});
		// start() 绑定端口后才会执行这个回调，测试脚本从标准输出读取端口。
		boost::asio::post(server.ioContext(),
						  [&server]
						  {
							  std::cout << "PORT=" << server.port() << std::endl;
						  });
		server.start();
	}
	catch (const std::exception& error)
	{
		HICAL_LOG_ERROR("房间服务启动失败：{}", error.what());
		return 1;
	}
}
