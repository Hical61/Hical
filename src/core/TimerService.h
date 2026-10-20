/**
 * @file TimerService.h
 * @brief 每个 io_context 一个的集中式定时器
 */

#pragma once

#include <boost/asio/any_io_executor.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

namespace hical
{

	class TimerService;

	namespace detail
	{
		struct TimerServiceCore;
	} // namespace detail

	/**
	 * @brief 拿当前线程绑的 TimerService，没绑就是 nullptr
	 */
	TimerService* currentThreadTimerService();

	/**
	 * @brief 最小堆 + 一个 steady_timer 的定时器服务
	 * 原来每次 sleep 都现造一个 steady_timer，3 万个协程同时挂 10ms 就是 3 万个
	 * timerfd/epoll_ctl，再加内核那边红黑树的维护。这里整条 io_context 只留一个
	 * steady_timer，永远只把堆顶的 deadline 挂上去，堆顶变了才重新 expires_at。
	 * 所以 10 万个并发定时器也只占一个 timerfd。
	 * 线程模型：跟它所属的 io_context 一样跑在单线程上，全程无锁。
	 * schedule/cancel 和回调执行都必须在同一条线程（也就是跑那个 io_context 的线程）。
	 * 生命周期：回调里引用的外部对象由调用方自己保证，服务只存 std::function。
	 * shutdown()（析构里会自动调）会把没触发的回调连堆一起扔掉，一个都不会执行；
	 * 之后 schedule 一律返回 kInvalidId。服务必须先于 io_context 销毁，跟 IdleScanner 一个规矩。
	 * thread_local：构造不自动绑。HttpServer 是在主线程上给每个 worker loop 各建一个的，
	 * 构造就绑会让主线程绑到最后一个 loop 的服务上，主线程里 sleep 就会被 resume 到错的
	 * 线程去。所以绑定走显式的 bindToCurrentThread()，在目标 io_context 线程上调。
	 */
	class TimerService
	{
	public:
		using Clock = std::chrono::steady_clock;
		using TimerId = std::uint64_t;
		using Callback = std::function<void()>;

		/// 无效 id。0 永远不会派给定时器，服务停了之后 schedule 也返回它
		inline static constexpr TimerId kInvalidId = 0;

		explicit TimerService(boost::asio::any_io_executor executor);
		~TimerService();

		TimerService(const TimerService&) = delete;
		TimerService& operator=(const TimerService&) = delete;
		TimerService(TimerService&&) = delete;
		TimerService& operator=(TimerService&&) = delete;

		/**
		 * @brief 挂一个定时器
		 * @param deadline 绝对时间点，已经过去的话下一轮就触发
		 * @param callback 到点执行，跑在服务自己的线程上
		 * @return 定时器 id，服务已停止时返回 kInvalidId
		 */
		TimerId schedule(Clock::time_point deadline, Callback callback);

		/**
		 * @brief 相对现在挂一个定时器，sleep 这类用法更顺手
		 * @param delay 延迟，负数当 0 处理
		 * @return 定时器 id，服务已停止时返回 kInvalidId
		 */
		TimerId scheduleAfter(Clock::duration delay, Callback callback);

		/**
		 * @brief 取消定时器，幂等
		 * 只是打个标记，节点赖在堆里等它到期再清，回调保证不会再执行。
		 * 不用从堆中间真删，因为堆顶一定是到期最早的那个：取消堆顶也不会让别的项
		 * 早于它该到的时间，所以不用重挂 timer。
		 * @return false 表示这个 id 之前就取消过了。取消一个已经触发过的 id 也会返回
		 *         true，但除了留个标记什么都不做（标记攒多了内部会自己清，不用管）
		 */
		bool cancel(TimerId id);

		/**
		 * @brief 停服务：扔掉所有没触发的回调，cancel 底层 timer
		 * 已触发的回调不受影响（它们在别的栈上）。可以重复调。
		 * @warning 扔掉的回调不会被执行，也就没人去唤醒挂在上面的等待方——拿
		 *          `hical::sleep()` 来说，协程会一直挂在那儿不返回、帧也不释放。
		 *          调用方得自己保证关服务时没有在途的等待，或者先让它们自然到点。
		 *          `HttpServer::stop()` 就是绕开这条不调它：睡着的协程靠定时器自然
		 *          醒来才能把 run() 收尾，提前关掉整条循环就卡住了。
		 */
		void shutdown();

		/**
		 * @brief 还没触发、也没被取消的定时器数量
		 * 每次调用现数一遍堆，O(堆大小)，堆里一个取消标记都没有时直接返回堆大小。
		 * 只有测试和诊断会调，没在 schedule / cancel 那条路上维护计数器。
		 */
		[[nodiscard]] std::size_t pendingCount() const;

		/**
		 * @brief 把自己绑到当前线程的 thread_local，供 currentThreadTimerService() 取
		 * 必须在服务自己那条 io_context 线程上调。跨线程建的时候（比如 HttpServer 在
		 * 主线程给每个 worker loop 建一个）构造完要先 post 到目标线程再调这个，
		 * 不然主线程上的 sleep 会拿到别的 loop 的服务，协程就被 resume 到错的线程去了。
		 */
		void bindToCurrentThread();

		/**
		 * @brief 从当前线程解绑，只解自己绑的那个
		 */
		void unbindFromCurrentThread();

	private:
		std::shared_ptr<detail::TimerServiceCore> core_;
	};

} // namespace hical
