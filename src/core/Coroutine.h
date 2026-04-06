#pragma once

#include <boost/asio.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <chrono>
#include <functional>

namespace hical
{

class AsioEventLoop;

/**
 * @brief 协程返回类型别名
 * @tparam T 返回值类型（默认 void）
 */
template <typename T = void>
using Awaitable = boost::asio::awaitable<T>;

/**
 * @brief 协程化定时器等待（秒）
 * @param ioCtx io_context 引用
 * @param seconds 等待秒数
 *
 * 用法：co_await hical::sleepFor(ioCtx, 1.0);
 */
inline Awaitable<void> sleepFor(boost::asio::io_context& ioCtx, double seconds)
{
    boost::asio::steady_timer timer(
        ioCtx,
        std::chrono::milliseconds(static_cast<int64_t>(seconds * 1000)));
    co_await timer.async_wait(boost::asio::use_awaitable);
}

/**
 * @brief 协程化定时器等待（chrono duration）
 * @param ioCtx io_context 引用
 * @param duration 等待时长
 */
inline Awaitable<void> sleepFor(
    boost::asio::io_context& ioCtx,
    std::chrono::steady_clock::duration duration)
{
    boost::asio::steady_timer timer(ioCtx, duration);
    co_await timer.async_wait(boost::asio::use_awaitable);
}

/**
 * @brief 在当前协程上下文中等待指定时长（使用 co_await 的 executor）
 * @param seconds 等待秒数
 *
 * 用法：co_await hical::sleep(1.0);
 * 注意：必须在协程内调用
 */
inline Awaitable<void> sleep(double seconds)
{
    auto executor = co_await boost::asio::this_coro::executor;
    boost::asio::steady_timer timer(
        executor,
        std::chrono::milliseconds(static_cast<int64_t>(seconds * 1000)));
    co_await timer.async_wait(boost::asio::use_awaitable);
}

/**
 * @brief 在当前协程上下文中等待指定时长
 * @param duration 等待时长
 */
inline Awaitable<void> sleep(std::chrono::steady_clock::duration duration)
{
    auto executor = co_await boost::asio::this_coro::executor;
    boost::asio::steady_timer timer(executor, duration);
    co_await timer.async_wait(boost::asio::use_awaitable);
}

/**
 * @brief co_spawn 便捷封装
 * @param ioCtx io_context 引用
 * @param coroutine 协程函数
 *
 * 在指定 io_context 上启动一个协程。
 */
template <typename F>
void coSpawn(boost::asio::io_context& ioCtx, F&& coroutine)
{
    boost::asio::co_spawn(ioCtx, std::forward<F>(coroutine),
                          boost::asio::detached);
}

/**
 * @brief co_spawn 带完成回调的封装
 * @param ioCtx io_context 引用
 * @param coroutine 协程函数
 * @param handler 完成回调（接收 std::exception_ptr）
 */
template <typename F, typename Handler>
void coSpawn(boost::asio::io_context& ioCtx, F&& coroutine, Handler&& handler)
{
    boost::asio::co_spawn(ioCtx, std::forward<F>(coroutine),
                          std::forward<Handler>(handler));
}

}  // namespace hical
