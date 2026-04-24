#pragma once

#include "HttpRequest.h"
#include "HttpResponse.h"
#include "Coroutine.h"
#include <functional>
#include <vector>

namespace hical
{

	/**
	 * @brief 中间件 next 回调类型
	 * 调用 next 会继续执行下一个中间件或最终的路由处理器。
	 */
	using MiddlewareNext = std::function<Awaitable<HttpResponse>(HttpRequest&)>;

	/**
	 * @brief 中间件处理器类型
	 * 接收请求和 next 回调，返回协程化的响应。
	 * 中间件可以在调用 next 前后执行逻辑（洋葱模型）。
	 * 示例：
	 * ```cpp
	 * auto logger = [](HttpRequest& req, MiddlewareNext next)
	 *     -> Awaitable<HttpResponse> {
	 *     std::cout << req.path() << std::endl;    // 前置逻辑
	 *     auto res = co_await next(req);            // 调用下一层
	 *     std::cout << res.statusCode() << std::endl; // 后置逻辑
	 *     co_return res;
	 * };
	 * ```
	 */
	using MiddlewareHandler = std::function<Awaitable<HttpResponse>(HttpRequest&, MiddlewareNext)>;

	/**
	 * @brief 中间件管道（洋葱模型）
	 * 按注册顺序依次执行中间件，最后执行最终处理器。
	 * 每个中间件可以决定是否调用 next 继续执行，或直接返回响应（拦截）。
	 */
	class MiddlewarePipeline
	{
	public:
		MiddlewarePipeline() = default;

		/**
		 * @brief 添加中间件
		 * @param middleware 中间件处理器
		 * @throw std::logic_error 当 build() 已调用后再 use() 时抛出
		 * @note 必须在 build() 之前调用
		 */
		void use(MiddlewareHandler middleware);

		/**
		 * @brief 预构建中间件调用链
		 * @param finalHandler 最终处理器（通常是路由分发）
		 * @throw std::logic_error 当重复调用 build() 时抛出
		 * 调用后，execute() 直接使用缓存的调用链，避免每次请求重建。
		 * 调用后不应再 use() 添加中间件。
		 */
		void build(MiddlewareNext finalHandler);

		/**
		 * @brief 执行中间件管道（预构建路径，无需传入 finalHandler）
		 * @param req HTTP 请求
		 * @return 协程化的 HTTP 响应
		 * @throw std::logic_error 当未调用 build() 时抛出
		 */
		Awaitable<HttpResponse> execute(HttpRequest& req);

		/**
		 * @brief 执行中间件管道（始终按传入的 finalHandler 动态构建调用链）
		 * @param req HTTP 请求
		 * @param finalHandler 最终处理器
		 * @return 协程化的 HTTP 响应
		 */
		Awaitable<HttpResponse> execute(HttpRequest& req, MiddlewareNext finalHandler);

		/**
		 * @brief 预构建自定义终端处理器的调用链
		 * @param finalHandler 自定义终端处理器
		 * @return 预构建好的调用链（可缓存后多次调用）
		 */
		MiddlewareNext buildFor(MiddlewareNext finalHandler) const;

		/**
		 * @brief 获取中间件数量
		 * @return 数量
		 */
		size_t size() const;

	private:
		/**
		 * @brief 从中间件列表构建洋葱调用链
		 * @param finalHandler 最终处理器
		 * @return 构建好的调用链
		 */
		MiddlewareNext buildChain(MiddlewareNext finalHandler) const;

		std::vector<MiddlewareHandler> middlewares_;
		MiddlewareNext cachedChain_; // 预构建的调用链缓存
	};

} // namespace hical
