/**
 * @file dto.h
 * @brief 用宏和原生反射两种写法声明房间接口的 DTO
 */

#pragma once

#include "core/MetaJson.h"
#include "core/OpenApiSchema.h"
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace hical::example::contract
{

	inline constexpr std::int32_t kMinCapacity = 1;
	inline constexpr std::int32_t kMaxCapacity = 100;

	struct RoomId
	{
		static constexpr std::string_view kParam = "id";
		static constexpr std::int32_t kMinimum = 1;
		static constexpr auto kMaximum = std::numeric_limits<std::int32_t>::max();

		std::int32_t value;
	};

	struct CreateRoom
	{
#if HICAL_HAS_REFLECTION
		[[= anno::json_rename("room_name"), = anno::json_required, = anno::json_not_empty]]
#endif
			std::string name;
#if HICAL_HAS_REFLECTION
		[[= anno::json_required, = anno::json_min(kMinCapacity), = anno::json_max(kMaxCapacity)]]
#endif
			std::int32_t capacity = 0;

#if !HICAL_HAS_REFLECTION
		HICAL_JSON(CreateRoom,
				   REQUIRED_ALIAS(name, "room_name"),
				   NOT_EMPTY(name),
				   REQUIRED(capacity),
				   MIN(capacity, kMinCapacity),
				   MAX(capacity, kMaxCapacity))
#endif
	};

	struct Room
	{
		std::int32_t id;
#if HICAL_HAS_REFLECTION
		[[= anno::json_rename("room_name")]]
#endif
			std::string name;
		std::int32_t capacity;

		HICAL_JSON(Room, id, ALIAS(name, "room_name"), capacity)
	};

	struct RoomList
	{
		std::vector<Room> rooms;
		std::int32_t total;

		HICAL_JSON(RoomList, rooms, total)
	};

	struct Error
	{
		std::string message;
		HICAL_JSON(Error, message)
	};

} // namespace hical::example::contract

HICAL_SCHEMA_NAME(hical::example::contract::CreateRoom, "CreateRoom")
HICAL_SCHEMA_NAME(hical::example::contract::Room, "Room")
HICAL_SCHEMA_NAME(hical::example::contract::RoomList, "RoomList")
