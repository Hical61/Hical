#include "Middleware.h"

namespace hical
{

	void MiddlewarePipeline::use(MiddlewareHandler middleware)
	{
		middlewares_.push_back(std::move(middleware));
	}

	Awaitable<HttpResponse> MiddlewarePipeline::execute(const HttpRequest& req, MiddlewareNext finalHandler)
	{
		if (middlewares_.empty())
		{
			co_return co_await finalHandler(req);
		}

		// 从最内层（最后注册的中间件）向外构建调用链
		// 最终处理器作为最内层
		MiddlewareNext current = std::move(finalHandler);

		for (int i = static_cast<int>(middlewares_.size()) - 1; i >= 0; --i)
		{
			// 值拷贝捕获，避免 vector 扩容后引用悬空
			auto mw = middlewares_[i];
			current = [mw, next = std::move(current)](const HttpRequest& r) -> Awaitable<HttpResponse>
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
