/**
 * @file main.cpp
 * @brief 用 oatpp DTO 和 Swagger 搭建房间接口对照服务
 */

#include "oatpp-swagger/Generator.hpp"
#include "oatpp/core/base/Environment.hpp"
#include "oatpp/network/Server.hpp"
#include "oatpp/network/tcp/server/ConnectionProvider.hpp"
#include "oatpp/parser/json/mapping/ObjectMapper.hpp"
#include "oatpp/web/server/HttpConnectionHandler.hpp"
#include "oatpp/web/server/HttpRouter.hpp"
#include "oatpp/web/server/api/ApiController.hpp"
#include <charconv>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
	using JsonMapper = oatpp::parser::json::mapping::ObjectMapper;
	using JsonDeserializer = oatpp::parser::json::mapping::Deserializer;
	constexpr auto kJsonMediaType = "application/json";
	constexpr std::int32_t kMinCapacity = 1;
	constexpr std::int32_t kMaxCapacity = 100;
	constexpr std::int32_t kMinId = 1;
	constexpr std::int32_t kMaxId = std::numeric_limits<std::int32_t>::max();

#include OATPP_CODEGEN_BEGIN(DTO)

	class CreateRoom : public oatpp::DTO
	{
		DTO_INIT(CreateRoom, DTO)
		DTO_FIELD(String, roomName, "room_name");
		DTO_FIELD(Int32, capacity);

		DTO_FIELD_INFO(roomName)
		{
			info->required = true;
		}

		DTO_FIELD_INFO(capacity)
		{
			info->required = true;
		}
	};

	class Room : public oatpp::DTO
	{
		DTO_INIT(Room, DTO)
		DTO_FIELD(Int32, id);
		DTO_FIELD(String, roomName, "room_name");
		DTO_FIELD(Int32, capacity);

		DTO_FIELD_INFO(id)
		{
			info->required = true;
		}

		DTO_FIELD_INFO(roomName)
		{
			info->required = true;
		}

		DTO_FIELD_INFO(capacity)
		{
			info->required = true;
		}
	};

	class RoomList : public oatpp::DTO
	{
		DTO_INIT(RoomList, DTO)
		DTO_FIELD(List<Object<Room>>, rooms);
		DTO_FIELD(Int32, total);

		DTO_FIELD_INFO(rooms)
		{
			info->required = true;
		}

		DTO_FIELD_INFO(total)
		{
			info->required = true;
		}
	};

	class Error : public oatpp::DTO
	{
		DTO_INIT(Error, DTO)
		DTO_FIELD(String, message);

		DTO_FIELD_INFO(message)
		{
			info->required = true;
		}
	};

#include OATPP_CODEGEN_END(DTO)

	template <typename Integer>
	bool parseInteger(std::string_view text, Integer& value)
	{
		if (text.empty())
		{
			return false;
		}
		const auto end = text.data() + text.size();
		const auto result = std::from_chars(text.data(), end, value);
		return result.ec == std::errc {} && result.ptr == end;
	}

	// oatpp 会先解析再转成 Int32，这里直接检查 JSON 原文中的整数写法和取值范围。
	oatpp::Void readInt32(JsonDeserializer*, oatpp::parser::Caret& caret, const JsonDeserializer::Type* type)
	{
		if (caret.isAtText("null", true))
		{
			return oatpp::Void(type);
		}
		const std::string_view remaining(caret.getCurrData(), caret.getDataSize() - caret.getPosition());
		const auto token = remaining.substr(0, remaining.find_first_of(",]} \t\n\r"));
		const auto digits = token.starts_with('-') ? token.substr(1) : token;
		std::int32_t value {};
		if ((digits.size() > 1 && digits.front() == '0') || !parseInteger(token, value))
		{
			caret.setError("需要可表示的 int32 整数");
			return oatpp::Void(type);
		}
		caret.setPosition(caret.getPosition() + token.size());
		return oatpp::Int32(value);
	}

	bool hasJsonContentType(const oatpp::String& header)
	{
		if (!header)
		{
			return false;
		}
		std::string_view mediaType(*header);
		mediaType = mediaType.substr(0, mediaType.find(';'));
		const auto start = mediaType.find_first_not_of(" \t");
		if (start == std::string_view::npos)
		{
			return false;
		}
		mediaType = mediaType.substr(start, mediaType.find_last_not_of(" \t") - start + 1);
		return mediaType == kJsonMediaType
			   || oatpp::String(mediaType.data(), mediaType.size()).equalsCI_ASCII(kJsonMediaType);
	}

#include OATPP_CODEGEN_BEGIN(ApiController)

	class RoomsController : public oatpp::web::server::api::ApiController
	{
	public:
		explicit RoomsController(const std::shared_ptr<JsonMapper>& mapper) : ApiController(mapper)
		{
		}

		ENDPOINT_INFO(createRoom)
		{
			info->tags = {"Rooms"};
			info->body.required = true;
			info->addConsumes<Object<CreateRoom>>(kJsonMediaType);
			info->addResponse<Object<Room>>(Status::CODE_201, kJsonMediaType);
			info->addResponse<Object<Error>>(Status::CODE_400, kJsonMediaType);
		}

		ENDPOINT("POST", "/rooms", createRoom, REQUEST(std::shared_ptr<IncomingRequest>, request))
		{
			if (!hasJsonContentType(request->getHeader(Header::CONTENT_TYPE)))
			{
				return error(Status::CODE_400, "需要 application/json 请求体");
			}
			const auto body = request->readBodyToString();
			oatpp::parser::Caret caret(body);
			Object<CreateRoom> input;
			try
			{
				input = getDefaultObjectMapper()->readFromCaret<Object<CreateRoom>>(caret);
			}
			catch (const std::runtime_error&)
			{
				// 必填字段显式传 null 时，oatpp 会抛异常。
				return error(Status::CODE_400, "房间数据无效");
			}
			caret.skipBlankChars();
			if (caret.hasError() || caret.canContinue() || !input || !input->roomName || input->roomName->empty()
				|| !input->capacity || *input->capacity < kMinCapacity || *input->capacity > kMaxCapacity)
			{
				return error(Status::CODE_400, "房间数据无效");
			}

			auto room = Room::createShared();
			room->roomName = input->roomName;
			room->capacity = input->capacity;
			{
				std::lock_guard lock(mutex_);
				if (rooms_.size() == kMaxId)
				{
					return error(Status::CODE_500, "房间数量已达上限");
				}
				room->id = static_cast<std::int32_t>(rooms_.size() + 1);
				rooms_.push_back(room);
			}
			return createDtoResponse(Status::CODE_201, room);
		}

		ENDPOINT_INFO(getRoom)
		{
			info->tags = {"Rooms"};
			info->addResponse<Object<Room>>(Status::CODE_200, kJsonMediaType);
			info->addResponse<Object<Error>>(Status::CODE_400, kJsonMediaType);
			info->addResponse<Object<Error>>(Status::CODE_404, kJsonMediaType);
		}

		ENDPOINT("GET", "/rooms/{id}", getRoom, PATH(String, id))
		{
			std::int32_t number {};
			if (!parseInteger(std::string_view(*id), number) || number < kMinId)
			{
				return error(Status::CODE_400, "房间 ID 必须是正 int32 整数");
			}

			Object<Room> room;
			{
				std::lock_guard lock(mutex_);
				const auto index = static_cast<std::size_t>(number - kMinId);
				if (index < rooms_.size())
				{
					room = rooms_[index];
				}
			}
			return room ? createDtoResponse(Status::CODE_200, room) : error(Status::CODE_404, "房间不存在");
		}

		ENDPOINT_INFO(listRooms)
		{
			info->tags = {"Rooms"};
			info->addResponse<Object<RoomList>>(Status::CODE_200, kJsonMediaType);
		}

		ENDPOINT("GET", "/rooms", listRooms)
		{
			auto list = RoomList::createShared();
			list->rooms = List<Object<Room>>::createShared();
			{
				std::lock_guard lock(mutex_);
				list->rooms->assign(rooms_.begin(), rooms_.end());
				list->total = static_cast<std::int32_t>(rooms_.size());
			}
			return createDtoResponse(Status::CODE_200, list);
		}

		// 先生成文档，再挂载这个路由。这样文档里只有三条房间路由。
		void setOpenApiDocument(oatpp::String document)
		{
			openApiDocument_ = std::move(document);
		}

		ENDPOINT("GET", "/openapi.json", openApi)
		{
			auto response = createResponse(Status::CODE_200, openApiDocument_);
			response->putHeader(Header::CONTENT_TYPE, kJsonMediaType);
			return response;
		}

	private:
		std::shared_ptr<OutgoingResponse> error(const Status& status, const char* message)
		{
			auto body = Error::createShared();
			body->message = message;
			return createDtoResponse(status, body);
		}

		std::mutex mutex_;
		std::vector<Object<Room>> rooms_;
		oatpp::String openApiDocument_;
	};

#include OATPP_CODEGEN_END(ApiController)

	oatpp::String generateOpenApi(RoomsController& controller)
	{
		oatpp::web::server::api::Endpoints businessEndpoints;
		for (const auto& endpoint : controller.getEndpoints().list)
		{
			if (endpoint->info()->path != "/openapi.json")
			{
				businessEndpoints.append(endpoint);
			}
		}
		auto info = oatpp::swagger::DocumentInfo::Builder().setTitle("Rooms").setVersion("1.0.0").build();
		oatpp::swagger::Generator generator(std::make_shared<oatpp::swagger::Generator::Config>());
		auto document = generator.generateDocument(info, businessEndpoints);
		auto& input = document->components->schemas["CreateRoom"]->properties;
		input["capacity"]->minimum = kMinCapacity;
		input["capacity"]->maximum = kMaxCapacity;
		// oatpp-swagger 1.3.1 的 Schema 模型没有 minLength，用正则检查非空，换行也算字符。
		input["room_name"]->pattern = "^[\\s\\S]+$";
		const auto idSchema = document->paths["/rooms/{id}"]->operationGet->parameters->front()->schema;
		// PATH(String) 把参数原文交给服务校验，文档里仍声明接口接受的是整数。
		idSchema->type = "integer";
		idSchema->format = "int32";
		idSchema->minimum = kMinId;
		idSchema->maximum = kMaxId;

		auto serializer = oatpp::parser::json::mapping::Serializer::Config::createShared();
		serializer->includeNullFields = false;
		return JsonMapper::createShared(serializer, JsonDeserializer::Config::createShared())->writeToString(document);
	}

	void serve(std::uint16_t port)
	{
		auto mapper = JsonMapper::createShared();
		mapper->getDeserializer()->setDeserializerMethod(oatpp::Int32::Class::CLASS_ID, readInt32);
		auto controller = std::make_shared<RoomsController>(mapper);
		controller->setOpenApiDocument(generateOpenApi(*controller));
		auto router = oatpp::web::server::HttpRouter::createShared();
		router->addController(controller);
		auto handler = oatpp::web::server::HttpConnectionHandler::createShared(router);
		auto provider = oatpp::network::tcp::server::ConnectionProvider::createShared(
			{"127.0.0.1", port, oatpp::network::Address::IP_4});
		oatpp::network::Server server(provider, handler);
		std::cout << "PORT=" << provider->getProperty("port").toString()->c_str() << std::endl;
		server.run();
	}
} // namespace

int main(int argc, char* argv[])
{
	std::uint16_t port {};
	if (argc > 2 || (argc == 2 && !parseInteger(std::string_view(argv[1]), port)))
	{
		std::cerr << "用法 oatpp_room_contract [端口]\n";
		return 1;
	}
	oatpp::base::Environment::init();
	int exitCode = 0;
	try
	{
		serve(port);
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		exitCode = 1;
	}
	oatpp::base::Environment::destroy();
	return exitCode;
}
