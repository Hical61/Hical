/**
 * @file Router.h
 * @brief 路由分发（静态 O(1) + 参数路由 + 编译期完美哈希可选增强）
 */

#pragma once

#include "HttpTypes.h"
#include "HttpRequest.h"
#include "HttpResponse.h"
#include "CompileTimeChain.h"
#include "Coroutine.h"
#include "Middleware.h"
#include "PerfectHashRouter.h"

#include <chrono>
#include <concepts>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace hical
{

	class RouteGroup; // 前向声明
	class SseSession; // 前向声明

	/**
	 * @brief 路由处理器类型（协程版）
	 * 接收请求，返回协程化的响应。
	 */
	using RouteHandler = std::function<Awaitable<HttpResponse>(const HttpRequest&)>;

	/**
	 * @brief 同步路由处理器类型
	 * 接收请求，直接返回响应（非协程）。
	 */
	using SyncRouteHandler = std::function<HttpResponse(const HttpRequest&)>;

	class WebSocketSession; // 前向声明
	struct WsMessage;       // 前向声明

	/**
	 * @brief WebSocket 消息回调类型（文本，向后兼容）
	 */
	using WsMessageCallback = std::function<Awaitable<void>(const std::string&, WebSocketSession&)>;

	/**
	 * @brief WebSocket 类型消息回调（区分 Text/Binary）
	 */
	using WsTypedMessageCallback = std::function<Awaitable<void>(const WsMessage&, WebSocketSession&)>;

	/**
	 * @brief WebSocket 连接回调类型
	 */
	using WsConnectCallback = std::function<Awaitable<void>(WebSocketSession&)>;

	/**
	 * @brief WebSocket 断开回调类型
	 */
	using WsDisconnectCallback = std::function<Awaitable<void>(WebSocketSession&)>;

	// ============ SSE 回调类型 ============

	/**
	 * @brief SSE 连接建立回调类型
	 */
	using SseConnectCallback = std::function<Awaitable<void>(std::shared_ptr<SseSession>)>;

	/**
	 * @brief HTTP 路由器
	 * 管理路由注册和请求分发。
	 * 静态路由使用哈希表 O(1) 查找，参数路由线性匹配。
	 * 支持协程和同步两种处理器风格。
	 * 可选注入运行时完美哈希表加速静态路由查找。
	 */
	class Router
	{
	public:
		// 路径参数安全限制
		static constexpr size_t kMaxPathSegments = 32;
		static constexpr size_t kMaxParamValueLength = 1024;

		// 参数列表类型：用 vector<pair> 替代 unordered_map，减少堆分配
		using ParamList = std::vector<std::pair<std::string, std::string>>;

		Router() = default;

		/**
		 * @brief 注册协程路由处理器
		 * @param method HTTP 方法
		 * @param path 路由路径（如 "/api/users"）
		 * @param handler 协程处理器
		 */
		void route(HttpMethod method, const std::string& path, RouteHandler handler);

		/**
		 * @brief 注册同步路由处理器（自动包装为协程）
		 * @param method HTTP 方法
		 * @param path 路由路径
		 * @param handler 同步处理器
		 */
		void route(HttpMethod method, const std::string& path, SyncRouteHandler handler);

		/**
		 * @brief 注册带编译期中间件链的路由（编译期展开调用链）
		 * @tparam Entries CompileTimeAsyncMw / CompileTimeSyncMw / CompileTimeSyncFullMw 类型列表
		 * @param method HTTP 方法
		 * @param path 路由路径
		 * @param handler 路由处理器（异步）
		 * @note 编译期链在 dispatch() 中直接调用，跳过 MiddlewarePipeline::execute()。
		 *       全局中间件仍由 HttpSessionImpl 层包裹在最外层。
		 */
		template <typename... Entries>
		void compileTimeRoute(HttpMethod method, const std::string& path, RouteHandler handler);

		/**
		 * @brief 注册带编译期中间件链的同步路由
		 * @tparam Entries 编译期中间件条目类型列表
		 */
		template <typename... Entries>
		void compileTimeRoute(HttpMethod method, const std::string& path, SyncRouteHandler handler);

		// ============ 便捷方法 ============

		/**
		 * @brief 注册 GET 路由（协程处理器）
		 */
		void get(const std::string& path, RouteHandler handler);
		/**
		 * @brief 注册 GET 路由（同步处理器）
		 */
		void get(const std::string& path, SyncRouteHandler handler);

		/**
		 * @brief 注册 POST 路由（协程处理器）
		 */
		void post(const std::string& path, RouteHandler handler);
		/**
		 * @brief 注册 POST 路由（同步处理器）
		 */
		void post(const std::string& path, SyncRouteHandler handler);

		/**
		 * @brief 注册 PUT 路由（协程处理器）
		 */
		void put(const std::string& path, RouteHandler handler);
		/**
		 * @brief 注册 PUT 路由（同步处理器）
		 */
		void put(const std::string& path, SyncRouteHandler handler);

		/**
		 * @brief 注册 DELETE 路由（协程处理器）
		 */
		void del(const std::string& path, RouteHandler handler);
		/**
		 * @brief 注册 DELETE 路由（同步处理器）
		 */
		void del(const std::string& path, SyncRouteHandler handler);

		/**
		 * @brief WebSocket 路由选项
		 */
		struct WsOptions
		{
			/// 允许的 Origin 列表（防 CSWSH）。为空时不校验 Origin。
			std::unordered_set<std::string, StringHash, StringEqual> allowedOrigins;

			/// 是否启用 permessage-deflate 压缩（默认关闭，向后兼容）
			bool enableCompression = false;

			/// zlib 服务端最大窗口位数 (9-15，默认 15)
			int serverMaxWindowBits = 15;

			/// zlib 客户端最大窗口位数 (9-15，默认 15)
			int clientMaxWindowBits = 15;

			/// 每消息独立压缩（省内存但降低压缩率）
			bool serverNoContextTakeover = false;

			/// 心跳 Ping 间隔（0 = 禁用）
			std::chrono::seconds pingInterval {0};

			/// 最大连续未收到 Pong 次数，超过则关闭（默认 3）
			uint32_t maxMissedPongs = 3;

			/// 自定义 Ping 载荷（空 = 零长度 Ping，最大 125 字节）
			std::string pingPayload;

			/// 支持的子协议列表
			std::vector<std::string> subprotocols;
		};

		struct WsRoute
		{
			std::string path;
			WsMessageCallback onMessage;
			WsTypedMessageCallback onTypedMessage; ///< 可选，优先于 onMessage
			WsConnectCallback onConnect;
			WsDisconnectCallback onDisconnect;
			std::unordered_set<std::string, StringHash, StringEqual> allowedOrigins;

			/// 压缩配置
			bool enableCompression = false;
			int serverMaxWindowBits = 15;
			int clientMaxWindowBits = 15;
			bool serverNoContextTakeover = false;

			/// 心跳配置
			std::chrono::seconds pingInterval {0};
			uint32_t maxMissedPongs = 3;
			std::string pingPayload;

			/// 子协议
			std::vector<std::string> subprotocols;
		};

		/**
		 * @brief 注册 WebSocket 路由
		 */
		void ws(const std::string& path,
				WsMessageCallback onMessage,
				WsConnectCallback onConnect = nullptr,
				WsDisconnectCallback onDisconnect = nullptr);

		/**
		 * @brief 注册 WebSocket 路由（带安全选项）
		 */
		void ws(const std::string& path,
				WsOptions options,
				WsMessageCallback onMessage,
				WsConnectCallback onConnect = nullptr,
				WsDisconnectCallback onDisconnect = nullptr);

		/**
		 * @brief 注册 WebSocket 路由（类型感知回调）
		 */
		void ws(const std::string& path,
				WsTypedMessageCallback onTypedMessage,
				WsConnectCallback onConnect = nullptr,
				WsDisconnectCallback onDisconnect = nullptr);

		/**
		 * @brief 注册 WebSocket 路由（类型感知回调，带安全选项）
		 */
		void ws(const std::string& path,
				WsOptions options,
				WsTypedMessageCallback onTypedMessage,
				WsConnectCallback onConnect = nullptr,
				WsDisconnectCallback onDisconnect = nullptr);

		// ============ SSE 路由 ============

		struct SseRoute
		{
			std::string path;
			SseConnectCallback onConnect;
		};

		/**
		 * @brief 注册 SSE 路由
		 */
		void sse(const std::string& path, SseConnectCallback onConnect);

		struct SseRouteMatch
		{
			const SseRoute* route = nullptr;
			ParamList params;
		};

		[[nodiscard]] SseRouteMatch findSseRoute(std::string_view path) const;

		/**
		 * @brief 分发请求到匹配的路由处理器
		 */
		[[nodiscard]] Awaitable<HttpResponse> dispatch(HttpRequest& req);

		/**
		 * @brief 同步快速路径分发（零协程帧开销）
		 */
		[[nodiscard]] std::optional<HttpResponse> dispatchSync(HttpRequest& req);

		struct WsRouteMatch
		{
			const WsRoute* route = nullptr;
			ParamList params;
		};

		[[nodiscard]] WsRouteMatch findWsRoute(std::string_view path) const;

		/**
		 * @brief 检查指定方法+路径的路由是否存在（Expect: 100-continue 准入预检用）
		 */
		[[nodiscard]] bool exists(HttpMethod method, std::string_view path) const;

		/**
		 * @brief 获取已注册路由数量（HTTP + WebSocket）
		 */
		[[nodiscard]] size_t routeCount() const;

		/**
		 * @brief 创建路由组（前缀分组）
		 */
		[[nodiscard]] RouteGroup group(const std::string& prefix);

		/**
		 * @brief URL 解码
		 */
		[[nodiscard]] static std::string urlDecode(std::string_view encoded);

		// ============ 静态路由数据结构 ============

		struct RouteKey
		{
			HttpMethod method;
			std::string path;

			bool operator==(const RouteKey& other) const
			{
				return method == other.method && path == other.path;
			}
		};

		struct RouteKeyView
		{
			HttpMethod method;
			std::string_view path;
		};

		struct RouteKeyHash
		{
			using is_transparent = void;

			static size_t combine(size_t h1, size_t h2)
			{
				h1 ^= h2 + 0x9e3779b9 + (h1 << 6) + (h1 >> 2);
				return h1;
			}

			size_t operator()(const RouteKey& key) const
			{
				return combine(std::hash<int> {}(static_cast<int>(key.method)), std::hash<std::string> {}(key.path));
			}

			size_t operator()(const RouteKeyView& key) const
			{
				return combine(std::hash<int> {}(static_cast<int>(key.method)),
							   std::hash<std::string_view> {}(key.path));
			}
		};

		struct RouteKeyEqual
		{
			using is_transparent = void;

			bool operator()(const RouteKey& a, const RouteKey& b) const
			{
				return a.method == b.method && a.path == b.path;
			}

			bool operator()(const RouteKeyView& a, const RouteKey& b) const
			{
				return a.method == b.method && a.path == b.path;
			}

			bool operator()(const RouteKey& a, const RouteKeyView& b) const
			{
				return a.method == b.method && a.path == b.path;
			}
		};

		struct RouteEntry
		{
			RouteHandler asyncHandler;
			SyncRouteHandler syncHandler;
			/**
			 * @brief 编译期预构建的中间件调用链
			 * 若非空，dispatch() 直接调用此链（跳过 MiddlewarePipeline::execute()）。
			 * 注意：此链不含组级中间件，组级中间件在 RouteGroup::compileTimeRoute() 中外层包裹。
			 */
			std::optional<MiddlewareNext> compileTimeChain;
		};

		/**
		 * @brief 注入运行时完美哈希加速查找（可选增强）
		 * 由 MetaRoutes::registerRoutes() 自动调用，用户无需手动操作。
		 * 命中时跳过 unordered_map，miss 时透明回退。
		 */
		void setPerfectHashLookup(RuntimePerfectHashLookup lookup);

	private:
		std::unordered_map<RouteKey, RouteEntry, RouteKeyHash, RouteKeyEqual> staticRoutes_;

		struct ParamRouteEntry
		{
			HttpMethod method;
			std::string path;
			RouteHandler handler;
			SyncRouteHandler syncHandler;
			std::optional<MiddlewareNext> compileTimeChain;
		};

		std::unordered_map<HttpMethod, std::vector<ParamRouteEntry>> paramRoutesByMethod_;

		struct WildcardRouteEntry
		{
			HttpMethod method;
			std::string pattern;
			std::string prefix;
			std::string paramName;
			RouteHandler handler;
			SyncRouteHandler syncHandler;
			std::optional<MiddlewareNext> compileTimeChain;
		};

		std::unordered_map<HttpMethod, std::vector<WildcardRouteEntry>> wildcardRoutesByMethod_;

		struct ResolveResult
		{
			const RouteEntry* staticEntry = nullptr;
			const ParamRouteEntry* paramEntry = nullptr;
			const WildcardRouteEntry* wildcardEntry = nullptr;
			std::string allowedMethods;
			bool pathTooDeep = false;
		};

		ResolveResult resolveRoute(HttpRequest& req) const;

		std::unordered_map<std::string, std::vector<HttpMethod>, StringHash, StringEqual> staticPathMethods_;

		std::vector<WsRoute> wsRoutes_;

		std::vector<SseRoute> sseRoutes_;

		// ============ 运行时完美哈希可选加速 ============

		/// 完美哈希查找器（valid() == false 时退化为未启用）
		RuntimePerfectHashLookup phrLookup_;
		/// index -> RouteEntry* 映射（和 phrLookup_ 的 index_ 字段对应）
		std::shared_ptr<std::vector<const RouteEntry*>> phrEntryMap_;

		static bool isParamRoute(const std::string& path);
		static bool matchParamPath(std::string_view pattern, std::string_view path, ParamList& params);
		static bool isWildcardRoute(const std::string& path);
		static bool matchWildcardPath(std::string_view prefix,
									  std::string_view paramName,
									  std::string_view path,
									  ParamList& params);
	};

/**
 * @brief 路由注册宏（手动注册的便捷方式）
 */
#define HICAL_ROUTE(router, method, path, handler) (router).route(::hical::HttpMethod::h##method, path, handler)

	// ============ compileTimeRoute 模板实现 ============

	template <typename... Entries>
	void Router::compileTimeRoute(HttpMethod method, const std::string& path, RouteHandler handler)
	{
		MiddlewareNext finalNext = [h = std::move(handler)](HttpRequest& req) -> Awaitable<HttpResponse>
		{
			co_return co_await h(req);
		};

		auto chain = buildCompileTimeChain<Entries...>(std::move(finalNext));

		if (isWildcardRoute(path))
		{
			WildcardRouteEntry we;
			we.method = method;
			we.pattern = path;
			auto starPos = path.find('*');
			we.prefix = path.substr(0, starPos);
			we.paramName = path.substr(starPos + 1);
			we.compileTimeChain = std::move(chain);
			wildcardRoutesByMethod_[method].push_back(std::move(we));
		}
		else if (isParamRoute(path))
		{
			paramRoutesByMethod_[method].push_back({method, path, nullptr, nullptr, std::move(chain)});
		}
		else
		{
			staticRoutes_[{method, path}] = RouteEntry {nullptr, nullptr, std::move(chain)};
			staticPathMethods_[path].push_back(method);
		}
	}

	template <typename... Entries>
	void Router::compileTimeRoute(HttpMethod method, const std::string& path, SyncRouteHandler handler)
	{
		MiddlewareNext finalNext = [h = std::move(handler)](HttpRequest& req) -> Awaitable<HttpResponse>
		{
			co_return h(req);
		};

		auto chain = buildCompileTimeChain<Entries...>(std::move(finalNext));

		if (isWildcardRoute(path))
		{
			WildcardRouteEntry we;
			we.method = method;
			we.pattern = path;
			auto starPos = path.find('*');
			we.prefix = path.substr(0, starPos);
			we.paramName = path.substr(starPos + 1);
			we.compileTimeChain = std::move(chain);
			wildcardRoutesByMethod_[method].push_back(std::move(we));
		}
		else if (isParamRoute(path))
		{
			paramRoutesByMethod_[method].push_back({method, path, nullptr, nullptr, std::move(chain)});
		}
		else
		{
			staticRoutes_[{method, path}] = RouteEntry {nullptr, nullptr, std::move(chain)};
			staticPathMethods_[path].push_back(method);
		}
	}

} // namespace hical
