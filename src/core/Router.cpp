/**
 * @file Router.cpp
 * @brief 路由匹配与分发实现
 */

#include "Router.h"
#include "RouteGroup.h"

#include <iterator>

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
			indexRouteFirstSegment(method, path, RouteKind::hWildcard);
		}
		else if (isParamRoute(path))
		{
			paramRoutesByMethod_[method].push_back({method, path, std::move(handler), nullptr, std::nullopt});
			indexRouteFirstSegment(method, path, RouteKind::hParam);
		}
		else
		{
			staticRoutes_[{method, path}] = RouteEntry {std::move(handler), nullptr, std::nullopt};
			staticPathMethods_[path].push_back(method);
			indexRouteFirstSegment(method, path, RouteKind::hStatic);
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
			indexRouteFirstSegment(method, path, RouteKind::hWildcard);
		}
		else if (isParamRoute(path))
		{
			paramRoutesByMethod_[method].push_back({method, path, nullptr, std::move(handler), std::nullopt});
			indexRouteFirstSegment(method, path, RouteKind::hParam);
		}
		else
		{
			staticRoutes_[{method, path}] = RouteEntry {nullptr, std::move(handler), std::nullopt};
			staticPathMethods_[path].push_back(method);
			indexRouteFirstSegment(method, path, RouteKind::hStatic);
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
		// resolveRoute 结果存局部变量：协程挂起时临时对象必须存活，不能直接内联进参数
		ResolveResult result = resolveRoute(req);
		co_return co_await dispatchResolved(req, result);
	}

	std::optional<HttpResponse> Router::dispatchSync(HttpRequest& req)
	{
		ResolveResult result = resolveRoute(req);
		return dispatchSyncResolved(req, result);
	}

	Awaitable<HttpResponse> Router::dispatchResolved(HttpRequest& req, const ResolveResult& result)
	{
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

	std::optional<HttpResponse> Router::dispatchSyncResolved(HttpRequest& req, const ResolveResult& result)
	{
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
			return std::nullopt; // 异步 handler，需要 fallback 到 co_await dispatchResolved()
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

	namespace
	{
		// 真实方法列表，只列这些；hUnknown 是解析兜底值，不该出现在 Allow 里
		constexpr HttpMethod kMethods[] = {HttpMethod::hGet,
										   HttpMethod::hPost,
										   HttpMethod::hPut,
										   HttpMethod::hDelete,
										   HttpMethod::hPatch,
										   HttpMethod::hHead,
										   HttpMethod::hOptions};

		// Allow 头就是靠这张手写表拼的：methodBit() 会给任何枚举值置位，但只有列进
		// kMethods 的才会被打印。哪天有人在 hOptions 后面插个 hTrace 却忘了同步这张表，
		// 位照样置上、方法却列不出来——不报错也不崩，只是 405 的 Allow 头悄悄少一个方法。
		// 下面这几条把「表 == 枚举全集，且逐项按枚举顺序」钉死在编译期。
		static_assert(std::size(kMethods) == static_cast<size_t>(HttpMethod::hUnknown),
					  "新增 HttpMethod 时必须同步 kMethods，否则 Allow 头会静默漏方法");
		static_assert(
			[]() constexpr
			{
				for (size_t i = 0; i < std::size(kMethods); ++i)
				{
					if (kMethods[i] != static_cast<HttpMethod>(i))
					{
						return false;
					}
				}
				return true;
			}(),
			"kMethods 必须按枚举顺序完整列出 hGet..hOptions，错位一样会让 Allow 头列错方法");
		// allowMask 是 32 位掩码，枚举值一旦超过 32 个，移位就撞上 UB 了
		static_assert(static_cast<size_t>(HttpMethod::hUnknown) <= 32,
					  "HttpMethod 枚举值超过 32 个，methodBit() 的移位会溢出");

		/**
		 * @brief 把方法映射成掩码里的 1 bit
		 */
		inline uint32_t methodBit(HttpMethod method) noexcept
		{
			return 1u << static_cast<unsigned>(method);
		}

		/**
		 * @brief 掩码拼回 Allow 头字符串，按枚举顺序输出
		 */
		std::string allowedMethodsFromMask(uint32_t mask)
		{
			std::string out;
			for (auto method : kMethods)
			{
				if ((mask & methodBit(method)) == 0)
				{
					continue;
				}
				if (!out.empty())
				{
					out += ", ";
				}
				out += httpMethodToString(method);
			}
			return out;
		}

		/**
		 * @brief 取路径首段：丢掉前导 '/'，切到下一个 '/' 为止
		 * 切段规则必须和 matchParamPath 一致，否则索引算出来的首段和实际匹配用的首段对不上
		 */
		std::string_view firstPathSegment(std::string_view path) noexcept
		{
			if (!path.empty() && path.front() == '/')
			{
				path.remove_prefix(1);
			}
			auto slash = path.find('/');
			return slash == std::string_view::npos ? path : path.substr(0, slash);
		}

		/**
		 * @brief 判断一个路径段是不是参数段（{name}）
		 * 判定条件抄的 matchParamPath，改那边记得同步这里
		 */
		bool isParamSegment(std::string_view segment) noexcept
		{
			return segment.size() >= 3 && segment.front() == '{' && segment.back() == '}';
		}
	} // namespace

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
		// 静态索引、参数路由、通配路由三段都可能给出同一个方法（比如静态 GET 和通配 GET 并存），
		// 所以先用掩码去重，最后再一次性拼串——不然 Allow 会吐出 "GET, GET"。
		uint32_t allowMask = 0;

		// 先拿首段索引判一下「这个路径是不是在所有 method 下都没注册过」：candidateMask 是可能匹配的
		// 方法集合，为 0 就直接 404 走人，省掉下面两段对 param/wildcard 路由的全量线性扫描。
		// 命中路径在上面早就返回了，压根到不了这儿，热路径不受影响。
		auto firstSegment = firstPathSegment(reqPath);
		uint32_t candidateMask = dynamicFirstSegmentMask_;
		if (auto segIt = firstSegmentMethodMasks_.find(firstSegment); segIt != firstSegmentMethodMasks_.end())
		{
			candidateMask |= segIt->second;
		}
		for (const auto& [prefix, methods] : wildcardSegmentPrefixMasks_)
		{
			if (firstSegment.starts_with(prefix))
			{
				candidateMask |= methods;
			}
		}

		// reqMethod 自己的路由在上面已经匹配过一轮了，这里只关心别的方法
		uint32_t otherMethods = candidateMask & ~methodBit(reqMethod);
		if (otherMethods != 0)
		{
			// 静态路由：O(1) 反向索引查找
			if (auto pathIt = staticPathMethods_.find(reqPath); pathIt != staticPathMethods_.end())
			{
				for (auto m : pathIt->second)
				{
					if (m != reqMethod)
					{
						allowMask |= methodBit(m);
					}
				}
			}

			// 参数路由：线性扫描其他 method 的路由（路径匹配需要模式匹配）
			// 首段就对不上的 method 整组跳过，靠 candidateMask 筛，不然又多扫一遍
			ParamList tempParams;
			for (const auto& [method, routes] : paramRoutesByMethod_)
			{
				if ((otherMethods & methodBit(method)) == 0)
				{
					continue;
				}
				for (const auto& entry : routes)
				{
					if (matchParamPath(entry.path, reqPath, tempParams))
					{
						allowMask |= methodBit(method);
						break;
					}
				}
			}

			// 通配路由：同样线性扫其他 method，命中条件和上面的匹配阶段一致
			// （以前这段漏了，通配路由上的方法不匹配一律掉到 404，HEAD 也躺枪）
			for (const auto& [method, routes] : wildcardRoutesByMethod_)
			{
				if ((otherMethods & methodBit(method)) == 0)
				{
					continue;
				}
				for (const auto& entry : routes)
				{
					if (reqPath.size() >= entry.prefix.size() && reqPath.starts_with(entry.prefix))
					{
						allowMask |= methodBit(method);
						break;
					}
				}
			}
		}

		result.allowedMethods = allowedMethodsFromMask(allowMask);

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

	void Router::indexRouteFirstSegment(HttpMethod method, const std::string& path, RouteKind kind)
	{
		const uint32_t bit = methodBit(method);

		if (kind == RouteKind::hWildcard)
		{
			// 通配路由按前缀匹配，星号可能落在段边界（"/api/*rest" 的前缀 "/api/"），
			// 也可能落在段中间（"/files*x" 的前缀 "/files"），两种情况归档方式不一样
			auto starPos = path.find('*');
			std::string_view prefix = std::string_view(path).substr(0, starPos);

			// 前缀不以 '/' 开头（正常注册不会出现）时 starts_with 的语义没法用首段描述，
			// 直接当「任意首段都可能匹配」，宁可少优化也别把该报 405 的路径误判成 404
			if (prefix.empty() || prefix.front() != '/')
			{
				dynamicFirstSegmentMask_ |= bit;
				return;
			}

			auto rest = prefix.substr(1);
			auto slash = rest.find('/');
			if (slash == std::string_view::npos)
			{
				// 前缀没跨过首段，星号在段中间：任何以 rest 开头的首段都可能命中
				indexWildcardSegmentPrefix(method, rest);
				return;
			}
			firstSegmentMethodMasks_[std::string(rest.substr(0, slash))] |= bit;
			return;
		}

		auto segment = firstPathSegment(path);
		if (kind == RouteKind::hParam && isParamSegment(segment))
		{
			// 首段本身就是 {param}，那什么首段都可能是它匹配上的
			dynamicFirstSegmentMask_ |= bit;
			return;
		}
		firstSegmentMethodMasks_[std::string(segment)] |= bit;
	}

	void Router::indexWildcardSegmentPrefix(HttpMethod method, std::string_view prefix)
	{
		const uint32_t bit = methodBit(method);
		for (auto& [token, mask] : wildcardSegmentPrefixMasks_)
		{
			if (std::string_view(token) == prefix)
			{
				mask |= bit;
				return;
			}
		}
		wildcardSegmentPrefixMasks_.emplace_back(std::string(prefix), bit);
	}

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
