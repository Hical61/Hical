/**
 * @file LogAdmin.h
 * @brief 运行时动态日志级别管理端点
 */

#pragma once

#include "HttpRequest.h"
#include "HttpResponse.h"

#include <functional>
#include <string>

namespace hical
{

	class Router;

	/// 认证回调：返回 std::nullopt 表示通过，返回 HttpResponse 表示拒绝（如 401/403）
	using AdminAuthCheck = std::function<std::optional<HttpResponse>(const HttpRequest&)>;

	/**
	 * @brief 注册日志管理 Admin 端点
	 * - GET  {prefix}/log-level → 返回默认通道和所有命名通道的级别
	 * - PUT  {prefix}/log-level → 调整级别（body: {"level":"DEBUG"} 或 {"channel":"access","level":"WARN"}）
	 * @warning 此端点允许动态修改日志级别，生产环境中**必须**传入认证回调以防止未授权访问。
	 *          未传入认证回调时，任何能访问该端口的客户端均可修改日志级别。
	 * @param router 路由器引用
	 * @param prefix URL 前缀（默认 "/admin"）
	 * @param authCheck 可选的认证回调，返回 std::nullopt 表示通过，返回 HttpResponse 表示拒绝
	 */
	void registerLogAdminEndpoints(Router& router,
								   const std::string& prefix = "/admin",
								   AdminAuthCheck authCheck = nullptr);

} // namespace hical
