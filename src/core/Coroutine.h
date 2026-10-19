/**
 * @file Coroutine.h
 * @brief 协程工具（Awaitable 别名 + coSpawn）
 */

#pragma once

#include "TimerService.h"
#include <boost/asio.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/bind_allocator.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/deferred.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/recycling_allocator.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <chrono>
#include <cstdio>
#include <functional>
#include <memory>
#include <type_traits>

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
	 * 用法：co_await hical::sleepFor(ioCtx, 1.0);
	 */
	[[nodiscard]] inline Awaitable<void> sleepFor(boost::asio::io_context& ioCtx, double seconds)
	{
		boost::asio::steady_timer timer(ioCtx, std::chrono::milliseconds(static_cast<int64_t>(seconds * 1000)));
		co_await timer.async_wait(boost::asio::use_awaitable);
	}

	/**
	 * @brief 协程化定时器等待（chrono duration）
	 * @param ioCtx io_context 引用
	 * @param duration 等待时长
	 */
	[[nodiscard]] inline Awaitable<void> sleepFor(boost::asio::io_context& ioCtx,
												  std::chrono::steady_clock::duration duration)
	{
		boost::asio::steady_timer timer(ioCtx, duration);
		co_await timer.async_wait(boost::asio::use_awaitable);
	}

	namespace detail
	{
		/**
		 * @brief 「睡一会儿」这个异步操作
		 * 服务端当前线程有 TimerService 就挂过去，整条 io_context 只占一个 timerfd；
		 * 拿不到就退回自己的 steady_timer。回退这条路不能砍：客户端、自己 new 的
		 * io_context、单测里跑协程都没有 TimerService，砍了这些地方直接炸。
		 */
		struct SleepInitiation
		{
			template <typename Handler>
			void operator()(Handler&& handler,
							boost::asio::any_io_executor executor,
							std::chrono::steady_clock::duration delay) const
			{
				// 服务的回调类型是 std::function，装的必须可拷贝，而 asio 的 awaitable
				// 完成句柄是 move-only，所以裹一层 shared_ptr 递进去
				auto wakeUp = std::make_shared<Handler>(std::move(handler));

				if (auto* service = currentThreadTimerService())
				{
					if (service->scheduleAfter(delay,
											   [wakeUp]()
											   {
												   (*wakeUp)();
											   })
						!= TimerService::kInvalidId)
					{
						return;
					}
				}

				// 没绑服务，或者服务刚 shutdown 过（schedule 返回 kInvalidId）：用自己的
				// timer 兜底。后一种不能干脆不挂起就返回，不然 while 里的 sleep 会变成空旋。
				auto timer = std::make_shared<boost::asio::steady_timer>(std::move(executor), delay);
				timer->async_wait(
					[timer, wakeUp](const boost::system::error_code&)
					{
						// aborted 也照样叫醒：等的人只关心「时间到没到」
						(*wakeUp)();
					});
			}
		};

		/**
		 * @brief 睡一会儿的异步操作对象，直接 co_await 用，完成时不需要额外的协程帧
		 */
		template <typename CompletionToken>
		auto asyncSleep(boost::asio::any_io_executor executor,
						std::chrono::steady_clock::duration delay,
						CompletionToken&& token)
		{
			return boost::asio::async_initiate<CompletionToken, void()>(SleepInitiation {},
																		token,
																		std::move(executor),
																		delay);
		}
	} // namespace detail

	/**
	 * @brief 在当前协程上下文中等待指定时长（使用 co_await 的 executor）
	 * @param seconds 等待秒数
	 * 用法：co_await hical::sleep(1.0);
	 * 注意：必须在协程内调用
	 * 当前线程绑了 TimerService 就挂过去，整条 io_context 共用一个 timerfd；
	 * 没绑就用自己的 steady_timer，跟以前一样。
	 */
	[[nodiscard]] inline Awaitable<void> sleep(double seconds)
	{
		auto executor = co_await boost::asio::this_coro::executor;
		co_await detail::asyncSleep(std::move(executor),
									std::chrono::milliseconds(static_cast<int64_t>(seconds * 1000)),
									boost::asio::deferred);
	}

	/**
	 * @brief 在当前协程上下文中等待指定时长
	 * @param duration 等待时长
	 */
	[[nodiscard]] inline Awaitable<void> sleep(std::chrono::steady_clock::duration duration)
	{
		auto executor = co_await boost::asio::this_coro::executor;
		co_await detail::asyncSleep(std::move(executor), duration, boost::asio::deferred);
	}

	/**
	 * @brief 默认协程完成回调：异常时输出日志到 stderr，而非静默丢弃
	 * 相比 boost::asio::detached，此 handler 在协程抛出未捕获异常时
	 * 输出错误信息，避免连接泄漏等问题在生产环境中无从追踪。
	 */
	inline auto logOnException = [](std::exception_ptr eptr)
	{
		if (eptr)
		{
			try
			{
				std::rethrow_exception(eptr);
			}
			catch (const std::exception& e)
			{
				std::fprintf(stderr, "[hical] unhandled coroutine exception: %s\n", e.what());
			}
			catch (...)
			{
				std::fprintf(stderr, "[hical] unhandled coroutine exception (unknown type)\n");
			}
		}
	};

	/**
	 * @brief co_spawn 便捷封装（默认带异常日志 + recycling_allocator 复用 completion handler 内存）
	 * @param ioCtx io_context 引用
	 * @param coroutine 协程函数
	 * 在指定 io_context 上启动一个协程。
	 * 协程内未捕获的异常会输出到 stderr，而非静默丢弃。
	 * recycling_allocator 用 thread_local 缓存兜着，高并发不用反复 malloc/free。
	 */
	template <typename F>
	void coSpawn(boost::asio::io_context& ioCtx, F&& coroutine)
	{
		boost::asio::co_spawn(ioCtx,
							  std::forward<F>(coroutine),
							  boost::asio::bind_allocator(boost::asio::recycling_allocator<void>(), logOnException));
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
		boost::asio::co_spawn(ioCtx, std::forward<F>(coroutine), std::forward<Handler>(handler));
	}

	/**
	 * @brief co_spawn 便捷封装（接受任意 executor，默认带异常日志 + recycling_allocator）
	 * @param executor 执行器（如 socket.get_executor()、co_await this_coro::executor）
	 * @param coroutine 协程函数
	 * 在指定 executor 上启动一个协程。
	 * 协程内未捕获的异常会输出到 stderr，而非静默丢弃。
	 */
	template <typename Executor, typename F>
		requires(!std::is_same_v<std::decay_t<Executor>, boost::asio::io_context>)
	void coSpawn(Executor&& executor, F&& coroutine)
	{
		boost::asio::co_spawn(std::forward<Executor>(executor),
							  std::forward<F>(coroutine),
							  boost::asio::bind_allocator(boost::asio::recycling_allocator<void>(), logOnException));
	}

	/**
	 * @brief co_spawn 带完成回调的封装（接受任意 executor）
	 * @param executor 执行器
	 * @param coroutine 协程函数
	 * @param handler 完成回调（接收 std::exception_ptr）
	 */
	template <typename Executor, typename F, typename Handler>
		requires(!std::is_same_v<std::decay_t<Executor>, boost::asio::io_context>)
	void coSpawn(Executor&& executor, F&& coroutine, Handler&& handler)
	{
		boost::asio::co_spawn(std::forward<Executor>(executor),
							  std::forward<F>(coroutine),
							  std::forward<Handler>(handler));
	}

} // namespace hical
