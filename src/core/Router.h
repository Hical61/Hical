#pragma once

#include "HttpTypes.h"
#include "HttpRequest.h"
#include "HttpResponse.h"
#include "Coroutine.h"
#include <concepts>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace hical
{

	/**
 * @brief 路由处理器类型（协程版）
 *
 * 接收请求，返回协程化的响应。
 */
	using RouteHandler = std::function<Awaitable<HttpResponse>(const HttpRequest&)>;

	/**
 * @brief 同步路由处理器类型
 *
 * 接收请求，直接返回响应（非协程）。
 */
	using SyncRouteHandler = std::function<HttpResponse(const HttpRequest&)>;

	class WebSocketSession; // 前向声明

	/**
 * @brief WebSocket 消息回调类型
 */
	using WsMessageCallback = std::function<Awaitable<void>(const std::string&, WebSocketSession&)>;

	/**
 * @brief WebSocket 连接回调类型
 */
	using WsConnectCallback = std::function<Awaitable<void>(WebSocketSession&)>;

	/**
 * @brief WebSocket 断开回调类型
 */
	using WsDisconnectCallback = std::function<Awaitable<void>(WebSocketSession&)>;

	/**
 * @brief HTTP 路由器
 *
 * 管理路由注册和请求分发。
 * 静态路由使用哈希表 O(1) 查找，参数路由线性匹配。
 * 支持协程和同步两种处理器风格。
 */
	class Router
	{
	public:
		// 路径参数安全限制
		static constexpr size_t hMaxPathSegments = 32;
		static constexpr size_t hMaxParamValueLength = 1024;

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

		// ============ 便捷方法 ============

		void get(const std::string& path, RouteHandler handler);
		void get(const std::string& path, SyncRouteHandler handler);

		void post(const std::string& path, RouteHandler handler);
		void post(const std::string& path, SyncRouteHandler handler);

		void put(const std::string& path, RouteHandler handler);
		void put(const std::string& path, SyncRouteHandler handler);

		void del(const std::string& path, RouteHandler handler);
		void del(const std::string& path, SyncRouteHandler handler);

		/**
     * @brief 注册 WebSocket 路由
     * @param path 路由路径
     * @param onMessage 消息回调
     * @param onConnect 连接建立回调（可选）
     * @param onDisconnect 连接断开回调（可选）
     */
		void ws(const std::string& path,
				WsMessageCallback onMessage,
				WsConnectCallback onConnect = nullptr,
				WsDisconnectCallback onDisconnect = nullptr);

		/**
     * @brief 分发请求到匹配的路由处理器
     * @param req HTTP 请求（路径参数会被写入 req 中）
     * @return 协程化的 HTTP 响应
     *
     * 如果没有匹配的路由，返回 404 Not Found。
     */
		Awaitable<HttpResponse> dispatch(HttpRequest& req);

		/**
     * @brief 检查路径是否为 WebSocket 路由
     * @param path 请求路径
     * @return 如果是 ws 路由返回对应的 WsRoute 指针，否则 nullptr
     */
		struct WsRoute
		{
			std::string path;
			WsMessageCallback onMessage;
			WsConnectCallback onConnect;
			WsDisconnectCallback onDisconnect;
		};

		const WsRoute* findWsRoute(const std::string& path) const;

		/**
     * @brief 获取已注册路由数量（HTTP + WebSocket）
     * @return 路由数量
     */
		size_t routeCount() const;

	private:
		// ============ 静态路由（哈希表 O(1) 查找） ============

		/**
     * @brief 组合键：method + path，用于静态路由哈希查找
     */
		struct RouteKey
		{
			HttpMethod method;
			std::string path;

			bool operator==(const RouteKey& other) const
			{
				return method == other.method && path == other.path;
			}
		};

		/// 轻量级 view 类型，用于 find() 异构查找，避免构造临时 std::string
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

		std::unordered_map<RouteKey, RouteHandler, RouteKeyHash, RouteKeyEqual> staticRoutes_;

		// ============ 参数路由（线性匹配） ============

		struct ParamRouteEntry
		{
			HttpMethod method;
			std::string path;
			RouteHandler handler;
		};

		std::vector<ParamRouteEntry> paramRoutes_;

		// ============ WebSocket 路由 ============

		std::vector<WsRoute> wsRoutes_;

		/**
     * @brief 判断路径模式是否包含参数（如 {id}）
     * @param path 路径模式
     * @return true 如果是参数路由
     */
		static bool isParamRoute(const std::string& path);

		/**
     * @brief 匹配参数路径并提取参数（零分配 string_view 版本）
     * @param pattern 注册的路径模式（可含 {param}）
     * @param path 请求路径
     * @param params 提取的路径参数（输出）
     * @return true 如果匹配
     */
		static bool matchParamPath(std::string_view pattern, std::string_view path, ParamList& params);

		/**
     * @brief URL 解码（百分号编码 -> 原始字符）
     * @param encoded 编码后的字符串
     * @return 解码后的字符串
     */
		static std::string urlDecode(std::string_view encoded);
	};

/**
 * @brief 路由注册宏（手动注册的便捷方式）
 *
 * 用法：HICAL_ROUTE(router, Get, "/api/users", myHandler)
 *
 * 等价于 router.route(HttpMethod::hGet, "/api/users", myHandler);
 *
 * 如需自动路由注册，请参考 MetaRoutes.h 中的反射方案：
 *   hical::meta::registerRoutes<UserHandler>(router);
 */
#define HICAL_ROUTE(router, method, path, handler) (router).route(::hical::HttpMethod::h##method, path, handler)

} // namespace hical
