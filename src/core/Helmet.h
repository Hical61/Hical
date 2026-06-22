/**
 * @file Helmet.h
 * @brief 安全头中间件，自动为每个 HTTP 响应添加安全响应头
 * 所有头部默认启用，可单独关闭，支持自定义扩展头。
 * 使用示例：
 * ```cpp
 * // 启用所有默认安全头
 * server.use(makeHelmetMiddleware());
 * // 自定义配置：关闭 HSTS，自定义 CSP
 * server.use(makeHelmetMiddleware({
 *     .hsts = false,
 *     .csp = "default-src 'self' https://cdn.example.com",
 * }));
 * ```
 */

#pragma once

#include "HttpRequest.h"
#include "HttpResponse.h"
#include "Middleware.h"

#include <string>
#include <utility>
#include <vector>

namespace hical
{

	/**
	 * @brief 安全头中间件配置
	 * 每个选项对应一个安全响应头，默认全部启用。
	 * 设置对应选项为 false 可移除该安全头。
	 * customHeaders 用于添加框架内置之外的扩展安全头。
	 */
	struct HelmetOptions
	{
		/// X-Content-Type-Options: nosniff — 禁止 MIME 类型嗅探
		bool contentTypeNosniff = true;

		/// X-Frame-Options: DENY — 禁止 iframe 嵌套，防点击劫持
		bool frameDeny = true;

		/// Strict-Transport-Security: max-age=31536000; includeSubDomains — HSTS
		bool hsts = true;

		/// X-XSS-Protection: 0 — 禁用 XSS 过滤器（现代浏览器已废弃该头，设为 0 避免旧浏览器误判）
		bool xssProtection = true;

		/// Content-Security-Policy 值，空字符串表示不设置该头
		std::string csp = "default-src 'self'";

		/// Referrer-Policy 值，空字符串表示不设置该头
		std::string referrerPolicy = "strict-origin-when-cross-origin";

		/// Permissions-Policy 值，空字符串表示不设置该头
		std::string permissionsPolicy = "geolocation=(), microphone=(), camera=()";

		/// 自定义扩展头（如 "Expect-CT"、"Cross-Origin-Embedder-Policy" 等）
		std::vector<std::pair<std::string, std::string>> customHeaders;
	};

	/**
	 * @brief 创建安全头中间件（SyncAfterHandler）
	 * 纯后置中间件：在路由处理器返回响应后，自动注入安全响应头。
	 * 零协程帧开销，不修改响应体，不改变 Content-Length。
	 * @param opts 安全头配置，使用默认值即启用所有内置安全头
	 * @return SyncAfterHandler
	 */
	[[nodiscard]] SyncAfterHandler makeHelmetMiddleware(HelmetOptions opts = {});

} // namespace hical
