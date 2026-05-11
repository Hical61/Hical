#include "RouteGroup.h"
#include <algorithm>

namespace hical
{

	RouteGroup::RouteGroup(Router& router, std::string prefix, std::vector<MiddlewareHandler> middlewares)
		: m_router(router), m_prefix(std::move(prefix))
	{
		m_entries.reserve(middlewares.size());
		for (auto& mw : middlewares)
		{
			MiddlewareEntry entry;
			entry.type = MiddlewareEntry::Type::Async;
			entry.asyncHandler = std::move(mw);
			m_entries.push_back(std::move(entry));
		}
	}

	RouteGroup::RouteGroup(Router& router, std::string prefix, std::vector<MiddlewareEntry> entries)
		: m_router(router), m_prefix(std::move(prefix)), m_entries(std::move(entries))
	{
	}

	void RouteGroup::use(MiddlewareHandler middleware)
	{
		MiddlewareEntry entry;
		entry.type = MiddlewareEntry::Type::Async;
		entry.asyncHandler = std::move(middleware);
		m_entries.push_back(std::move(entry));
	}

	void RouteGroup::use(SyncBeforeHandler before)
	{
		MiddlewareEntry entry;
		entry.type = MiddlewareEntry::Type::Sync;
		entry.before = std::move(before);
		m_entries.push_back(std::move(entry));
	}

	void RouteGroup::use(SyncBeforeHandler before, SyncAfterHandler after)
	{
		MiddlewareEntry entry;
		entry.type = MiddlewareEntry::Type::Sync;
		entry.before = std::move(before);
		entry.after = std::move(after);
		m_entries.push_back(std::move(entry));
	}

	RouteGroup RouteGroup::group(const std::string& subPrefix)
	{
		return RouteGroup(m_router, joinPath(subPrefix), m_entries);
	}

	std::string RouteGroup::joinPath(const std::string& path) const
	{
		if (m_prefix.empty())
		{
			return path;
		}
		if (path.empty())
		{
			return m_prefix;
		}

		bool prefixEndsSlash = m_prefix.back() == '/';
		bool pathStartsSlash = path.front() == '/';

		if (prefixEndsSlash && pathStartsSlash)
		{
			return m_prefix + path.substr(1);
		}
		if (!prefixEndsSlash && !pathStartsSlash)
		{
			return m_prefix + "/" + path;
		}
		return m_prefix + path;
	}

	RouteHandler RouteGroup::wrapHandler(RouteHandler handler) const
	{
		if (m_entries.empty())
		{
			return handler;
		}

		MiddlewareNext finalNext = [h = std::move(handler)](HttpRequest& req) -> Awaitable<HttpResponse>
		{
			// RouteHandler 接收 const HttpRequest&，req 在 HttpServer 中始终非 const
			co_return co_await h(req);
		};

		auto chain = MiddlewarePipeline::buildOptimizedChain(m_entries, std::move(finalNext));

		// SAFETY: const_cast 是安全的，因为 RouteHandler 被调用时，req 对象
		// 始终是 HttpServer::handleSession 中的非 const 局部变量。
		// 根本原因是 RouteHandler 的签名用 const HttpRequest& 而 MiddlewareNext 用 HttpRequest&。
		// TODO: 考虑统一两个函数类型的 const 语义以消除 const_cast
		return [chain = std::move(chain)](const HttpRequest& req) -> Awaitable<HttpResponse>
		{
			co_return co_await chain(const_cast<HttpRequest&>(req)); // NOLINT(cppcoreguidelines-pro-type-const-cast)
		};
	}

	void RouteGroup::route(HttpMethod method, const std::string& path, RouteHandler handler)
	{
		m_router.route(method, joinPath(path), wrapHandler(std::move(handler)));
	}

	void RouteGroup::route(HttpMethod method, const std::string& path, SyncRouteHandler handler)
	{
		// 检查是否所有中间件都是 Sync 类型
		bool allSync = std::all_of(m_entries.begin(),
								   m_entries.end(),
								   [](const MiddlewareEntry& e)
								   {
									   return e.type == MiddlewareEntry::Type::Sync;
								   });

		if (m_entries.empty())
		{
			// 无中间件：直接注册同步 handler
			m_router.route(method, joinPath(path), std::move(handler));
			return;
		}

		if (allSync)
		{
			// 纯同步快速路径：所有中间件 + handler 都同步执行，零协程帧
			auto syncEntries = m_entries; // 复制一份给 lambda 捕获
			SyncRouteHandler syncWrapped = [syncEntries = std::move(syncEntries),
											h = handler](const HttpRequest& req) -> HttpResponse
			{
				// 依次执行 SyncBeforeHandler
				auto& mutableReq = const_cast<HttpRequest&>(req); // NOLINT
				for (const auto& entry : syncEntries)
				{
					if (entry.before)
					{
						auto result = entry.before(mutableReq);
						if (result.has_value())
						{
							return std::move(*result); // 短路：中间件拦截
						}
					}
				}

				// 所有 before 通过，调用最终 handler
				auto res = h(req);

				// 逆序执行 SyncAfterHandler
				for (auto it = syncEntries.rbegin(); it != syncEntries.rend(); ++it)
				{
					if (it->after)
					{
						it->after(mutableReq, res);
					}
				}

				return res;
			};

			// 同时注册 async 和 sync 版本
			m_router.route(method, joinPath(path), std::move(syncWrapped));
		}
		else
		{
			// 有异步中间件或无中间件，走原来的异步包装路径
			auto asyncHandler = [h = std::move(handler)](const HttpRequest& req) -> Awaitable<HttpResponse>
			{
				co_return h(req);
			};
			route(method, path, std::move(asyncHandler));
		}
	}

	void RouteGroup::get(const std::string& path, RouteHandler handler)
	{
		route(HttpMethod::hGet, path, std::move(handler));
	}

	void RouteGroup::get(const std::string& path, SyncRouteHandler handler)
	{
		route(HttpMethod::hGet, path, std::move(handler));
	}

	void RouteGroup::post(const std::string& path, RouteHandler handler)
	{
		route(HttpMethod::hPost, path, std::move(handler));
	}

	void RouteGroup::post(const std::string& path, SyncRouteHandler handler)
	{
		route(HttpMethod::hPost, path, std::move(handler));
	}

	void RouteGroup::put(const std::string& path, RouteHandler handler)
	{
		route(HttpMethod::hPut, path, std::move(handler));
	}

	void RouteGroup::put(const std::string& path, SyncRouteHandler handler)
	{
		route(HttpMethod::hPut, path, std::move(handler));
	}

	void RouteGroup::del(const std::string& path, RouteHandler handler)
	{
		route(HttpMethod::hDelete, path, std::move(handler));
	}

	void RouteGroup::del(const std::string& path, SyncRouteHandler handler)
	{
		route(HttpMethod::hDelete, path, std::move(handler));
	}

} // namespace hical
