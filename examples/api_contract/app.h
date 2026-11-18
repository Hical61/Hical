/**
 * @file app.h
 * @brief 在内存中存取房间，并提供注册路由的入口
 */

#pragma once

#include "dto.h"
#include <cstddef>
#include <optional>
#include <stdexcept>

namespace hical
{
	class Router;

	namespace meta::openapi
	{
		class OpenApiRegistry;
	} // namespace meta::openapi
} // namespace hical

namespace hical::example::contract
{
	inline constexpr std::string_view kGetRoomPath = "/rooms/{id}";

	/** @brief 房间数据只由服务的单个 I/O 线程读写 */
	class RoomStore
	{
	public:
		/**
		 * @brief 为校验通过的房间分配编号并保存
		 * @param input 房间名称和人数上限
		 * @return 包含新编号的房间
		 */
		Room create(const CreateRoom& input)
		{
			if (rooms_.size() == static_cast<std::size_t>(RoomId::kMaximum))
			{
				throw std::overflow_error("房间编号已用尽");
			}
			Room room {static_cast<std::int32_t>(rooms_.size() + 1), input.name, input.capacity};
			rooms_.push_back(room);
			return room;
		}

		/**
		 * @brief 按编号查找房间
		 * @param id 房间编号，必须是正整数
		 * @return 房间数据的副本，找不到时返回空值
		 */
		[[nodiscard]] std::optional<Room> get(std::int32_t id) const
		{
			if (id < RoomId::kMinimum || static_cast<std::size_t>(id) > rooms_.size())
			{
				return std::nullopt;
			}
			return rooms_[static_cast<std::size_t>(id - 1)];
		}

		/**
		 * @brief 按创建顺序读取房间列表
		 * @return 房间列表的副本和房间总数
		 */
		[[nodiscard]] RoomList list() const
		{
			return {rooms_, static_cast<std::int32_t>(rooms_.size())};
		}

	private:
		std::vector<Room> rooms_;
	};

	/**
	 * @brief 注册三个房间路由和对应的 OpenAPI 说明
	 * @param router 用来注册同步处理函数的路由器
	 * @param registry 保存接口文档的注册表
	 * @param store 由 I/O 线程读写的房间数据
	 */
	void registerRoomEndpoints(Router& router, meta::openapi::OpenApiRegistry& registry, RoomStore& store);

} // namespace hical::example::contract
