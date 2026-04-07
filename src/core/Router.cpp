#include "Router.h"

namespace hical
{

	// ============ 路由注册 ============

	void Router::route(HttpMethod method, const std::string& path, RouteHandler handler)
	{
		if (isParamRoute(path))
		{
			paramRoutes_.push_back({method, path, std::move(handler)});
		}
		else
		{
			staticRoutes_[{method, path}] = std::move(handler);
		}
	}

	void Router::route(HttpMethod method, const std::string& path, SyncRouteHandler handler)
	{
		auto asyncHandler = [h = std::move(handler)](const HttpRequest& req) -> Awaitable<HttpResponse>
		{
			co_return h(req);
		};
		route(method, path, std::move(asyncHandler));
	}

	// ============ 便捷方法 ============

	void Router::get(const std::string& path, RouteHandler handler)
	{
		route(HttpMethod::hGet, path, std::move(handler));
	}

	void Router::get(const std::string& path, SyncRouteHandler handler)
	{
		route(HttpMethod::hGet, path, std::move(handler));
	}

	void Router::post(const std::string& path, RouteHandler handler)
	{
		route(HttpMethod::hPost, path, std::move(handler));
	}

	void Router::post(const std::string& path, SyncRouteHandler handler)
	{
		route(HttpMethod::hPost, path, std::move(handler));
	}

	void Router::put(const std::string& path, RouteHandler handler)
	{
		route(HttpMethod::hPut, path, std::move(handler));
	}

	void Router::put(const std::string& path, SyncRouteHandler handler)
	{
		route(HttpMethod::hPut, path, std::move(handler));
	}

	void Router::del(const std::string& path, RouteHandler handler)
	{
		route(HttpMethod::hDelete, path, std::move(handler));
	}

	void Router::del(const std::string& path, SyncRouteHandler handler)
	{
		route(HttpMethod::hDelete, path, std::move(handler));
	}

	// ============ WebSocket ============

	void Router::ws(const std::string& path, WsMessageCallback onMessage, WsConnectCallback onConnect)
	{
		wsRoutes_.push_back({path, std::move(onMessage), std::move(onConnect)});
	}

	const Router::WsRoute* Router::findWsRoute(const std::string& path) const
	{
		for (const auto& route : wsRoutes_)
		{
			if (route.path == path)
			{
				return &route;
			}
		}
		return nullptr;
	}

	// ============ 分发 ============

	Awaitable<HttpResponse> Router::dispatch(HttpRequest& req)
	{
		auto reqMethod = req.method();
		auto reqPath = req.path();

		// 路径深度快速检查，防止超深路径 DoS
		size_t segmentCount = 0;
		for (char c : reqPath)
		{
			if (c == '/')
			{
				++segmentCount;
			}
		}
		if (segmentCount > hMaxPathSegments)
		{
			co_return HttpResponse::badRequest("Path too deep");
		}

		// 1. 优先查找静态路由（O(1) 哈希查找）
		auto it = staticRoutes_.find({reqMethod, reqPath});
		if (it != staticRoutes_.end())
		{
			co_return co_await it->second(req);
		}

		// 2. 回退到参数路由线性匹配
		for (const auto& entry : paramRoutes_)
		{
			if (entry.method != reqMethod)
			{
				continue;
			}

			std::unordered_map<std::string, std::string> params;
			if (matchParamPath(entry.path, reqPath, params))
			{
				for (const auto& [name, value] : params)
				{
					req.setParam(name, value);
				}
				co_return co_await entry.handler(req);
			}
		}

		co_return HttpResponse::notFound();
	}

	size_t Router::routeCount() const
	{
		return staticRoutes_.size() + paramRoutes_.size() + wsRoutes_.size();
	}

	// ============ 辅助方法 ============

	bool Router::isParamRoute(const std::string& path)
	{
		return path.find('{') != std::string::npos;
	}

	bool Router::matchParamPath(std::string_view pattern,
								std::string_view path,
								std::unordered_map<std::string, std::string>& params)
	{
		params.clear();

		// 跳过前导 '/'
		if (!pattern.empty() && pattern.front() == '/')
		{
			pattern.remove_prefix(1);
		}
		if (!path.empty() && path.front() == '/')
		{
			path.remove_prefix(1);
		}

		// 按 '/' 逐段匹配（零分配：使用 string_view 原地切分）
		size_t segmentCount = 0;
		while (!pattern.empty() && !path.empty())
		{
			// 段数限制，防止超深路径 DoS
			if (++segmentCount > Router::hMaxPathSegments)
			{
				params.clear();
				return false;
			}

			// 提取当前段
			auto pSlash = pattern.find('/');
			auto rSlash = path.find('/');

			auto patSeg = pattern.substr(0, pSlash);
			auto reqSeg = path.substr(0, rSlash);

			// 推进到下一段
			pattern = (pSlash == std::string_view::npos) ? std::string_view {} : pattern.substr(pSlash + 1);
			path = (rSlash == std::string_view::npos) ? std::string_view {} : path.substr(rSlash + 1);

			if (patSeg.size() >= 3 && patSeg.front() == '{' && patSeg.back() == '}')
			{
				// 参数值长度限制
				if (reqSeg.size() > Router::hMaxParamValueLength)
				{
					params.clear();
					return false;
				}
				// 参数段：提取参数名和值
				auto paramName = patSeg.substr(1, patSeg.size() - 2);
				params[std::string(paramName)] = std::string(reqSeg);
			}
			else if (patSeg != reqSeg)
			{
				params.clear();
				return false;
			}
		}

		// 两边必须同时用完
		if (!pattern.empty() || !path.empty())
		{
			params.clear();
			return false;
		}

		return true;
	}

} // namespace hical
