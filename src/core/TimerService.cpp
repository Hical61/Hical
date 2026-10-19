/**
 * @file TimerService.cpp
 * @brief 集中式定时器实现
 */

#include "core/TimerService.h"

#include <boost/asio/steady_timer.hpp>
#include <algorithm>
#include <optional>
#include <unordered_set>
#include <utility>
#include <vector>

namespace hical
{

	namespace
	{
		// id 从 1 开始，0 留给 kInvalidId
		constexpr TimerService::TimerId kFirstTimerId = 1;

		// cancelled 表比堆还大这么多就重建一次，见 compactCancelled
		constexpr std::size_t kCancelledSlack = 256;

		// 每个 io_context 线程绑一个服务。裸指针，线程退出不用清（跟 IdleScanner 一个套路）
		thread_local TimerService* tls_timerService = nullptr;
	} // namespace

	namespace detail
	{
		/**
		 * @brief 服务的全部状态
		 * 单独拎出来用 shared_ptr 持有，是为了让 timer 的完成处理函数能拿 weak_ptr
		 * 而不是裸 this：服务先被销毁、handler 还排在 io_context 队列里时，lock 失败
		 * 直接返回，不会踩到已经析构的对象。
		 */
		struct TimerServiceCore : std::enable_shared_from_this<TimerServiceCore>
		{
			struct Node
			{
				TimerService::Clock::time_point deadline;
				TimerService::TimerId id;
				TimerService::Callback callback;
			};

			// 小顶堆：deadline 早的先出，deadline 相同按 id 走，保证先挂的先执行
			struct Later
			{
				bool operator()(const Node& lhs, const Node& rhs) const noexcept
				{
					if (lhs.deadline != rhs.deadline)
					{
						return lhs.deadline > rhs.deadline;
					}
					return lhs.id > rhs.id;
				}
			};

			explicit TimerServiceCore(boost::asio::any_io_executor executor) : timer(std::move(executor))
			{
			}

			// std::priority_queue 的 top() 只给 const 引用，节点里带 std::function 挪不出来，
			// 所以直接用 vector 配 push_heap/pop_heap 自己管
			void push(Node node)
			{
				heap.push_back(std::move(node));
				std::push_heap(heap.begin(), heap.end(), Later {});
			}

			Node popTop()
			{
				std::pop_heap(heap.begin(), heap.end(), Later {});
				Node top = std::move(heap.back());
				heap.pop_back();
				return top;
			}

			boost::asio::steady_timer timer;
			std::vector<Node> heap;
			// 只记取消掉的 id，正常项不进表；节点到期被摘出来时顺手 erase
			std::unordered_set<TimerService::TimerId> cancelled;
			// 已经挂到 timer 上的 deadline，空表示 timer 闲着没在等
			std::optional<TimerService::Clock::time_point> armedDeadline;
			TimerService::TimerId nextId = kFirstTimerId;
			// fireDue 正在跑那批摘出来的节点。这期间不能重建取消表，见 compactCancelled
			bool inFireDue = false;
			bool stopped = false;
		};
	} // namespace detail

	namespace
	{
		void rearm(detail::TimerServiceCore& core);

		/**
		 * @brief fireDue 期间把 inFireDue 挂上，告诉 compactCancelled 先别重建
		 * 那批到期的节点已经从堆里摘出来了，这时的重建只认堆里的活标记，会顺手把「取消
		 * 同批节点」留下的标记当幽灵删掉，那个回调就照样跑了。中途 break、回调抛异常都
		 * 得复位，所以用 RAII 而不是手工置位。
		 */
		struct FireDueGuard
		{
			explicit FireDueGuard(bool& flag) : flag_(flag)
			{
				flag_ = true;
			}

			~FireDueGuard()
			{
				flag_ = false;
			}

			FireDueGuard(const FireDueGuard&) = delete;
			FireDueGuard& operator=(const FireDueGuard&) = delete;

			bool& flag_;
		};

		/**
		 * @brief 到点了：一次性把过期的全摘出来执行，再按新堆顶重挂
		 */
		void fireDue(detail::TimerServiceCore& core)
		{
			core.armedDeadline.reset();
			if (core.stopped)
			{
				return;
			}

			// 下面这批摘出来之后就不在堆里了，这期间取消表不许重建
			FireDueGuard firing(core.inFireDue);

			const auto now = TimerService::Clock::now();

			// 先全摘出来再执行。回调里可能又 schedule/cancel，边遍历边改堆会乱
			std::vector<detail::TimerServiceCore::Node> due;
			while (!core.heap.empty() && core.heap.front().deadline <= now)
			{
				due.push_back(core.popTop());
			}

			for (auto& node : due)
			{
				// 前面的回调可能把服务关了，剩下的回调直接扔
				if (core.stopped)
				{
					break;
				}

				// 取消过的到这儿才真正清掉标记，回调不执行
				if (core.cancelled.erase(node.id) > 0)
				{
					continue;
				}

				// 先挪出来再调，回调里干了什么都不会影响 node 自己的生命周期
				auto callback = std::move(node.callback);
				if (callback)
				{
					callback();
				}
			}

			rearm(core);
		}

		/**
		 * @brief 清掉取消表里已经没意义的 id
		 * 正常情况取消的都是还挂在堆里的，节点摘出来时顺手 erase 掉，表一直是小的。
		 * 但如果有人取消一个已经触发过的 id，那个标记就没人清了，攒多了一直涨——
		 * 超过堆里实际节点数太多时就按堆重建一次，把幽灵 id 都扔掉。
		 * 重建只从堆里捞活标记，所以 fireDue 跑着那一批的时候不能重建：那批已经不在堆里，
		 * 它们的取消标记会被当幽灵扔掉，取消就失效了（inFireDue 拦的就是这个）。
		 */
		void compactCancelled(detail::TimerServiceCore& core)
		{
			if (core.inFireDue)
			{
				return;
			}

			if (core.cancelled.size() <= core.heap.size() + kCancelledSlack)
			{
				return;
			}

			std::unordered_set<TimerService::TimerId> live;
			live.reserve(core.heap.size());
			for (const auto& node : core.heap)
			{
				if (core.cancelled.count(node.id) > 0)
				{
					live.insert(node.id);
				}
			}
			core.cancelled.swap(live);
		}

		/**
		 * @brief 把堆顶的 deadline 挂到那个唯一的 timer 上
		 * 已经挂着更早的就什么都不做——这条是省 epoll_ctl 的关键：连着挂 N 个定时器时
		 * 只有刷新最小值的少数几次会真正动 timer。
		 */
		void rearm(detail::TimerServiceCore& core)
		{
			if (core.stopped || core.heap.empty())
			{
				core.armedDeadline.reset();
				return;
			}

			const auto top = core.heap.front().deadline;
			if (core.armedDeadline.has_value() && *core.armedDeadline <= top)
			{
				return;
			}

			core.armedDeadline = top;
			core.timer.expires_at(top);
			core.timer.async_wait(
				[weak = core.weak_from_this()](const boost::system::error_code& ec)
				{
					// 服务没了就闪人；ec 是重挂时被 expires_at abort 的老等，也别管
					auto self = weak.lock();
					if (!self || ec)
					{
						return;
					}
					fireDue(*self);
				});
		}
	} // namespace

	TimerService* currentThreadTimerService()
	{
		return tls_timerService;
	}

	TimerService::TimerService(boost::asio::any_io_executor executor)
		: core_(std::make_shared<detail::TimerServiceCore>(std::move(executor)))
	{
	}

	TimerService::~TimerService()
	{
		shutdown();
		unbindFromCurrentThread();
	}

	TimerService::TimerId TimerService::schedule(Clock::time_point deadline, Callback callback)
	{
		auto& core = *core_;
		if (core.stopped)
		{
			return kInvalidId;
		}

		const auto id = core.nextId++;
		core.push(detail::TimerServiceCore::Node {deadline, id, std::move(callback)});
		rearm(core);
		return id;
	}

	TimerService::TimerId TimerService::scheduleAfter(Clock::duration delay, Callback callback)
	{
		if (delay < Clock::duration::zero())
		{
			delay = Clock::duration::zero();
		}
		return schedule(Clock::now() + delay, std::move(callback));
	}

	bool TimerService::cancel(TimerId id)
	{
		auto& core = *core_;
		// id 比 nextId 小说明根本没派出去过
		if (core.stopped || id == kInvalidId || id >= core.nextId)
		{
			return false;
		}

		if (!core.cancelled.insert(id).second)
		{
			return false;
		}

		compactCancelled(core);
		return true;
	}

	void TimerService::shutdown()
	{
		auto& core = *core_;
		if (core.stopped)
		{
			return;
		}

		core.stopped = true;
		core.armedDeadline.reset();
		core.cancelled.clear();
		// heap.clear() 会把没触发的 callback 一起析构掉——按约定这里就是不执行
		core.heap.clear();

		// 挂着的 async_wait 会带着 operation_aborted 回来，handler 里直接忽略
		core.timer.cancel();
	}

	std::size_t TimerService::pendingCount() const
	{
		// 现数堆，不维护计数器。计数器得在 schedule、cancel、触发三处各改一次，而 cancel
		// 这条路上判断不出那个 id 的节点还在不在堆里（取消一个已经触发过的 id 时标记照样
		// 插得进去），一处判错整个数就开始漂，扣穿一次就下溢成 SIZE_MAX。这个接口只有测试
		// 和诊断在用，一次 O(堆大小) 换掉这份账是划算的。
		const auto& core = *core_;
		if (core.cancelled.empty())
		{
			return core.heap.size();
		}

		std::size_t live = 0;
		for (const auto& node : core.heap)
		{
			if (core.cancelled.count(node.id) == 0)
			{
				++live;
			}
		}
		return live;
	}

	void TimerService::bindToCurrentThread()
	{
		tls_timerService = this;
	}

	void TimerService::unbindFromCurrentThread()
	{
		if (tls_timerService == this)
		{
			tls_timerService = nullptr;
		}
	}

} // namespace hical
