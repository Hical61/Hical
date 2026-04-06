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
 *
 * 调用 next 会继续执行下一个中间件或最终的路由处理器。
 */
using MiddlewareNext =
    std::function<Awaitable<HttpResponse>(const HttpRequest&)>;

/**
 * @brief 中间件处理器类型
 *
 * 接收请求和 next 回调，返回协程化的响应。
 * 中间件可以在调用 next 前后执行逻辑（洋葱模型）。
 *
 * 示例：
 * ```cpp
 * auto logger = [](const HttpRequest& req, MiddlewareNext next)
 *     -> Awaitable<HttpResponse> {
 *     std::cout << req.path() << std::endl;    // 前置逻辑
 *     auto res = co_await next(req);            // 调用下一层
 *     std::cout << res.statusCode() << std::endl; // 后置逻辑
 *     co_return res;
 * };
 * ```
 */
using MiddlewareHandler =
    std::function<Awaitable<HttpResponse>(const HttpRequest&, MiddlewareNext)>;

/**
 * @brief 中间件管道（洋葱模型）
 *
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
     */
    void use(MiddlewareHandler middleware);

    /**
     * @brief 执行中间件管道
     * @param req HTTP 请求
     * @param finalHandler 最终处理器（通常是路由分发）
     * @return 协程化的 HTTP 响应
     */
    Awaitable<HttpResponse> execute(const HttpRequest& req,
                                    MiddlewareNext finalHandler);

    /**
     * @brief 获取中间件数量
     * @return 数量
     */
    size_t size() const;

  private:
    std::vector<MiddlewareHandler> middlewares_;
};

}  // namespace hical
