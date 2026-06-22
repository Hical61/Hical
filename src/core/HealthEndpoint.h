/**
 * @file HealthEndpoint.h
 * @brief 健康检查端点，支持 K8s 存活探针和就绪探针
 * 参考 k8s 最佳实践，提供两个标准端点：
 * - GET {prefix}/health — 存活探针（liveness probe），进程活着即返回 200
 * - GET {prefix}/ready  — 就绪探针（readiness probe），通过回调检查服务是否就绪
 * 使用示例：
 * ```cpp
 * Router router;
 * // 默认 /admin/health 和 /admin/ready
 * registerHealthEndpoints(router);
 * // 自定义检查：数据库就绪后才标记为 ready
 * registerHealthEndpoints(router, {
 *     .prefix = "/admin",
 *     .readyCheck = []() -> bool {
 *         return dbPool && dbPool->isReady();
 *     }
 * });
 * ```
 */

#pragma once

#include "HttpRequest.h"
#include "HttpResponse.h"
#include "HttpTypes.h"

#include <functional>
#include <string>

namespace hical
{

	class Router;

	/**
	 * @brief 健康检查端点配置
	 */
	struct HealthEndpointOptions
	{
		/// URL 前缀，默认 "/admin"，端点路径为 {prefix}/health 和 {prefix}/ready
		std::string prefix = "/admin";

		/**
		 * @brief 就绪检查回调
		 * 返回 true 表示服务就绪，/ready 返回 200。
		 * 返回 false 时 /ready 返回 503。
		 * 默认 nullptr 表示始终就绪。
		 */
		std::function<bool()> readyCheck;
	};

	/**
	 * @brief 注册健康检查端点
	 * 注册两个端点：
	 * - GET {prefix}/health → 200 {"status":"ok"}
	 * - GET {prefix}/ready  → 200 {"status":"ready"} 或 503 {"status":"not ready","reason":"..."}
	 * @param router 路由器引用
	 * @param opts 配置选项
	 */
	void registerHealthEndpoints(Router& router, HealthEndpointOptions opts = {});

} // namespace hical
