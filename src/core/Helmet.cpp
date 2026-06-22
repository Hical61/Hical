/**
 * @file Helmet.cpp
 * @brief 安全头中间件实现
 */

#include "core/Helmet.h"

#include <string_view>

using namespace std::string_view_literals;

namespace hical
{

	SyncAfterHandler makeHelmetMiddleware(HelmetOptions opts)
	{
		return [opts = std::move(opts)](HttpRequest& /*req*/, HttpResponse& res) -> void
		{
			// X-Content-Type-Options: nosniff
			if (opts.contentTypeNosniff)
			{
				res.setHeader("X-Content-Type-Options"sv, "nosniff"sv);
			}

			// X-Frame-Options: DENY
			if (opts.frameDeny)
			{
				res.setHeader("X-Frame-Options"sv, "DENY"sv);
			}

			// Strict-Transport-Security
			if (opts.hsts)
			{
				res.setHeader("Strict-Transport-Security"sv, "max-age=31536000; includeSubDomains"sv);
			}

			// X-XSS-Protection: 0
			if (opts.xssProtection)
			{
				res.setHeader("X-XSS-Protection"sv, "0"sv);
			}

			// Content-Security-Policy
			if (!opts.csp.empty())
			{
				res.setHeader("Content-Security-Policy"sv, std::string_view(opts.csp));
			}

			// Referrer-Policy
			if (!opts.referrerPolicy.empty())
			{
				res.setHeader("Referrer-Policy"sv, std::string_view(opts.referrerPolicy));
			}

			// Permissions-Policy
			if (!opts.permissionsPolicy.empty())
			{
				res.setHeader("Permissions-Policy"sv, std::string_view(opts.permissionsPolicy));
			}

			// 自定义头
			for (const auto& [name, value] : opts.customHeaders)
			{
				res.setHeader(std::string_view(name), std::string_view(value));
			}
		};
	}

} // namespace hical
