/**
 * @file OpenApiRegistry.h
 * @brief 路由元数据注册表与注解宏
 * 提供：
 *   1. RouteApiInfo 数据结构 — 存储单个路由的 OpenAPI 标注（summary/tags/requestBody/responses）
 *   2. OpenApiRegistry 类 — 线程安全的路由和 schema 注册表
 *   3. HICAL_API() 宏 — 综合标注（summary/tags/request/response 一次性声明）
 *   4. builder::* 辅助函数 — 类型安全的标注构建器
 *   5. HICAL_ROUTES_WITH_API() 宏 — 增强版路由收集，自动收集 OpenAPI 标注
 *   6. registerRoutesWithOpenApi() — 注册路由同时收集 OpenAPI 元数据
 */

#pragma once

#include "Reflection.h"
#include "MetaRoutes.h"
#include "OpenApiSchema.h"
#include <boost/json.hpp>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace hical::meta::openapi
{

	// ============ 数据结构 ============

	/**
	 * @brief 单个路由的 OpenAPI 标注信息
	 */
	struct RouteApiInfo
	{
		std::string summary;
		std::string description;
		std::string operationId;
		std::vector<std::string> tags;

		// 请求体
		std::function<boost::json::object()> requestBodySchema; // 惰性求值
		std::string requestBodyDescription;
		bool requestBodyRequired = false;

		// 响应
		struct ResponseInfo
		{
			std::string description;
			std::function<boost::json::object()> schema; // 可为空
		};

		std::unordered_map<int, ResponseInfo> responses;

		// 路径参数覆盖
		struct ParamInfo
		{
			std::string name;
			std::string description;
			std::string schemaType = "string";
		};

		std::vector<ParamInfo> parameters;
	};

	/**
	 * @brief 已注册的路由条目（含 OpenAPI 标注）
	 */
	struct RegisteredRoute
	{
		HttpMethod method;
		std::string path;
		std::string handlerName;
		RouteApiInfo apiInfo;
	};

	// ============ Registry 类 ============

	/**
	 * @brief OpenAPI 路由和 Schema 注册表
	 * 线程安全：所有读写操作均通过 mutex 保护。
	 * 注意：读操作返回快照副本（by value），避免跨线程引用悬挂。
	 */
	class OpenApiRegistry
	{
	public:
		/**
		 * @brief 注册一条路由的 OpenAPI 元数据
		 */
		void addRoute(HttpMethod method, std::string path, std::string handlerName, RouteApiInfo apiInfo = {});

		/**
		 * @brief 注册一个 JSON Schema（用于 $ref 引用）
		 */
		void addSchema(const std::string& name, boost::json::object schema);

		/**
		 * @brief 检查指定名称的 Schema 是否已注册
		 */
		[[nodiscard]] bool hasSchema(const std::string& name) const;

		/**
		 * @brief 获取所有已注册路由的快照（线程安全，返回副本）
		 */
		[[nodiscard]] std::vector<RegisteredRoute> routes() const;

		/**
		 * @brief 获取所有已注册 schema 的快照（线程安全，返回副本）
		 */
		[[nodiscard]] std::unordered_map<std::string, boost::json::object> schemas() const;

	private:
		mutable std::mutex mutex_;
		std::vector<RegisteredRoute> routes_;
		std::unordered_map<std::string, boost::json::object> schemas_;
	};

	// ============ builder 辅助函数 ============

	namespace builder
	{

		inline void summary(RouteApiInfo& info, std::string_view s)
		{
			info.summary = std::string(s);
		}

		inline void description(RouteApiInfo& info, std::string_view d)
		{
			info.description = std::string(d);
		}

		inline void tags(RouteApiInfo& info, std::initializer_list<std::string_view> t)
		{
			info.tags.clear();
			for (auto sv : t)
			{
				info.tags.emplace_back(sv);
			}
		}

		/**
		 * @brief 声明请求体的 DTO 类型
		 * @tparam T 标注了 HICAL_JSON 的 DTO 类型
		 */
		template <typename T>
		inline void request(RouteApiInfo& info, std::string_view desc = "", bool required = true)
		{
			static_assert(HasJsonFields<T>::value, "Request body type must use HICAL_JSON() macro");
			info.requestBodySchema = []() -> boost::json::object
			{
				if constexpr (SchemaName<T>::value != nullptr)
				{
					boost::json::object ref;
					ref["$ref"] = std::string("#/components/schemas/") + SchemaName<T>::value;
					return ref;
				}
				else
				{
					return jsonSchema<T>();
				}
			};
			info.requestBodyDescription = std::string(desc);
			info.requestBodyRequired = required;
		}

		/**
		 * @brief 声明响应的 DTO 类型
		 * @tparam T 标注了 HICAL_JSON 的 DTO 类型
		 */
		template <typename T>
		inline void response(RouteApiInfo& info, int statusCode, std::string_view desc)
		{
			static_assert(HasJsonFields<T>::value, "Response body type must use HICAL_JSON() macro");
			info.responses[statusCode] =
				RouteApiInfo::ResponseInfo {std::string(desc),
											[]() -> boost::json::object
											{
												if constexpr (SchemaName<T>::value != nullptr)
												{
													boost::json::object ref;
													ref["$ref"] =
														std::string("#/components/schemas/") + SchemaName<T>::value;
													return ref;
												}
												else
												{
													return jsonSchema<T>();
												}
											}};
		}

		/**
		 * @brief 声明无 body 的响应（仅描述）
		 */
		inline void responseDesc(RouteApiInfo& info, int statusCode, std::string_view desc)
		{
			info.responses[statusCode] = RouteApiInfo::ResponseInfo {std::string(desc), nullptr};
		}

		/**
		 * @brief 声明路径参数的类型和描述
		 */
		inline void pathParam(RouteApiInfo& info,
							  std::string_view name,
							  std::string_view type,
							  std::string_view desc = "")
		{
			info.parameters.push_back(
				RouteApiInfo::ParamInfo {std::string(name), std::string(desc), std::string(type)});
		}

	} // namespace builder

	// ============ registerRoutesWithOpenApi ============

	namespace detail
	{

		/**
		 * @brief 从 hicalRouteTable + hicalApiInfoTable 同步收集路由元数据
		 */
		template <typename Handler, typename RouteTable, typename ApiTable, size_t... I>
		void collectWithApi(OpenApiRegistry& registry,
							const RouteTable& routeTable,
							const ApiTable& apiTable,
							std::index_sequence<I...>)
		{
			(...,
			 registry.addRoute(std::get<I>(routeTable).info.method,
							   std::string(std::get<I>(routeTable).info.path),
							   std::string(std::get<I>(routeTable).info.handlerName),
							   std::get<I>(apiTable)));
		}

		/**
		 * @brief 仅从 hicalRouteTable 收集路由基础信息（无 API 标注）
		 */
		template <typename Handler, typename RouteTable, size_t... I>
		void collectWithoutApi(OpenApiRegistry& registry, const RouteTable& routeTable, std::index_sequence<I...>)
		{
			(...,
			 registry.addRoute(std::get<I>(routeTable).info.method,
							   std::string(std::get<I>(routeTable).info.path),
							   std::string(std::get<I>(routeTable).info.handlerName)));
		}

	} // namespace detail

	/**
	 * @brief 注册路由到 Router 并同时收集 OpenAPI 元数据到 Registry
	 * @param router 路由器
	 * @param handler Handler 实例（引用版本）
	 * @param registry OpenAPI 注册表
	 */
	template <typename Handler>
	void registerRoutesWithOpenApi(Router& router, Handler& handler, OpenApiRegistry& registry)
	{
		static_assert(HasRouteTable<Handler>::value,
					  "Handler must use HICAL_ROUTES() or HICAL_ROUTES_WITH_API() macro");

		// 1. 正常注册路由
		registerRoutes(router, handler);

		// 2. 收集路由元数据
		auto routeTable = Handler::hicalRouteTable();
		constexpr auto count = std::tuple_size_v<decltype(routeTable)>;

		if constexpr (requires { Handler::hicalApiInfoTable(); })
		{
			auto apiTable = Handler::hicalApiInfoTable();
			detail::collectWithApi<Handler>(registry, routeTable, apiTable, std::make_index_sequence<count> {});
		}
		else
		{
			detail::collectWithoutApi<Handler>(registry, routeTable, std::make_index_sequence<count> {});
		}
	}

	/**
	 * @brief shared_ptr 版本
	 */
	template <typename Handler>
	void registerRoutesWithOpenApi(Router& router, std::shared_ptr<Handler> pHandler, OpenApiRegistry& registry)
	{
		static_assert(HasRouteTable<Handler>::value,
					  "Handler must use HICAL_ROUTES() or HICAL_ROUTES_WITH_API() macro");

		registerRoutes(router, pHandler);

		auto routeTable = Handler::hicalRouteTable();
		constexpr auto count = std::tuple_size_v<decltype(routeTable)>;

		if constexpr (requires { Handler::hicalApiInfoTable(); })
		{
			auto apiTable = Handler::hicalApiInfoTable();
			detail::collectWithApi<Handler>(registry, routeTable, apiTable, std::make_index_sequence<count> {});
		}
		else
		{
			detail::collectWithoutApi<Handler>(registry, routeTable, std::make_index_sequence<count> {});
		}
	}

} // namespace hical::meta::openapi

// ============ C++20 回退宏 ============

#if !HICAL_HAS_REFLECTION

	// NOLINTBEGIN(cppcoreguidelines-macro-usage)

	/**
	 * @brief 为单个路由添加 OpenAPI 标注（综合标注宏）
	 * 在 Handler 类中使用，配合 builder::* 函数声明路由的元数据。
	 * 用法：
	 * ```cpp
	 * HICAL_API(listUsers,
	 *     builder::summary(info, "List all users");
	 *     builder::tags(info, {"users"});
	 *     builder::response<UserDTO>(info, 200, "User list"))
	 * ```
	 */
	#define HICAL_API(func, ...)                                                            \
		inline static const ::hical::meta::openapi::RouteApiInfo hicalApiInfo_##func = []() \
		{                                                                                   \
			using namespace ::hical::meta::openapi;                                         \
			::hical::meta::openapi::RouteApiInfo info;                                      \
			info.operationId = #func;                                                       \
			__VA_ARGS__;                                                                    \
			return info;                                                                    \
		}();

	// ---- HICAL_ROUTES_WITH_API 增强版路由收集 ----

	// 叶子宏：直接引用 hicalApiInfo_<func>
	// 要求所有在 HICAL_ROUTES_WITH_API 中列出的 func 都必须有 HICAL_API 标注
	// 未标注的 func 使用 HICAL_API_DEFAULT 宏生成空标注
	#define HICAL_OA_INFO_(T, func) T::hicalApiInfo_##func

	/**
	 * @brief 为未标注 HICAL_API 的路由生成默认空标注
	 * 必须在 Handler 类定义内部使用，位置在 HICAL_HANDLER 之后、HICAL_ROUTES_WITH_API 之前。
	 * 用法：
	 * ```cpp
	 * struct MyHandler
	 * {
	 *     HttpResponse plain(const HttpRequest& req) { ... }
	 *     HICAL_HANDLER(Get, "/plain", plain)
	 *     HICAL_API_DEFAULT(plain)  // 生成默认空标注（operationId = "plain"）
	 *     HICAL_ROUTES_WITH_API(MyHandler, plain)
	 * };
	 * ```
	 */
	#define HICAL_API_DEFAULT(func)                                                         \
		inline static const ::hical::meta::openapi::RouteApiInfo hicalApiInfo_##func = []() \
		{                                                                                   \
			::hical::meta::openapi::RouteApiInfo info;                                      \
			info.operationId = #func;                                                       \
			return info;                                                                    \
		}();

	// __VA_OPT__ 递归展开（复用 HICAL_ROUTES 的模式）
	#define HICAL_OA_PARENS_ ()

	#define HICAL_OA_FOR_EACH_(T, a, ...) \
		HICAL_OA_INFO_(T, a) __VA_OPT__(, HICAL_OA_FE_AGAIN_ HICAL_OA_PARENS_(T, __VA_ARGS__))

	#define HICAL_OA_FE_AGAIN_() HICAL_OA_FOR_EACH_

	#define HICAL_OA_EXPAND_(...) HICAL_OA_EXP4_(HICAL_OA_EXP4_(__VA_ARGS__))
	#define HICAL_OA_EXP4_(...) HICAL_OA_EXP3_(HICAL_OA_EXP3_(__VA_ARGS__))
	#define HICAL_OA_EXP3_(...) HICAL_OA_EXP2_(HICAL_OA_EXP2_(__VA_ARGS__))
	#define HICAL_OA_EXP2_(...) HICAL_OA_EXP1_(HICAL_OA_EXP1_(__VA_ARGS__))
	#define HICAL_OA_EXP1_(...) __VA_ARGS__

	/**
	 * @brief 增强版路由收集宏，同时生成 API 标注表
	 * 在 HICAL_ROUTES 的基础上额外生成 hicalApiInfoTable() 方法。
	 * 用法：HICAL_ROUTES_WITH_API(MyHandler, listUsers, createUser, getUser)
	 */
	#define HICAL_ROUTES_WITH_API(Type, ...)                                                 \
		HICAL_ROUTES(Type, __VA_ARGS__)                                                      \
		static auto hicalApiInfoTable()                                                      \
		{                                                                                    \
			return std::make_tuple(HICAL_OA_EXPAND_(HICAL_OA_FOR_EACH_(Type, __VA_ARGS__))); \
		}

// NOLINTEND(cppcoreguidelines-macro-usage)

#else
	// C++26 反射模式下为空操作
	#define HICAL_API(func, ...)
	#define HICAL_ROUTES_WITH_API(Type, ...) HICAL_ROUTES(Type, __VA_ARGS__)
#endif
