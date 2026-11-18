/**
 * @file manual.cpp
 * @brief 手写 Hical 房间路由和 OpenAPI 文档，供对照测试使用
 */

#include "app.h"
#include "core/OpenApiRegistry.h"
#include "core/Router.h"
#include <charconv>

namespace hical::example::contract
{
	namespace
	{
		namespace openapi = hical::meta::openapi;

		template <typename T>
		HttpResponse jsonResponse(HttpStatusCode status, const T& dto)
		{
			HttpResponse response;
			response.setStatus(status);
			response.setJsonBody(meta::toJson(dto));
			return response;
		}

		boost::json::object ref(std::string_view name)
		{
			return {{"$ref", std::string("#/components/schemas/") + std::string(name)}};
		}

		boost::json::object int32Schema()
		{
			return {{"type", "integer"},
					{"format", "int32"},
					{"minimum", std::numeric_limits<std::int32_t>::min()},
					{"maximum", std::numeric_limits<std::int32_t>::max()}};
		}

		void registerSchemas(openapi::OpenApiRegistry& registry)
		{
			// 字段名和约束需要在手写文档里维护，约束值沿用业务代码里的常量。
			constexpr auto kRoomNameKey = "room_name";
			const auto integer = int32Schema();
			const boost::json::object stringType {{"type", "string"}};
			const boost::json::object inputName {{"type", "string"}, {"minLength", 1}};
			auto inputCapacity = integer;
			inputCapacity["minimum"] = kMinCapacity;
			inputCapacity["maximum"] = kMaxCapacity;
			registry.addSchema("CreateRoom",
							   {{"type", "object"},
								{"required", boost::json::array {kRoomNameKey, "capacity"}},
								{"properties", {{kRoomNameKey, inputName}, {"capacity", inputCapacity}}}});
			registry.addSchema("Room",
							   {{"type", "object"},
								{"required", boost::json::array {"id", kRoomNameKey, "capacity"}},
								{"properties", {{"id", integer}, {kRoomNameKey, stringType}, {"capacity", integer}}}});
			const boost::json::object rooms {{"type", "array"}, {"items", ref("Room")}};
			registry.addSchema("RoomList",
							   {{"type", "object"},
								{"required", boost::json::array {"rooms", "total"}},
								{"properties", {{"rooms", rooms}, {"total", integer}}}});
			registry.addSchema("Error",
							   {{"type", "object"},
								{"required", boost::json::array {"message"}},
								{"properties", {{"message", stringType}}}});
		}

		openapi::RouteApiInfo::ResponseInfo describes(std::string description, std::string_view model)
		{
			return {std::move(description),
					[schema = ref(model)]
					{
						return schema;
					}};
		}

		void registerDocs(openapi::OpenApiRegistry& registry)
		{
			registerSchemas(registry);
			openapi::RouteApiInfo create;
			create.tags = {"Rooms"};
			create.requestBodyRequired = true;
			create.requestBodySchema = []
			{
				return ref("CreateRoom");
			};
			create.responses.emplace(201, describes("成功", "Room"));
			create.responses.emplace(400, describes("请求体不符合契约", "Error"));
			registry.addRoute(HttpMethod::hPost, "/rooms", "createRoom", std::move(create));

			openapi::RouteApiInfo get;
			get.tags = {"Rooms"};
			openapi::builder::pathParam(get, RoomId::kParam, "integer", "正的 32 位房间编号");
			get.responses.emplace(200, describes("成功", "Room"));
			get.responses.emplace(400, describes("房间编号无效", "Error"));
			get.responses.emplace(404, describes("房间不存在", "Error"));
			registry.addRoute(HttpMethod::hGet, std::string(kGetRoomPath), "getRoom", std::move(get));

			openapi::RouteApiInfo list;
			list.tags = {"Rooms"};
			list.responses.emplace(200, describes("成功", "RoomList"));
			registry.addRoute(HttpMethod::hGet, "/rooms", "listRooms", std::move(list));
		}

		HttpResponse createRoom(RoomStore& store, const HttpRequest& request)
		{
			CreateRoom input;
			try
			{
				input = request.readJson<CreateRoom>();
			}
			catch (const std::runtime_error&)
			{
				return jsonResponse(HttpStatusCode::hBadRequest, Error {"请求体不符合契约"});
			}
			return jsonResponse(HttpStatusCode::hCreated, store.create(input));
		}

		HttpResponse getRoom(RoomStore& store, const HttpRequest& request)
		{
			const auto& text = request.param(RoomId::kParam);
			const auto end = text.data() + text.size();
			std::int32_t id = 0;
			const auto [next, error] = std::from_chars(text.data(), end, id);
			if (error != std::errc {} || next != end || id < RoomId::kMinimum)
			{
				return jsonResponse(HttpStatusCode::hBadRequest, Error {"房间编号无效"});
			}
			const auto room = store.get(id);
			return room ? jsonResponse(HttpStatusCode::hOk, *room)
						: jsonResponse(HttpStatusCode::hNotFound, Error {"房间不存在"});
		}
	} // namespace

	void registerRoomEndpoints(Router& router, openapi::OpenApiRegistry& registry, RoomStore& store)
	{
		router.post("/rooms",
					[&store](const HttpRequest& request)
					{
						return createRoom(store, request);
					});
		router.get(std::string(kGetRoomPath),
				   [&store](const HttpRequest& request)
				   {
					   return getRoom(store, request);
				   });
		router.get("/rooms",
				   [&store](const HttpRequest&)
				   {
					   return jsonResponse(HttpStatusCode::hOk, store.list());
				   });
		registerDocs(registry);
	}
} // namespace hical::example::contract
