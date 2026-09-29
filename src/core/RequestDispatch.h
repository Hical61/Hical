/**
 * @file RequestDispatch.h
 * @brief 请求分发上下文与中间件链终端骨架（框架内部）
 */

#pragma once

#include "HttpRequest.h"
#include "HttpResponse.h"
#include "Coroutine.h"
#include <functional>

namespace hical::detail
{

	/**
	 * @brief 每请求分发上下文（栈上），由有中间件路径写入 HttpRequest 内部槽
	 * 洋葱链的结构（哪些中间件、顺序、Sync 合并）是进程级不变的，start() 里就预构建好；
	 * 只有「读 body + 按前置 resolveRoute 结果分发」这最后一步依赖 per-request 状态。
	 * 本结构把这个 per-request 的终端处理器塞进 HttpRequest 的内部槽，让预构建链终端
	 * 骨架（runInternalDispatch）执行时从槽里取——链只 build() 一次，每请求零重建。
	 * tailHandler 按引用捕获 handleSession 栈上的 resolveResult / readRequestBody 等，
	 * 生命周期由 handleSession 的 for 循环保证：链执行完 ctx 即失效，req 下一轮复用前
	 * 槽会被重新写入。禁止把 tailHandler 副本缓存到链里，否则上一请求的 resolveResult
	 * 会 stale 甚至悬空。
	 */
	struct RequestDispatchContext
	{
		std::function<Awaitable<HttpResponse>(HttpRequest&)> tailHandler;

		/**
		 * @brief 写入请求的内部槽（供 HttpSessionImpl 有中间件路径调用）
		 * @param req 请求
		 * @param ctx 栈上分发上下文
		 */
		static void attach(HttpRequest& req, RequestDispatchContext* ctx) noexcept
		{
			req.internalSlot_ = ctx;
		}

		/**
		 * @brief 读取请求内部槽指向的分发上下文（供终端骨架调用）
		 * @param req 请求
		 * @return 分发上下文指针，槽未写入时为 nullptr
		 */
		static RequestDispatchContext* get(HttpRequest& req) noexcept
		{
			return static_cast<RequestDispatchContext*>(req.internalSlot_);
		}
	};

	/**
	 * @brief 预构建中间件链的终端骨架：从请求槽取分发上下文并调用其 tailHandler
	 * 这个 lambda 被 HttpServer::start() 里 build() 作为 finalHandler 缓存进 cachedChain_，
	 * 每次请求执行到最内层时才被调用。它绝不缓存 tailHandler 副本，每次执行即时读槽，
	 * 保证拿到的是当前请求的分发上下文，不会把上一请求的 resolveResult 带进来。
	 * @param req 请求
	 * @return 分发结果
	 */
	inline Awaitable<HttpResponse> runInternalDispatch(HttpRequest& req)
	{
		auto* ctx = RequestDispatchContext::get(req);
		// 有中间件路径必然先 attach，槽不会是空；空则说明链路被误用，属编程错误。
		if (ctx == nullptr)
		{
			co_return HttpResponse::serverError();
		}
		co_return co_await ctx->tailHandler(req);
	}

} // namespace hical::detail
