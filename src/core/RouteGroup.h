/**
 * @file RouteGroup.h
 * @brief 路由分组与前缀中间件
 */

#pragma once

#include "Router.h"
#include "Middleware.h"
#include <string>
#include <vector>

namespace hical
{

	/**
	 * @brief 路由组（前缀分组）
	 * 支持为一组路由设置公共前缀和组级中间件。
	 * 路由组是值对象（持有 Router 引用），注册时将路由展开写入 Router。
	 * 用法：
	 * ```cpp
	 * auto api = router.group("/api/v1");
	 * api.use(authMiddleware);
	 * api.get("/users", listUsers);
	 * api.post("/users", createUser);
	 * auto admin = api.group("/admin");
	 * admin.get("/stats", getStats);
	 * ```
	 */
	class RouteGroup
	{
	public:
		/**
		 * @brief 构造路由组
		 * @param router 路由器引用
		 * @param prefix 路由前缀（如 "/api/v1"）
		 * @param middlewares 继承自父组的中间件列表（兼容）
		 */
		RouteGroup(Router& router, std::string prefix, std::vector<MiddlewareHandler> middlewares = {});

		/**
		 * @brief 构造路由组（优化路径，接收 MiddlewareEntry 列表）
		 */
		RouteGroup(Router& router, std::string prefix, std::vector<MiddlewareEntry> entries);

		/**
		 * @brief 添加组级中间件（仅对组内路由生效）
		 * @param middleware 中间件处理器
		 */
		void use(MiddlewareHandler middleware);

		/**
		 * @brief 添加同步前置中间件（无协程帧开销）
		 */
		void use(SyncBeforeHandler before);

		/**
		 * @brief 添加同步前/后中间件（无协程帧开销）
		 */
		void use(SyncBeforeHandler before, SyncAfterHandler after);

		/**
		 * @brief 创建嵌套子组
		 * @param subPrefix 子前缀
		 * @return 子路由组（继承当前组的中间件和前缀）
		 */
		RouteGroup group(const std::string& subPrefix);

		// ============ 路由注册（镜像 Router API） ============

		/**
		 * @brief 注册协程路由处理器（路径自动拼接组前缀）
		 */
		void route(HttpMethod method, const std::string& path, RouteHandler handler);
		/**
		 * @brief 注册同步路由处理器（路径自动拼接组前缀）
		 */
		void route(HttpMethod method, const std::string& path, SyncRouteHandler handler);

		/**
		 * @brief 注册 GET 路由（协程处理器）
		 */
		void get(const std::string& path, RouteHandler handler);
		/**
		 * @brief 注册 GET 路由（同步处理器）
		 */
		void get(const std::string& path, SyncRouteHandler handler);

		/**
		 * @brief 注册 POST 路由（协程处理器）
		 */
		void post(const std::string& path, RouteHandler handler);
		/**
		 * @brief 注册 POST 路由（同步处理器）
		 */
		void post(const std::string& path, SyncRouteHandler handler);

		/**
		 * @brief 注册 PUT 路由（协程处理器）
		 */
		void put(const std::string& path, RouteHandler handler);
		/**
		 * @brief 注册 PUT 路由（同步处理器）
		 */
		void put(const std::string& path, SyncRouteHandler handler);

		/**
		 * @brief 注册 DELETE 路由（协程处理器）
		 */
		void del(const std::string& path, RouteHandler handler);
		/**
		 * @brief 注册 DELETE 路由（同步处理器）
		 */
		void del(const std::string& path, SyncRouteHandler handler);

	private:
		/**
		 * @brief 拼接前缀和子路径，处理双斜杠边界情况
		 */
		std::string joinPath(const std::string& path) const;

		/**
		 * @brief 用组级中间件包装 handler
		 * 无中间件时直接返回原 handler，有中间件时用 buildChainFrom 构建局部链。
		 */
		RouteHandler wrapHandler(RouteHandler handler) const;

		Router& router_;
		std::string prefix_;
		std::vector<MiddlewareEntry> entries_;
	};

} // namespace hical
