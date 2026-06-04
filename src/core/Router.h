/**
 * @file Router.h
 * @brief 路由分发（静态 O(1) + 参数路由）
 */

#pragma once

#include "HttpTypes.h"
#include "HttpRequest.h"
#include "HttpResponse.h"
#include "Coroutine.h"
#include <chrono>
#include <concepts>
#include <functional>
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

	/**
	 * @brief HTTP 路由器
	 * 管理路由注册和请求分发。
	 * 静态路由使用哈希表 O(1) 查找，参数路由线性匹配。
	 * 支持协程和同步两种处理器风格。
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
			/// 透明哈希：string_view 查找零临时 string 构造。
			std::unordered_set<std::string, StringHash, StringEqual> allowedOrigins;

			/// 是否启用 permessage-deflate 压缩（默认关闭，向后兼容）
			bool enableCompression = false;

			/// zlib 服务端最大窗口位数 (9-15，默认 15)
			int serverMaxWindowBits = 15;

			/// zlib 客户端最大窗口位数 (9-15，默认 15)
			int clientMaxWindowBits = 15;

			/// 每消息独立压缩（省内存但降低压缩率）
			bool serverNoContextTakeover = false;

			/// 心跳 Ping 间隔（0 = 禁用，默认 0 向后兼容）
			std::chrono::seconds pingInterval {0};

			/// 最大连续未收到 Pong 次数，超过则关闭（默认 3）
			uint32_t maxMissedPongs = 3;

			/// 自定义 Ping 载荷（空 = 零长度 Ping，最大 125 字节）
			std::string pingPayload;

			/// 支持的子协议列表（从客户端 offer 中选择第一个匹配）
			/// 空 = 忽略 Sec-WebSocket-Protocol 头
			std::vector<std::string> subprotocols;
		};

		struct WsRoute
		{
			std::string path;
			WsMessageCallback onMessage;
			WsTypedMessageCallback onTypedMessage; ///< 可选，优先于 onMessage（区分 Text/Binary）
			WsConnectCallback onConnect;
			WsDisconnectCallback onDisconnect;
			std::unordered_set<std::string, StringHash, StringEqual>
				allowedOrigins; ///< 允许的 Origin 列表（空 = 不校验，透明哈希）

			/// 压缩配置（从 WsOptions 复制）
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
		 * @brief 注册 WebSocket 路由（带安全选项）
		 * @param path 路由路径
		 * @param options WebSocket 选项（Origin 白名单等）
		 * @param onMessage 消息回调
		 * @param onConnect 连接建立回调（可选）
		 * @param onDisconnect 连接断开回调（可选）
		 */
		void ws(const std::string& path,
				WsOptions options,
				WsMessageCallback onMessage,
				WsConnectCallback onConnect = nullptr,
				WsDisconnectCallback onDisconnect = nullptr);

		/**
		 * @brief 分发请求到匹配的路由处理器
		 * @param req HTTP 请求（路径参数会被写入 req 中）
		 * @return 协程化的 HTTP 响应
		 * 如果没有匹配的路由，返回 404 Not Found。
		 */
		[[nodiscard]] Awaitable<HttpResponse> dispatch(HttpRequest& req);

		/**
		 * @brief 同步快速路径分发（零协程帧开销）
		 * 当 handler 是同步注册且无路由级中间件时，直接调用返回结果。
		 * @param req HTTP 请求
		 * @return 有值 = 同步处理完成；nullopt = 需要 fallback 到 co_await dispatch()
		 */
		[[nodiscard]] std::optional<HttpResponse> dispatchSync(HttpRequest& req);

		/**
		 * @brief WebSocket 路由匹配结果
		 */
		struct WsRouteMatch
		{
			const WsRoute* route = nullptr;
			ParamList params; ///< 捕获的路径参数（仅参数路由时非空）
		};

		/**
		 * @brief 检查路径是否为 WebSocket 路由
		 * 支持精确匹配和参数路由 `{param}` 模式。
		 * @param path 请求路径
		 * @return WsRouteMatch（route 为 nullptr 表示无匹配）
		 */

		[[nodiscard]] WsRouteMatch findWsRoute(std::string_view path) const;

		/**
		 * @brief 检查指定方法+路径的路由是否存在（Expect: 100-continue 准入预检用）
		 * 只做存在性判断，不捕获路径参数，不触发 405 检测。
		 * @param method HTTP 方法
		 * @param path 已去除查询串的请求路径（未 URL decode）
		 * @return true 表示路由存在
		 */
		[[nodiscard]] bool exists(HttpMethod method, std::string_view path) const;

		/**
		 * @brief 获取已注册路由数量（HTTP + WebSocket）
		 * @return 路由数量
		 */
		[[nodiscard]] size_t routeCount() const;

		/**
		 * @brief 创建路由组（前缀分组）
		 * @param prefix 路由前缀（如 "/api/v1"）
		 * @return 路由组对象
		 */
		[[nodiscard]] RouteGroup group(const std::string& prefix);

		/**
		 * @brief URL 解码（百分号编码 -> 原始字符，'+' -> 空格）
		 * @param encoded 编码后的字符串
		 * @return 解码后的字符串
		 */
		[[nodiscard]] static std::string urlDecode(std::string_view encoded);

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

		struct RouteEntry
		{
			RouteHandler asyncHandler;
			SyncRouteHandler syncHandler; ///< 可选，同步注册时保留原始 handler
		};

		std::unordered_map<RouteKey, RouteEntry, RouteKeyHash, RouteKeyEqual> staticRoutes_;

		// ============ 参数路由（按 method 分组，减少线性扫描范围） ============

		struct ParamRouteEntry
		{
			HttpMethod method;
			std::string path;
			RouteHandler handler;
			SyncRouteHandler syncHandler; ///< 可选
		};

		std::unordered_map<HttpMethod, std::vector<ParamRouteEntry>> paramRoutesByMethod_;

		// ============ 路由查找结果（dispatch/dispatchSync 共用） ============

		struct ResolveResult
		{
			const RouteEntry* staticEntry = nullptr;     ///< 静态路由命中
			const ParamRouteEntry* paramEntry = nullptr; ///< 参数路由命中
			std::string allowedMethods;                  ///< 405 时的 Allow 头值
			bool pathTooDeep = false;                    ///< 路径深度超限
		};

		/**
		 * @brief 统一的路由查找（dispatch/dispatchSync 共用）
		 * 执行 URL 解码、路径深度校验、静态路由查找、参数路由匹配、405 检测。
		 * 匹配成功时将路径参数写入 req。
		 */
		ResolveResult resolveRoute(HttpRequest& req) const;

		// ============ 静态路由路径 → 方法集反向索引（405 检测 O(1)） ============

		std::unordered_map<std::string, std::vector<HttpMethod>, StringHash, StringEqual> staticPathMethods_;

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
	};

/**
 * @brief 路由注册宏（手动注册的便捷方式）
 * 用法：HICAL_ROUTE(router, Get, "/api/users", myHandler)
 * 等价于 router.route(HttpMethod::hGet, "/api/users", myHandler);
 * 如需自动路由注册，请参考 MetaRoutes.h 中的反射方案：
 *   hical::meta::registerRoutes<UserHandler>(router);
 */
#define HICAL_ROUTE(router, method, path, handler) (router).route(::hical::HttpMethod::h##method, path, handler)

} // namespace hical
