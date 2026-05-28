/**
 * @file Cors.h
 * @brief CORS 跨域中间件
 */

#pragma once

#include "Coroutine.h"
#include "HttpRequest.h"
#include "HttpResponse.h"
#include "HttpTypes.h"
#include "Middleware.h"
#include <stdexcept>
#include <string>
#include <vector>

namespace hical
{

	/**
	 * @brief CORS 配置选项
	 */
	struct CorsOptions
	{
		/// 允许的 Origin 列表（{"*"} 表示全放行，具体域名精确匹配）
		std::vector<std::string> allowedOrigins = {"*"};

		/// 允许的 HTTP 方法
		std::vector<std::string> allowedMethods = {"GET", "POST", "PUT", "DELETE", "PATCH", "OPTIONS"};

		/// 允许的请求头
		std::vector<std::string> allowedHeaders = {"Content-Type", "Authorization"};

		/// 暴露给客户端的响应头
		std::vector<std::string> exposeHeaders;

		/// 是否允许携带凭证（Cookie 等）
		bool allowCredentials = false;

		/// preflight 缓存时间（秒）
		int maxAge = 86400;
	};

	namespace detail
	{

		inline std::string joinStrings(const std::vector<std::string>& vec)
		{
			std::string result;
			for (size_t i = 0; i < vec.size(); ++i)
			{
				if (i > 0)
				{
					result += ", ";
				}
				result += vec[i];
			}
			return result;
		}

	} // namespace detail

	/**
	 * @brief 创建 CORS 中间件
	 * 行为：
	 * - 无 Origin 头 → 直接透传，不附加任何 CORS 响应头
	 * - Origin 不在白名单 → 直接透传，不附加任何 CORS 响应头
	 * - OPTIONS preflight → 返回 204，附加完整 CORS 预检响应头，不调用 next
	 * - 其他跨域请求 → 调用 next，后置附加 CORS 简单响应头
	 * - allowCredentials=true 时 Allow-Origin 必须回显具体 origin 而非 "*"
	 * - 非通配符模式时附加 Vary: Origin，提示缓存按 Origin 区分
	 * @param opts CORS 配置选项
	 * @return MiddlewareHandler 可传入 server.use() 的中间件
	 */
	inline MiddlewareHandler makeCorsMiddleware(CorsOptions opts = {})
	{
		// credentials + wildcard origin 是经典 CSRF 坑，直接拒绝
		if (opts.allowCredentials)
		{
			for (const auto& o : opts.allowedOrigins)
			{
				if (o == "*")
				{
					throw std::invalid_argument("CORS: allowCredentials=true with wildcard origin '*' is insecure. "
												"Please specify explicit allowed origins.");
				}
			}
		}

		// 预拼接好，preflight 时直接用
		auto methodsStr = detail::joinStrings(opts.allowedMethods);
		auto headersStr = detail::joinStrings(opts.allowedHeaders);
		auto maxAgeStr = std::to_string(opts.maxAge);
		auto exposeHeadersStr = detail::joinStrings(opts.exposeHeaders);

		return [opts = std::move(opts),
				methodsStr = std::move(methodsStr),
				headersStr = std::move(headersStr),
				maxAgeStr = std::move(maxAgeStr),
				exposeHeadersStr = std::move(exposeHeadersStr)](HttpRequest& req,
																MiddlewareNext next) -> Awaitable<HttpResponse>
		{
			auto origin = std::string(req.header("Origin"));

			if (origin.empty())
			{
				co_return co_await next(req);
			}

			bool isWildcard = false;
			bool originAllowed = false;
			for (const auto& allowed : opts.allowedOrigins)
			{
				if (allowed == "*")
				{
					isWildcard = true;
					originAllowed = true;
					break;
				}
				if (allowed == origin)
				{
					originAllowed = true;
					break;
				}
			}

			if (!originAllowed)
			{
				co_return co_await next(req);
			}

			// credentials 时不能用 "*"，得回显具体 origin
			std::string allowOriginValue = (opts.allowCredentials || !isWildcard) ? origin : "*";

			auto applyCommonHeaders = [&](HttpResponse& res)
			{
				res.setHeader("Access-Control-Allow-Origin", allowOriginValue);
				if (opts.allowCredentials)
				{
					res.setHeader("Access-Control-Allow-Credentials", "true");
				}
				if (!isWildcard)
				{
					res.setHeader("Vary", "Origin");
				}
			};

			// preflight = OPTIONS + 带 Access-Control-Request-Method 头
			// 纯 OPTIONS 不算 preflight，正常路由
			bool isPreflight =
				req.method() == HttpMethod::hOptions && !req.header("Access-Control-Request-Method").empty();

			if (isPreflight)
			{
				HttpResponse res;
				res.setStatus(HttpStatusCode::hNoContent);
				applyCommonHeaders(res);
				res.setHeader("Access-Control-Allow-Methods", methodsStr);
				res.setHeader("Access-Control-Allow-Headers", headersStr);
				res.setHeader("Access-Control-Max-Age", maxAgeStr);
				co_return res;
			}

			auto res = co_await next(req);
			applyCommonHeaders(res);

			if (!exposeHeadersStr.empty())
			{
				res.setHeader("Access-Control-Expose-Headers", exposeHeadersStr);
			}

			co_return res;
		};
	}

} // namespace hical
