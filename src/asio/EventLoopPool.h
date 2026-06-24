/**
 * @file EventLoopPool.h
 * @brief 多线程事件循环池（1线程:1 io_context）
 */

#pragma once

#include "AsioEventLoop.h"
#include <atomic>
#include <memory>
#include <thread>
#include <vector>

namespace hical
{

	/**
	 * @brief 多线程事件循环池
	 * 管理多个 AsioEventLoop 实例，每个运行在独立线程中。
	 * 采用 1 Thread : 1 io_context 模型。
	 * 通过 round-robin 策略将新连接分发到不同的事件循环。
	 */
	class EventLoopPool
	{
	public:
		/**
		 * @brief 构造事件循环池
		 * @param numThreads 线程数（即事件循环数量）
		 * @param concurrencyHint Asio io_context 并发提示，透传给每个 AsioEventLoop
		 */
		explicit EventLoopPool(size_t numThreads, int concurrencyHint = 1);

		~EventLoopPool();

		/**
		 * @brief 启动所有事件循环线程
		 */
		void start();

		/**
		 * @brief 停止所有事件循环并等待线程结束
		 */
		void stop();

		/**
		 * @brief 放掉所有 worker 的 work_guard，让它们干完手头活自己退出
		 */
		void releaseWork();

		/**
		 * @brief 获取下一个事件循环（round-robin）
		 * @return 事件循环指针
		 */
		[[nodiscard]] AsioEventLoop* getNextLoop();

		/**
		 * @brief 获取所有事件循环
		 * @return 事件循环指针列表
		 */
		[[nodiscard]] std::vector<AsioEventLoop*> getAllLoops();

		/**
		 * @brief 获取池中事件循环数量
		 * @return 数量
		 */
		[[nodiscard]] size_t size() const;

		/**
		 * @brief 池是否已启动
		 * @return true 如果已启动
		 */
		[[nodiscard]] bool isRunning() const;

		// 禁止拷贝和移动
		EventLoopPool(const EventLoopPool&) = delete;
		EventLoopPool& operator=(const EventLoopPool&) = delete;

	private:
		std::vector<std::unique_ptr<AsioEventLoop>> loops_;
		std::vector<std::thread> threads_;
		std::atomic<size_t> nextIndex_ {0};
		std::atomic<bool> running_ {false};
	};

} // namespace hical
