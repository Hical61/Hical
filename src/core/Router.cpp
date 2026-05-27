#include "Router.h"
#include "RouteGroup.h"

namespace hical
{

	// ============ 路由注册 ============

	void Router::route(HttpMethod method, const std::string& path, RouteHandler handler)
	{
		if (isParamRoute(path))
		{
			paramRoutesByMethod_[method].push_back({method, path, std::move(handler), nullptr});
		}
		else
		{
			staticRoutes_[{method, path}] = RouteEntry {std::move(handler), nullptr};
			staticPathMethods_[path].push_back(method);
		}
	}

	void Router::route(HttpMethod method, const std::string& path, SyncRouteHandler handler)
	{
		// 只存 syncHandler，不创建 asyncHandler wrapper，零拷贝。
		// dispatch() 优先检查 syncHandler，asyncHandler 为 nullptr 时不会被调用。
		if (isParamRoute(path))
		{
			paramRoutesByMethod_[method].push_back({method, path, nullptr, std::move(handler)});
		}
		else
		{
			staticRoutes_[{method, path}] = RouteEntry {nullptr, std::move(handler)};
			staticPathMethods_[path].push_back(method);
		}
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

	void Router::ws(const std::string& path,
					WsMessageCallback onMessage,
					WsConnectCallback onConnect,
					WsDisconnectCallback onDisconnect)
	{
		WsRoute route;
		route.path = path;
		route.onMessage = std::move(onMessage);
		route.onConnect = std::move(onConnect);
		route.onDisconnect = std::move(onDisconnect);
		wsRoutes_.push_back(std::move(route));
	}

	void Router::ws(const std::string& path,
					WsOptions options,
					WsMessageCallback onMessage,
					WsConnectCallback onConnect,
					WsDisconnectCallback onDisconnect)
	{
		WsRoute route;
		route.path = path;
		route.onMessage = std::move(onMessage);
		route.onConnect = std::move(onConnect);
		route.onDisconnect = std::move(onDisconnect);
		route.allowedOrigins = std::move(options.allowedOrigins);
		route.enableCompression = options.enableCompression;
		route.serverMaxWindowBits = options.serverMaxWindowBits;
		route.clientMaxWindowBits = options.clientMaxWindowBits;
		route.serverNoContextTakeover = options.serverNoContextTakeover;
		route.pingInterval = options.pingInterval;
		route.maxMissedPongs = options.maxMissedPongs;
		route.pingPayload = std::move(options.pingPayload);
		route.subprotocols = std::move(options.subprotocols);
		wsRoutes_.push_back(std::move(route));
	}

	Router::WsRouteMatch Router::findWsRoute(std::string_view path) const
	{
		WsRouteMatch result;

		// 1. 精确匹配（快速路径）
		for (const auto& route : wsRoutes_)
		{
			if (!isParamRoute(route.path) && route.path == path)
			{
				result.route = &route;
				return result;
			}
		}

		// 2. 参数路由匹配
		for (const auto& route : wsRoutes_)
		{
			ParamList params;
			if (isParamRoute(route.path) && matchParamPath(route.path, path, params))
			{
				result.route = &route;
				result.params = std::move(params);
				return result;
			}
		}

		return result;
	}

	// ============ 分发 ============

	Awaitable<HttpResponse> Router::dispatch(HttpRequest& req)
	{
		auto result = resolveRoute(req);

		if (result.pathTooDeep)
		{
			co_return HttpResponse::badRequest("Path too deep");
		}

		if (result.staticEntry)
		{
			if (result.staticEntry->syncHandler)
			{
				co_return result.staticEntry->syncHandler(req);
			}
			co_return co_await result.staticEntry->asyncHandler(req);
		}

		if (result.paramEntry)
		{
			if (result.paramEntry->syncHandler)
			{
				co_return result.paramEntry->syncHandler(req);
			}
			co_return co_await result.paramEntry->handler(req);
		}

		if (!result.allowedMethods.empty())
		{
			HttpResponse res;
			res.setStatus(HttpStatusCode::hMethodNotAllowed);
			res.setHeader("Allow", result.allowedMethods);
			res.setBody("Method Not Allowed");
			co_return res;
		}

		co_return HttpResponse::notFound();
	}

	std::optional<HttpResponse> Router::dispatchSync(HttpRequest& req)
	{
		auto result = resolveRoute(req);

		if (result.pathTooDeep)
		{
			return HttpResponse::badRequest("Path too deep");
		}

		if (result.staticEntry)
		{
			if (result.staticEntry->syncHandler)
			{
				return result.staticEntry->syncHandler(req);
			}
			return std::nullopt; // 异步 handler，需要 fallback 到 co_await dispatch()
		}

		if (result.paramEntry)
		{
			if (result.paramEntry->syncHandler)
			{
				return result.paramEntry->syncHandler(req);
			}
			return std::nullopt; // 异步 handler
		}

		// 404/405 无法同步处理，回退到异步 dispatch
		return std::nullopt;
	}

	Router::ResolveResult Router::resolveRoute(HttpRequest& req) const
	{
		ResolveResult result;
		auto reqMethod = req.method();
		auto rawPath = req.path();

		// 单次遍历：同时检查 urlDecode 需求和路径深度
		bool needsDecode = false;
		size_t segmentCount = 0;
		for (char c : rawPath)
		{
			if (c == '%' || c == '+')
			{
				needsDecode = true;
			}
			if (c == '/')
			{
				++segmentCount;
			}
		}

		if (segmentCount > kMaxPathSegments)
		{
			result.pathTooDeep = true;
			return result;
		}

		std::string decodedStorage;
		std::string_view reqPath;
		if (needsDecode)
		{
			decodedStorage = urlDecode(rawPath);
			reqPath = decodedStorage;
		}
		else
		{
			reqPath = rawPath;
		}

		// 1. 优先查找静态路由（O(1) 哈希查找，透明哈希避免构造临时 std::string）
		if (auto it = staticRoutes_.find(RouteKeyView {reqMethod, reqPath}); it != staticRoutes_.end())
		{
			result.staticEntry = &it->second;
			return result;
		}

		// 2. 回退到参数路由匹配（按 method 分组，仅扫描同 method 的路由）
		if (auto groupIt = paramRoutesByMethod_.find(reqMethod); groupIt != paramRoutesByMethod_.end())
		{
			ParamList params;
			for (const auto& entry : groupIt->second)
			{
				if (matchParamPath(entry.path, reqPath, params))
				{
					for (const auto& [name, value] : params)
					{
						req.setParam(name, value);
					}
					result.paramEntry = &entry;
					return result;
				}
			}
		}

		// 3. 405 检测：路径匹配但方法不匹配时收集 Allow 头
		// 静态路由：O(1) 反向索引查找
		if (auto pathIt = staticPathMethods_.find(reqPath); pathIt != staticPathMethods_.end())
		{
			for (auto m : pathIt->second)
			{
				if (m != reqMethod)
				{
					if (!result.allowedMethods.empty())
					{
						result.allowedMethods += ", ";
					}
					result.allowedMethods += httpMethodToString(m);
				}
			}
		}

		// 参数路由：线性扫描其他 method 的路由（路径匹配需要模式匹配）
		ParamList tempParams;
		for (const auto& [method, routes] : paramRoutesByMethod_)
		{
			if (method == reqMethod)
			{
				continue;
			}
			for (const auto& entry : routes)
			{
				if (matchParamPath(entry.path, reqPath, tempParams))
				{
					if (!result.allowedMethods.empty())
					{
						result.allowedMethods += ", ";
					}
					result.allowedMethods += httpMethodToString(method);
					break;
				}
			}
		}

		return result;
	}

	size_t Router::routeCount() const
	{
		size_t paramCount = 0;
		for (const auto& [method, routes] : paramRoutesByMethod_)
		{
			paramCount += routes.size();
		}
		return staticRoutes_.size() + paramCount + wsRoutes_.size();
	}

	// ============ 辅助方法 ============

	bool Router::isParamRoute(const std::string& path)
	{
		return path.find('{') != std::string::npos;
	}

	bool Router::matchParamPath(std::string_view pattern, std::string_view path, ParamList& params)
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
			if (++segmentCount > Router::kMaxPathSegments)
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
				if (reqSeg.size() > Router::kMaxParamValueLength)
				{
					params.clear();
					return false;
				}
				// 参数段：提取参数名和值
				auto paramName = patSeg.substr(1, patSeg.size() - 2);
				params.emplace_back(std::string(paramName), std::string(reqSeg));
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

	std::string Router::urlDecode(std::string_view encoded)
	{
		std::string result;
		result.reserve(encoded.size());

		for (size_t i = 0; i < encoded.size(); ++i)
		{
			if (encoded[i] == '%' && i + 2 < encoded.size())
			{
				auto hi = encoded[i + 1];
				auto lo = encoded[i + 2];

				auto hexVal = [](char c) -> int
				{
					if (c >= '0' && c <= '9')
					{
						return c - '0';
					}
					if (c >= 'A' && c <= 'F')
					{
						return c - 'A' + 10;
					}
					if (c >= 'a' && c <= 'f')
					{
						return c - 'a' + 10;
					}
					return -1;
				};

				int hiVal = hexVal(hi);
				int loVal = hexVal(lo);
				if (hiVal >= 0 && loVal >= 0)
				{
					char decoded = static_cast<char>((hiVal << 4) | loVal);
					// 防御纵深：跳过 %00 NULL 字节，防止 C API 路径截断攻击
					if (decoded != '\0')
					{
						result += decoded;
					}
					i += 2;
					continue;
				}
			}
			else if (encoded[i] == '+')
			{
				result += ' ';
				continue;
			}
			result += encoded[i];
		}
		return result;
	}

	RouteGroup Router::group(const std::string& prefix)
	{
		return RouteGroup(*this, prefix);
	}

} // namespace hical
