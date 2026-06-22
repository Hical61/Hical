/**
 * @file HealthEndpoint.cpp
 * @brief 健康检查端点实现
 */

#include "core/HealthEndpoint.h"

#include "core/Router.h"

#include <boost/json/object.hpp>
#include <boost/json/serialize.hpp>
#include <boost/json/value.hpp>

namespace hical
{

	void registerHealthEndpoints(Router& router, HealthEndpointOptions opts)
	{
		// GET {prefix}/health — 存活探针
		router.get(opts.prefix + "/health",
				   [](const HttpRequest& /*req*/) -> HttpResponse
				   {
					   boost::json::object result;
					   result["status"] = "ok";
					   return HttpResponse::json(result);
				   });

		// GET {prefix}/ready — 就绪探针
		router.get(opts.prefix + "/ready",
				   [readyCheck = std::move(opts.readyCheck)](const HttpRequest& /*req*/) -> HttpResponse
				   {
					   // 有自定义检查回调时，按回调结果决定
					   if (readyCheck)
					   {
						   if (readyCheck())
						   {
							   boost::json::object result;
							   result["status"] = "ready";
							   return HttpResponse::json(result);
						   }

						   boost::json::object result;
						   result["status"] = "not ready";
						   result["reason"] = "service is not ready";
						   auto res = HttpResponse::json(result);
						   res.setStatus(HttpStatusCode::hServiceUnavailable);
						   return res;
					   }

					   // 无自定义检查：始终就绪
					   boost::json::object result;
					   result["status"] = "ready";
					   return HttpResponse::json(result);
				   });
	}

} // namespace hical
