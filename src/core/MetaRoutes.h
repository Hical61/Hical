/**
 * @file MetaRoutes.h
 * @brief 反射驱动的自动路由注册
 * 双路线：
 * - C++26 反射：用户用 [[hical::route(...)]] 标注，框架自动发现并注册
 * - C++20 回退：用户用 HICAL_HANDLER / HICAL_ROUTES 宏标注，框架遍历注册
 * 对外 API：
 *   hical::meta::registerRoutes<UserHandler>(router, handler);
 * 用法（C++20 回退）：
 * ```cpp
 * struct UserHandler
 * {
 *     HttpResponse listUsers(const HttpRequest& req) { ... }
 *     HICAL_HANDLER(Get, "/api/users", listUsers)
 *     HttpResponse getUser(const HttpRequest& req) { ... }
 *     HICAL_HANDLER(Get, "/api/users/{id}", getUser)
 *     HICAL_ROUTES(UserHandler, listUsers, getUser)
 * };
 * UserHandler handler;
 * hical::meta::registerRoutes(router, handler);
 * ```
 */

#pragma once

#include "Reflection.h"
#include "Router.h"
#include "PerfectHashRouter.h"
#include <functional>
#include <memory>
#include <tuple>
#include <type_traits>
#include <vector>

namespace hical::meta
{

	// ============ C++20 回退实现 ============

#if !HICAL_HAS_REFLECTION

	namespace detail
	{

		/**
		 * @brief 注册单个同步路由
		 * 通过 shared_ptr 捕获 handler，确保生命周期安全。
		 */
		template <typename Handler>
		void registerOneRoute(Router& router,
							  std::shared_ptr<Handler> pHandler,
							  const RouteInfo& info,
							  HttpResponse (Handler::*fn)(const HttpRequest&))
		{
			router.route(info.method,
						 std::string(info.path),
						 [pHandler, fn](const HttpRequest& req) -> HttpResponse
						 {
							 return (pHandler.get()->*fn)(req);
						 });
		}

		/**
		 * @brief 注册单个协程路由
		 */
		template <typename Handler>
		void registerOneRoute(Router& router,
							  std::shared_ptr<Handler> pHandler,
							  const RouteInfo& info,
							  Awaitable<HttpResponse> (Handler::*fn)(const HttpRequest&))
		{
			router.route(info.method,
						 std::string(info.path),
						 [pHandler, fn](const HttpRequest& req) -> Awaitable<HttpResponse>
						 {
							 co_return co_await (pHandler.get()->*fn)(req);
						 });
		}

		/**
		 * @brief 路由注册函数对象
		 * 将 RouteInfo 和成员函数指针打包为可调用对象。
		 */
		template <typename Handler, typename MemFnPtr>
		struct RouteRegistrar
		{
			RouteInfo info;
			MemFnPtr fnPtr;

			void apply(Router& router, std::shared_ptr<Handler> pHandler) const
			{
				registerOneRoute(router, pHandler, info, fnPtr);
			}
		};

		template <typename Handler, typename MemFnPtr>
		constexpr RouteRegistrar<Handler, MemFnPtr> makeRegistrar(RouteInfo info, MemFnPtr fn)
		{
			return {info, fn};
		}

		/**
		 * @brief 遍历 tuple 逐个注册
		 */
		template <typename Handler, typename Tuple, size_t... I>
		void registerAll(Router& router,
						 std::shared_ptr<Handler> pHandler,
						 const Tuple& table,
						 std::index_sequence<I...>)
		{
			(std::get<I>(table).apply(router, pHandler), ...);
		}

		/**
		 * @brief 提取所有静态路由的 (method, path) 对，用于构建完美哈希表
		 * 参数路由和通配路由不参与编译期哈希。
		 */
		template <size_t... I, typename Tuple>
		std::vector<std::pair<HttpMethod, std::string_view>> collectStaticRouteKeys(const Tuple& table,
																					std::index_sequence<I...>)
		{
			std::vector<std::pair<HttpMethod, std::string_view>> keys;
			// 折叠表达式：逐个检查是否为静态路由
			auto collect = [&](const auto& reg)
			{
				// 不含 '{' 且不含 '*' 的才是静态路由
				if (reg.info.path.find('{') == std::string_view::npos
					&& reg.info.path.find('*') == std::string_view::npos)
				{
					keys.emplace_back(reg.info.method, reg.info.path);
				}
			};
			(collect(std::get<I>(table)), ...);
			return keys;
		}

	} // namespace detail

	/**
	 * @brief 自动注册 Handler 中所有路由到 Router（shared_ptr 版本，推荐）
	 * 注册完成后自动构建运行时完美哈希表加速静态路由查找。
	 * 通过 shared_ptr 管理 handler 生命周期，确保路由回调中的引用始终有效。
	 * 适用于 handler 需要跨异步边界存活的场景。
	 */
	template <typename Handler>
	void registerRoutes(Router& router, std::shared_ptr<Handler> pHandler)
	{
		static_assert(HasRouteTable<Handler>::value,
					  "Handler must use HICAL_ROUTES() macro or have C++26 reflection support");

		auto table = Handler::hicalRouteTable();
		constexpr auto count = std::tuple_size_v<decltype(table)>;
		detail::registerAll(router, pHandler, table, std::make_index_sequence<count> {});

		// 收集静态路由键并构建完美哈希表
		auto keys = detail::collectStaticRouteKeys(table, std::make_index_sequence<count> {});
		if (!keys.empty())
		{
			auto lookup = RuntimePerfectHashLookup::buildFromKeys(keys);
			if (lookup.valid())
			{
				router.setPerfectHashLookup(std::move(lookup));
			}
		}
	}

	/**
	 * @brief 自动注册 Handler 中所有路由到 Router（引用版本，便捷）
	 * 内部创建 shared_ptr（以空删除器包装），调用者需确保 handler 的生命周期
	 * 覆盖所有路由回调的执行期（如 server.start() 阻塞期间）。
	 * 对于非阻塞/异步场景，推荐使用 shared_ptr 重载。
	 */
	template <typename Handler>
	void registerRoutes(Router& router, Handler& handler)
	{
		static_assert(HasRouteTable<Handler>::value,
					  "Handler must use HICAL_ROUTES() macro or have C++26 reflection support");

		// 空删除器：不接管所有权，由调用者管理生命周期
		auto pHandler = std::shared_ptr<Handler>(&handler,
												 [](Handler*)
												 {
												 });
		registerRoutes(router, pHandler);
	}

#else // HICAL_HAS_REFLECTION == 1

	// ============ C++26 反射实现 ============

	template <typename Handler>
	void registerRoutes(Router& router, Handler& handler)
	{
		template for (constexpr auto fn :
					  std::meta::nonstatic_member_functions_of(^^Handler, std::meta::access_context::unprivileged()))
		{
			constexpr auto attrs = std::meta::attributes_of(fn);
			template for (constexpr auto attr : attrs)
			{
				if constexpr (std::meta::identifier_of(attr) == "hical::route")
				{
					constexpr auto args = std::meta::attribute_arguments_of(attr);
					constexpr auto path = std::meta::extract<const char*>(args[0]);
					constexpr auto methodStr = std::meta::extract<const char*>(args[1]);
					auto method = stringToHttpMethod(methodStr);

					router.route(method,
								 path,
								 [&handler](const HttpRequest& req) -> Awaitable<HttpResponse>
								 {
									 if constexpr (std::is_same_v<decltype(handler.[:fn:](req)), HttpResponse>)
									 {
										 co_return handler.[:fn:](req);
									 }
									 else
									 {
										 co_return co_await handler.[:fn:](req);
									 }
								 });
				}
			}
		}
	}

#endif // HICAL_HAS_REFLECTION

} // namespace hical::meta

// ============ C++20 回退宏 ============

#if !HICAL_HAS_REFLECTION

/**
 * @brief 标注单个路由处理器（C++20 回退方案）
 * @param method HTTP 方法（Get, Post, Put, Delete 等）
 * @param path   路由路径
 * @param func   成员函数名
 */
	#define HICAL_HANDLER(method, path, func) \
		static constexpr ::hical::meta::RouteInfo hicalRouteInfo_##func {::hical::HttpMethod::h##method, path, #func};

/**
 * @brief 收集所有路由（C++20 回退方案）
 * 第一个参数为 Handler 类型名，后续参数为成员函数名。
 * 用法：HICAL_ROUTES(MyHandler, listUsers, getUser)
 */
	#define HICAL_ROUTES(Type, ...)                                                                  \
		static auto hicalRouteTable()                                                                \
		{                                                                                            \
			return std::make_tuple(HICAL_ROUTES_EXPAND_(HICAL_ROUTES_FOR_EACH_(Type, __VA_ARGS__))); \
		}

	// 生成单个 registrar
	#define HICAL_ROUTE_REG_(T, func) ::hical::meta::detail::makeRegistrar<T>(T::hicalRouteInfo_##func, &T::func)

// ---- __VA_OPT__ 递归展开 ----

	#define HICAL_ROUTES_PARENS_ ()

	#define HICAL_ROUTES_FOR_EACH_(T, a, ...) \
		HICAL_ROUTE_REG_(T, a) __VA_OPT__(, HICAL_ROUTES_FE_AGAIN_ HICAL_ROUTES_PARENS_(T, __VA_ARGS__))

	#define HICAL_ROUTES_FE_AGAIN_() HICAL_ROUTES_FOR_EACH_

	// 多层 EXPAND 解锁递归深度（支持最多 243 个路由）
	#define HICAL_ROUTES_EXPAND_(...) HICAL_ROUTES_EXP4_(HICAL_ROUTES_EXP4_(__VA_ARGS__))
	#define HICAL_ROUTES_EXP4_(...) HICAL_ROUTES_EXP3_(HICAL_ROUTES_EXP3_(__VA_ARGS__))
	#define HICAL_ROUTES_EXP3_(...) HICAL_ROUTES_EXP2_(HICAL_ROUTES_EXP2_(__VA_ARGS__))
	#define HICAL_ROUTES_EXP2_(...) HICAL_ROUTES_EXP1_(HICAL_ROUTES_EXP1_(__VA_ARGS__))
	#define HICAL_ROUTES_EXP1_(...) __VA_ARGS__

#else
	#define HICAL_HANDLER(method, path, func)
	#define HICAL_ROUTES(Type, ...)
#endif
