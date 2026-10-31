/**
 * @file RequestDispatch.h
 * @brief 请求分发上下文与中间件链终端骨架（框架内部）
 */

#pragma once

#include "HttpRequest.h"
#include "HttpResponse.h"
#include "Coroutine.h"
#include <optional>

namespace hical::detail
{

	/**
	 * @brief 每个请求的分发上下文，保存在会话栈或 HTTP/2 应用协程帧内
	 * 中间件链在 start() 时构建一次，读 body 和路由分发所需的请求状态通过内部槽传入。
	 * obj/invoke 保存终端闭包地址和调用入口，避免用 std::function 复制闭包并额外分配内存。
	 * 闭包必须活到应用链执行结束，不能缓存到中间件链中跨请求复用。
	 * bodyObj/readBody 用同样的方式调用 body 门，仅 needsBody 管线会设置这两个字段。
	 */
	struct RequestDispatchContext
	{
		/**
		 * @brief 终端处理器的类型擦除调用约定：从 obj 取回闭包并转发调用
		 * @param obj 会话栈或应用协程帧内的终端闭包地址
		 * @param req 请求
		 * @return 分发结果
		 */
		using InvokeFn = Awaitable<HttpResponse> (*)(void* obj, HttpRequest& req);

		/**
		 * @brief 读 body 的类型擦除调用约定：从 bodyObj 取回闭包并转发调用
		 * 返回 nullopt 表示读成功、body 已落进 req；返回 HttpResponse 表示读失败，
		 * 那个响应就是错误响应，调用方（body 门）要短路返回它、不再往下走。
		 * 跟 obj + invoke 一个套路，同样刻意不用 std::function——每请求一次堆分配省掉。
		 * @param obj 会话栈或应用协程帧内的读 body 闭包地址
		 * @return nullopt = 读成功；有值 = 读失败（错误响应）
		 */
		using BodyReadFn = Awaitable<std::optional<HttpResponse>> (*)(void* obj);

		void* obj = nullptr;           ///< 终端闭包地址，应用链执行完即失效
		InvokeFn invoke = nullptr;     ///< 与 obj 配套的调用入口，由 tailInvoke<闭包类型> 生成
		void* bodyObj = nullptr;       ///< 读 body 闭包地址，应用链执行完即失效
		BodyReadFn readBody = nullptr; ///< 与 bodyObj 配套的读 body 入口，由 bodyInvoke<闭包类型> 生成

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
	 * @brief 类型擦除调用入口：把 void* 还原成真实闭包类型再转发调用
	 * 直接 return 闭包产生的 awaitable，不套协程——本函数自己不是协程，不会多一个帧。
	 * @tparam Closure 终端闭包类型（由 handleSession 里 `ctx.invoke = &tailInvoke<decltype(tail)>` 推导）
	 */
	template <typename Closure>
	inline Awaitable<HttpResponse> tailInvoke(void* obj, HttpRequest& req)
	{
		return (*static_cast<Closure*>(obj))(req);
	}

	/**
	 * @brief 读 body 的类型擦除入口：把 void* 还原成真实闭包再转发调用
	 * 同 tailInvoke，本函数自己不是协程，直接 return 闭包产出的 awaitable，不多一个帧。
	 * @tparam Closure 读 body 闭包类型（由 handleSession 里 `ctx.readBody = &bodyInvoke<decltype(...)>` 推导）
	 */
	template <typename Closure>
	inline Awaitable<std::optional<HttpResponse>> bodyInvoke(void* obj)
	{
		return (*static_cast<Closure*>(obj))();
	}

	/**
	 * @brief 请求槽未写入时的冷路径兜底
	 * 有中间件路径必然先 attach，走到这里说明链路被误用，属编程错误；单独拆成一个协程
	 * 是为了不把冷路径的帧算进热路径的 runInternalDispatch。
	 */
	inline Awaitable<HttpResponse> dispatchContextMissing()
	{
		co_return HttpResponse::serverError();
	}

	/**
	 * @brief 预构建中间件链的终端骨架：从请求槽取分发上下文并调用其终端处理器
	 * 这个 lambda 被 HttpServer::start() 里 build() 作为 finalHandler 缓存进 cachedChain_，
	 * 每次请求执行到最内层时才被调用。它绝不缓存终端处理器副本，每次执行即时读槽，
	 * 保证拿到的是当前请求的分发上下文，不会把上一请求的 resolveResult 带进来。
	 * 本函数不再是协程：直接返回终端处理器产出的 awaitable，省掉一个每请求的转发帧。
	 * @param req 请求
	 * @return 分发结果
	 */
	inline Awaitable<HttpResponse> runInternalDispatch(HttpRequest& req)
	{
		auto* ctx = RequestDispatchContext::get(req);
		if (ctx == nullptr) [[unlikely]]
		{
			return dispatchContextMissing();
		}
		return ctx->invoke(ctx->obj, req);
	}

} // namespace hical::detail
