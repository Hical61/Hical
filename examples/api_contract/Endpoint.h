/**
 * @file Endpoint.h
 * @brief 房间示例的同步 JSON 接口适配器
 */

#pragma once

#include "dto.h"
#include "core/OpenApiRegistry.h"
#include "core/Router.h"
#include <charconv>
#include <concepts>
#include <cstdint>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#ifndef HICAL_HAS_OPENAPI
	#error "房间接口示例需要启用 HICAL_WITH_OPENAPI 的 Hical 安装包"
#endif

namespace hical::example::contract
{
	namespace openapi = hical::meta::openapi;

	struct EndpointSpec
	{
		HttpMethod method;
		std::string_view path;
		std::string_view operationId;
		HttpStatusCode successStatus;
	};

	template <typename T>
	concept EndpointDto = std::is_class_v<T> && std::is_aggregate_v<T> && std::same_as<T, std::remove_cvref_t<T>>;

	namespace detail
	{
		inline std::function<boost::json::object()> schemaProvider(openapi::OpenApiRegistry& registry,
																   boost::json::object schema,
																   const char* name)
		{
			if (name != nullptr)
			{
				// 服务启动时，同一个线程会注册路由和组件。
				if (registry.hasSchema(name))
				{
					if (registry.schemas().at(name) != schema)
					{
						throw std::logic_error(std::string("同名 OpenAPI 组件内容不同：") + name);
					}
				}
				else
				{
					registry.addSchema(name, std::move(schema));
				}
				schema = {{"$ref", std::string("#/components/schemas/") + name}};
			}
			return [schema = std::move(schema)]
			{
				return schema;
			};
		}

		template <EndpointDto Output>
		openapi::RouteApiInfo makeOperationInfo(openapi::OpenApiRegistry& registry, const EndpointSpec& spec)
		{
			openapi::RouteApiInfo info;
			info.tags.emplace_back("Rooms");
			info.responses.emplace(
				static_cast<int>(spec.successStatus),
				openapi::RouteApiInfo::ResponseInfo {
					"成功",
					schemaProvider(registry, openapi::outputSchema<Output>(), openapi::SchemaName<Output>::value)});
			return info;
		}

		inline void addErrorResponse(openapi::OpenApiRegistry& registry,
									 openapi::RouteApiInfo& info,
									 HttpStatusCode status,
									 std::string description)
		{
			info.responses.emplace(static_cast<int>(status),
								   openapi::RouteApiInfo::ResponseInfo {
									   std::move(description),
									   schemaProvider(registry, openapi::outputSchema<Error>(), "Error")});
		}

		inline void bindEndpoint(Router& router,
								 openapi::OpenApiRegistry& registry,
								 const EndpointSpec& spec,
								 openapi::RouteApiInfo info,
								 SyncRouteHandler handler)
		{
			std::string path(spec.path);
			router.route(spec.method, path, std::move(handler));
			registry.addRoute(spec.method, std::move(path), std::string(spec.operationId), std::move(info));
		}

		template <EndpointDto Output>
		HttpResponse jsonResponse(HttpStatusCode status, const Output& output)
		{
			HttpResponse response;
			response.setStatus(status);
			response.setJsonBody(meta::toJson(output));
			return response;
		}

		inline std::optional<RoomId> readRoomId(const HttpRequest& request)
		{
			const auto& text = request.param(RoomId::kParam);
			std::int32_t id = 0;
			const auto end = text.data() + text.size();
			const auto [next, error] = std::from_chars(text.data(), end, id);
			if (error != std::errc {} || next != end || id < RoomId::kMinimum)
			{
				return std::nullopt;
			}
			return RoomId {id};
		}
	} // namespace detail

	/**
	 * @brief 注册不需要输入的同步接口
	 * @param router 用来注册同步处理函数的路由器
	 * @param registry 保存接口文档的注册表
	 * @param spec 接口的 HTTP 方法、路径、名称和成功状态码
	 * @param state 业务状态，要保留到服务退出
	 * @param handler 返回响应 DTO 的同步函数
	 */
	template <typename State, EndpointDto Output>
	void registerEndpoint(Router& router,
						  openapi::OpenApiRegistry& registry,
						  const EndpointSpec& spec,
						  State& state,
						  Output (*handler)(State&))
	{
		auto info = detail::makeOperationInfo<Output>(registry, spec);
		detail::bindEndpoint(router,
							 registry,
							 spec,
							 std::move(info),
							 [&state, handler, status = spec.successStatus](const HttpRequest&)
							 {
								 return detail::jsonResponse(status, handler(state));
							 });
	}

	/**
	 * @brief 注册从 JSON 请求体读取 DTO 的同步接口
	 * @param router 用来注册同步处理函数的路由器
	 * @param registry 保存接口文档的注册表
	 * @param spec 接口的 HTTP 方法、路径、名称和成功状态码
	 * @param state 业务状态，要保留到服务退出
	 * @param handler 接收校验过的输入并返回响应 DTO 的同步函数
	 */
	template <typename State, EndpointDto Input, EndpointDto Output>
	void registerEndpoint(Router& router,
						  openapi::OpenApiRegistry& registry,
						  const EndpointSpec& spec,
						  State& state,
						  Output (*handler)(State&, const Input&))
	{
		auto info = detail::makeOperationInfo<Output>(registry, spec);
		info.requestBodySchema =
			detail::schemaProvider(registry, openapi::inputSchema<Input>(), openapi::SchemaName<Input>::value);
		info.requestBodyRequired = true;
		detail::addErrorResponse(registry, info, HttpStatusCode::hBadRequest, "请求体不符合契约");
		detail::bindEndpoint(router,
							 registry,
							 spec,
							 std::move(info),
							 [&state, handler, status = spec.successStatus](const HttpRequest& request)
							 {
								 std::optional<Input> input;
								 try
								 {
									 input.emplace(request.readJson<Input>());
								 }
								 catch (const std::runtime_error&)
								 {
									 return detail::jsonResponse(HttpStatusCode::hBadRequest,
																 Error {"请求体不符合契约"});
								 }
								 return detail::jsonResponse(status, handler(state, *input));
							 });
	}

	/**
	 * @brief 注册按正整数编号查询房间的同步接口
	 * @param router 用来注册同步处理函数的路由器
	 * @param registry 保存接口文档的注册表
	 * @param spec 路径以 /{id} 结尾的接口声明
	 * @param state 业务状态，要保留到服务退出
	 * @param handler 找不到房间时返回空值的同步函数
	 */
	template <typename State, EndpointDto Output>
	void registerEndpoint(Router& router,
						  openapi::OpenApiRegistry& registry,
						  const EndpointSpec& spec,
						  State& state,
						  std::optional<Output> (*handler)(State&, RoomId))
	{
		if (!spec.path.ends_with("/{id}"))
		{
			throw std::logic_error("房间查询路径必须以 /{id} 结束");
		}
		auto info = detail::makeOperationInfo<Output>(registry, spec);
		openapi::builder::pathParam(info, RoomId::kParam, "integer", "正的 32 位房间编号");
		detail::addErrorResponse(registry, info, HttpStatusCode::hBadRequest, "房间编号无效");
		detail::addErrorResponse(registry, info, HttpStatusCode::hNotFound, "房间不存在");
		detail::bindEndpoint(router,
							 registry,
							 spec,
							 std::move(info),
							 [&state, handler, status = spec.successStatus](const HttpRequest& request)
							 {
								 const auto id = detail::readRoomId(request);
								 if (!id)
								 {
									 return detail::jsonResponse(HttpStatusCode::hBadRequest, Error {"房间编号无效"});
								 }
								 const auto output = handler(state, *id);
								 if (!output)
								 {
									 return detail::jsonResponse(HttpStatusCode::hNotFound, Error {"房间不存在"});
								 }
								 return detail::jsonResponse(status, *output);
							 });
	}
} // namespace hical::example::contract
