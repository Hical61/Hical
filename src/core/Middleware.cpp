#include "Middleware.h"

namespace hical
{

	void MiddlewarePipeline::use(MiddlewareHandler middleware)
	{
		middlewares_.push_back(std::move(middleware));
	}

	void MiddlewarePipeline::build(MiddlewareNext finalHandler)
	{
		if (middlewares_.empty())
		{
			cachedChain_ = std::move(finalHandler);
			return;
		}

		// 从最内层向外构建调用链，仅构建一次
		MiddlewareNext current = std::move(finalHandler);

		for (int i = static_cast<int>(middlewares_.size()) - 1; i >= 0; --i)
		{
			// 按 const 引用捕获中间件，避免拷贝 std::function 的堆分配开销。
			// 安全：build() 后 middlewares_ 不再修改（started_ 标志保护），
			// 且 MiddlewarePipeline 生命周期覆盖所有请求处理。
			const auto& mw = middlewares_[i];
			current = [&mw, next = std::move(current)](HttpRequest& r) -> Awaitable<HttpResponse>
			{
				co_return co_await mw(r, next);
			};
		}

		cachedChain_ = std::move(current);
	}

	Awaitable<HttpResponse> MiddlewarePipeline::execute(HttpRequest& req, MiddlewareNext finalHandler)
	{
		// 优先使用预构建缓存
		if (cachedChain_)
		{
			co_return co_await cachedChain_(req);
		}

		// 回退：未调用 build()，动态构建（兼容旧代码路径）
		if (middlewares_.empty())
		{
			co_return co_await finalHandler(req);
		}

		MiddlewareNext current = std::move(finalHandler);

		for (int i = static_cast<int>(middlewares_.size()) - 1; i >= 0; --i)
		{
			const auto& mw = middlewares_[i];
			current = [&mw, next = std::move(current)](HttpRequest& r) -> Awaitable<HttpResponse>
			{
				co_return co_await mw(r, next);
			};
		}

		co_return co_await current(req);
	}

	size_t MiddlewarePipeline::size() const
	{
		return middlewares_.size();
	}

} // namespace hical
