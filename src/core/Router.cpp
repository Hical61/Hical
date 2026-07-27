/**
 * @file Router.cpp
 * @brief 路由匹配与分发实现
 */

#include "Router.h"
#include "RouteGroup.h"

namespace hical
{

	// ============ 路由注册 ============

	void Router::route(HttpMethod method, const std::string& path, RouteHandler handler)
	{
		if (isWildcardRoute(path))
		{
			WildcardRouteEntry we;
			we.method = method;
			we.pattern = path;
			auto starPos = path.find('*');
			we.prefix = path.substr(0, starPos);
			we.paramName = path.substr(starPos + 1);
			we.handler = std::move(handler);
			wildcardRoutesByMethod_[method].push_back(std::move(we));
		}
		else if (isParamRoute(path))
		{
			paramRoutesByMethod_[method].push_back({method, path, std::move(handler), nullptr, std::nullopt});
		}
		else
		{
			staticRoutes_[{method, path}] = RouteEntry {std::move(handler), nullptr, std::nullopt};
			staticPathMethods_[path].push_back(method);
		}
	}

	void Router::route(HttpMethod method, const std::string& path, SyncRouteHandler handler)
	{
		if (isWildcardRoute(path))
		{
			WildcardRouteEntry we;
			we.method = method;
			we.pattern = path;
			auto starPos = path.find('*');
			we.prefix = path.substr(0, starPos);
			we.paramName = path.substr(starPos + 1);
			we.syncHandler = std::move(handler);
			wildcardRoutesByMethod_[method].push_back(std::move(we));
		}
		else if (isParamRoute(path))
		{
			paramRoutesByMethod_[method].push_back({method, path, nullptr, std::move(handler), std::nullopt});
		}
		else
		{
			staticRoutes_[{method, path}] = RouteEntry {nullptr, std::move(handler), std::nullopt};
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

	void Router::ws(const std::string& path,
					WsTypedMessageCallback onTypedMessage,
					WsConnectCallback onConnect,
					WsDisconnectCallback onDisconnect)
	{
		WsRoute route;
		route.path = path;
		route.onTypedMessage = std::move(onTypedMessage);
		route.onConnect = std::move(onConnect);
		route.onDisconnect = std::move(onDisconnect);
		wsRoutes_.push_back(std::move(route));
	}

	void Router::ws(const std::string& path,
					WsOptions options,
					WsTypedMessageCallback onTypedMessage,
					WsConnectCallback onConnect,
					WsDisconnectCallback onDisconnect)
	{
		WsRoute route;
		route.path = path;
		route.onTypedMessage = std::move(onTypedMessage);
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

	// ============ SSE 路由 ============

	void Router::sse(const std::string& path, SseConnectCallback onConnect)
	{
		sseRoutes_.push_back({path, std::move(onConnect)});
	}

	Router::SseRouteMatch Router::findSseRoute(std::string_view path) const
	{
		SseRouteMatch result;

		// 1. 精确匹配（快速路径）
		for (const auto& route : sseRoutes_)
		{
			if (!isParamRoute(route.path) && route.path == path)
			{
				result.route = &route;
				return result;
			}
		}

		// 2. 参数路由匹配
		for (const auto& route : sseRoutes_)
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
			if (result.staticEntry->compileTimeChain)
			{
				co_return co_await (*result.staticEntry->compileTimeChain)(req);
			}
			if (result.staticEntry->syncHandler)
			{
				co_return result.staticEntry->syncHandler(req);
			}
			co_return co_await result.staticEntry->asyncHandler(req);
		}

		if (result.paramEntry)
		{
			if (result.paramEntry->compileTimeChain)
			{
				co_return co_await (*result.paramEntry->compileTimeChain)(req);
			}
			if (result.paramEntry->syncHandler)
			{
				co_return result.paramEntry->syncHandler(req);
			}
			co_return co_await result.paramEntry->handler(req);
		}

		if (result.wildcardEntry)
		{
			if (result.wildcardEntry->compileTimeChain)
			{
				co_return co_await (*result.wildcardEntry->compileTimeChain)(req);
			}
			if (result.wildcardEntry->syncHandler)
			{
				co_return result.wildcardEntry->syncHandler(req);
			}
			co_return co_await result.wildcardEntry->handler(req);
		}

		if (!result.allowedMethods.empty())
		{
			HttpResponse res;
			res.setStatus(HttpStatusCode::hMethodNotAllowed);
			res.setHeader("Allow", result.allowedMethods);
			res.setBody("Method Not Allowed", "text/plain");
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
			if (result.staticEntry->compileTimeChain)
			{
				return std::nullopt; // 编译期链走异步路径
			}
			if (result.staticEntry->syncHandler)
			{
				return result.staticEntry->syncHandler(req);
			}
			return std::nullopt; // 异步 handler，需要 fallback 到 co_await dispatch()
		}

		if (result.paramEntry)
		{
			if (result.paramEntry->compileTimeChain)
			{
				return std::nullopt; // 编译期链走异步路径
			}
			if (result.paramEntry->syncHandler)
			{
				return result.paramEntry->syncHandler(req);
			}
			return std::nullopt; // 异步 handler
		}

		if (result.wildcardEntry)
		{
			if (result.wildcardEntry->compileTimeChain)
			{
				return std::nullopt; // 编译期链走异步路径
			}
			if (result.wildcardEntry->syncHandler)
			{
				return result.wildcardEntry->syncHandler(req);
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

		// 1a. 完美哈希优先查找（如果注入）
		if (phrLookup_.valid())
		{
			size_t idx = phrLookup_.lookup(reqMethod, reqPath);
			if (idx != SIZE_MAX && idx < phrEntryMap_->size())
			{
				result.staticEntry = (*phrEntryMap_)[idx];
				return result;
			}
		}

		// 1b. 回退到运行时哈希表查找（O(1) 哈希查找，透明哈希避免构造临时 std::string）
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

		// 3. wildcard route matching
		if (auto wGroupIt = wildcardRoutesByMethod_.find(reqMethod); wGroupIt != wildcardRoutesByMethod_.end())
		{
			for (const auto& entry : wGroupIt->second)
			{
				if (reqPath.size() >= entry.prefix.size() && reqPath.starts_with(entry.prefix))
				{
					ParamList params;
					matchWildcardPath(entry.prefix, entry.paramName, reqPath, params);
					for (const auto& [name, value] : params)
					{
						req.setParam(name, value);
					}
					result.wildcardEntry = &entry;
					return result;
				}
			}
		}

		// 4. 405 检测：路径匹配但方法不匹配时收集 Allow 头
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

	bool Router::exists(HttpMethod method, std::string_view path) const
	{
		// URL decode（与 resolveRoute 保持一致）
		std::string decodedStorage;
		std::string_view reqPath;
		bool needsDecode = false;
		for (char c : path)
		{
			if (c == '%' || c == '+')
			{
				needsDecode = true;
				break;
			}
		}
		if (needsDecode)
		{
			decodedStorage = urlDecode(path);
			reqPath = decodedStorage;
		}
		else
		{
			reqPath = path;
		}

		// 1. 静态路由 O(1) 查找
		if (staticRoutes_.find(RouteKeyView {method, reqPath}) != staticRoutes_.end())
		{
			return true;
		}

		// 2. 参数路由匹配（仅同 method）
		if (auto groupIt = paramRoutesByMethod_.find(method); groupIt != paramRoutesByMethod_.end())
		{
			ParamList dummy;
			for (const auto& entry : groupIt->second)
			{
				if (matchParamPath(entry.path, reqPath, dummy))
				{
					return true;
				}
			}
		}

		return false;
	}

	size_t Router::routeCount() const
	{
		size_t paramCount = 0;
		for (const auto& [method, routes] : paramRoutesByMethod_)
		{
			paramCount += routes.size();
		}
		size_t wildcardCount = 0;
		for (const auto& [method, routes] : wildcardRoutesByMethod_)
		{
			wildcardCount += routes.size();
		}
		return staticRoutes_.size() + paramCount + wildcardCount + wsRoutes_.size() + sseRoutes_.size();
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

	bool Router::isWildcardRoute(const std::string& path)
	{
		auto starPos = path.find('*');
		return starPos != std::string::npos && starPos + 1 < path.size();
	}

	bool Router::matchWildcardPath(std::string_view prefix,
								   std::string_view paramName,
								   std::string_view path,
								   ParamList& params)
	{
		params.clear();
		auto captured = path.substr(prefix.size());
		if (captured.size() > Router::kMaxParamValueLength)
		{
			return false;
		}
		if (!paramName.empty())
		{
			params.emplace_back(std::string(paramName), std::string(captured));
		}
		return true;
	}

	namespace
	{
		/// 256 条查表，hex 字符转数值，0xFF 就是非法。
		/// 编译期搭好，运行时查一下就行，不用走分支。
		inline const unsigned char* hexLookup() noexcept
		{
			static constexpr auto build = []() constexpr
			{
				std::array<unsigned char, 256> arr {};
				for (unsigned i = 0; i < 256; ++i)
				{
					arr[i] = 0xFF;
				}
				for (unsigned i = '0'; i <= '9'; ++i)
				{
					arr[i] = static_cast<unsigned char>(i - '0');
				}
				for (unsigned i = 'A'; i <= 'F'; ++i)
				{
					arr[i] = static_cast<unsigned char>(i - 'A' + 10);
				}
				for (unsigned i = 'a'; i <= 'f'; ++i)
				{
					arr[i] = static_cast<unsigned char>(i - 'a' + 10);
				}
				return arr;
			};
			static constexpr auto kTable = build();
			return kTable.data();
		}
	} // namespace

	std::string Router::urlDecode(std::string_view encoded)
	{
		std::string result;
		result.reserve(encoded.size());
		const auto* hexTbl = hexLookup();

		for (size_t i = 0; i < encoded.size(); ++i)
		{
			if (encoded[i] == '%' && i + 2 < encoded.size())
			{
				auto hi = hexTbl[static_cast<unsigned char>(encoded[i + 1])];
				auto lo = hexTbl[static_cast<unsigned char>(encoded[i + 2])];
				if (hi != 0xFF && lo != 0xFF)
				{
					char decoded = static_cast<char>((hi << 4) | lo);
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

	void Router::setPerfectHashLookup(RuntimePerfectHashLookup lookup)
	{
		// 构建 index -> RouteEntry* 映射
		auto entryMap = std::make_shared<std::vector<const RouteEntry*>>();
		entryMap->resize(lookup.keyCount(), nullptr);
		phrLookup_ = std::move(lookup);

		// 遍历 staticRoutes_：所有静态路由的 entry 都是稳定的（unordered_map 节点引用稳定）
		for (auto& [key, entry] : staticRoutes_)
		{
			size_t idx = phrLookup_.lookup(key.method, key.path);
			if (idx != SIZE_MAX && idx < entryMap->size())
			{
				(*entryMap)[idx] = &entry;
			}
		}

		phrEntryMap_ = std::move(entryMap);
	}

} // namespace hical
