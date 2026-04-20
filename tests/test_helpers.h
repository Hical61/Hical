#pragma once

/**
 * @brief 测试辅助工具
 * 提供 runCoroutine 等共享辅助函数，避免各测试文件重复定义。
 */

#include "asio/AsioEventLoop.h"
#include "core/Coroutine.h"
#include <atomic>
#include <chrono>
#include <optional>
#include <thread>
#include <type_traits>

namespace hical::test
{

	/**
	 * @brief 在事件循环中运行协程并等待结果
	 * @tparam F 协程工厂函数类型
	 * @param loop 事件循环
	 * @param f 返回 Awaitable<T> 的可调用对象
	 * @return std::optional<T> 协程返回值，超时返回 nullopt
	 */
	template <typename F>
	auto runCoroutine(AsioEventLoop& loop, F&& f)
	{
		using ReturnType = typename std::invoke_result_t<F>::value_type;

		std::optional<ReturnType> result;
		std::atomic<bool> done {false};

		coSpawn(loop.getIoContext(),
				[&]() -> Awaitable<void>
				{
					result = co_await f();
					done = true;
				});

		std::thread loopThread(
			[&loop]()
			{
				loop.run();
			});

		for (int i = 0; i < 100 && !done.load(); ++i)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}

		loop.stop();
		loopThread.join();

		return result;
	}

} // namespace hical::test
