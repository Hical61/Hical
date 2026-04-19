#include "Middleware.h"
#include <stdexcept>

namespace hical
{

	void MiddlewarePipeline::use(MiddlewareHandler middleware)
	{
		if (cachedChain_)
		{
			throw std::logic_error("MiddlewarePipeline::use: cannot add middleware after build()");
		}
		middlewares_.push_back(std::move(middleware));
	}

	void MiddlewarePipeline::build(MiddlewareNext finalHandler)
	{
		if (cachedChain_)
		{
			throw std::logic_error("MiddlewarePipeline::build: already built");
		}

		cachedChain_ = buildChain(std::move(finalHandler));
	}

	MiddlewareNext MiddlewarePipeline::buildChain(MiddlewareNext finalHandler) const
	{
		if (middlewares_.empty())
		{
			return finalHandler;
		}

		// 从最内层向外构建调用链
		MiddlewareNext current = std::move(finalHandler);

		for (int i = static_cast<int>(middlewares_.size()) - 1; i >= 0; --i)
		{
			// 按值捕获中间件，确保协程帧持有独立副本，
			// 避免 middlewares_ 扩容或生命周期问题导致悬空引用。
			auto mw = middlewares_[i];
			current = [mw = std::move(mw), next = std::move(current)](HttpRequest& r) -> Awaitable<HttpResponse>
			{
				co_return co_await mw(r, next);
			};
		}

		return current;
	}

	Awaitable<HttpResponse> MiddlewarePipeline::execute(HttpRequest& req)
	{
		if (!cachedChain_)
		{
			throw std::logic_error("MiddlewarePipeline::execute: must call build() first");
		}
		co_return co_await cachedChain_(req);
	}

	Awaitable<HttpResponse> MiddlewarePipeline::execute(HttpRequest& req, MiddlewareNext finalHandler)
	{
		// 始终按传入的 finalHandler 构建调用链，不使用缓存，
		// 确保调用者的 finalHandler 被实际执行而非静默丢弃。
		auto chain = buildChain(std::move(finalHandler));
		co_return co_await chain(req);
	}

	size_t MiddlewarePipeline::size() const
	{
		return middlewares_.size();
	}

} // namespace hical
