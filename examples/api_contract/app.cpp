/**
 * @file app.cpp
 * @brief 注册房间接口的同步处理函数和 OpenAPI 文档
 */

#include "app.h"
#include "Endpoint.h"

namespace hical::example::contract
{
	void registerRoomEndpoints(Router& router, openapi::OpenApiRegistry& registry, RoomStore& store)
	{
		registerEndpoint(
			router,
			registry,
			{HttpMethod::hPost, "/rooms", "createRoom", HttpStatusCode::hCreated},
			store,
			+[](RoomStore& state, const CreateRoom& input)
			{
				return state.create(input);
			});
		registerEndpoint(
			router,
			registry,
			{HttpMethod::hGet, kGetRoomPath, "getRoom", HttpStatusCode::hOk},
			store,
			+[](RoomStore& state, RoomId id)
			{
				return state.get(id.value);
			});
		registerEndpoint(
			router,
			registry,
			{HttpMethod::hGet, "/rooms", "listRooms", HttpStatusCode::hOk},
			store,
			+[](RoomStore& state)
			{
				return state.list();
			});
	}
} // namespace hical::example::contract
