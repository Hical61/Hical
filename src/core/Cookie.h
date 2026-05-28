/**
 * @file Cookie.h
 * @brief HTTP Cookie 解析与 Set-Cookie 构建
 */

#pragma once

#include <string>

namespace hical
{

	/**
	 * @brief Cookie 设置选项
	 * 用于 HttpResponse::setCookie() 配置 Set-Cookie 响应头的附加属性。
	 */
	struct CookieOptions
	{
		std::string path = "/";       ///< Cookie 作用路径，默认 "/"
		std::string domain;           ///< Cookie 作用域名，空表示当前域
		int maxAge = -1;              ///< 有效期（秒），-1 表示会话 Cookie（不写 Max-Age）
		bool httpOnly = true;         ///< 禁止 JavaScript 访问（防 XSS），默认开启
		bool secure = true;           ///< 仅通过 HTTPS 传输，默认开启（开发环境需显式关闭）
		std::string sameSite = "Lax"; ///< SameSite 策略，默认 "Lax"
	};

} // namespace hical
