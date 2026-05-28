/**
 * @file AsioTimer.cpp
 * @brief Asio 定时器实现
 */

#include "AsioTimer.h"
#include "AsioEventLoop.h"

namespace hical
{

	AsioTimer::AsioTimer(AsioEventLoop* loop, double delay, Callback cb)
		: loop_(loop), timer_(loop->getIoContext()), callback_(std::move(cb)), interval_(delay), repeating_(false)
	{
	}

	AsioTimer::AsioTimer(AsioEventLoop* loop, double interval, Callback cb, bool repeating)
		: loop_(loop)
		, timer_(loop->getIoContext())
		, callback_(std::move(cb))
		, interval_(interval)
		, repeating_(repeating)
	{
	}

	AsioTimer::~AsioTimer()
	{
		// 析构时直接取消：此时无 shared_ptr 引用，不可能有并发 async_wait
		cancelled_.store(true);
		timer_.cancel();
	}

	void AsioTimer::cancel()
	{
		if (!cancelled_.exchange(true))
		{
			// timer_ 必须在其所属 io_context 线程操作，跨线程直接调用不安全
			boost::asio::post(timer_.get_executor(),
							  [self = shared_from_this()]()
							  {
								  self->timer_.cancel();
							  });
		}
	}

	bool AsioTimer::isActive() const
	{
		return !cancelled_.load();
	}

	EventLoop* AsioTimer::getLoop() const
	{
		return loop_;
	}

	bool AsioTimer::isRepeating() const
	{
		return repeating_;
	}

	double AsioTimer::interval() const
	{
		return interval_;
	}

	void AsioTimer::start()
	{
		if (repeating_)
		{
			scheduleRepeating();
		}
		else
		{
			scheduleOnce();
		}
	}

	void AsioTimer::scheduleOnce()
	{
		if (cancelled_.load())
		{
			return;
		}

		timer_.expires_after(std::chrono::milliseconds(static_cast<int64_t>(interval_ * 1000)));

		timer_.async_wait(
			[this, self = shared_from_this()](const boost::system::error_code& ec)
			{
				handleTimeout(ec);
			});
	}

	void AsioTimer::scheduleRepeating()
	{
		if (cancelled_.load())
		{
			return;
		}

		timer_.expires_after(std::chrono::milliseconds(static_cast<int64_t>(interval_ * 1000)));

		timer_.async_wait(
			[this, self = shared_from_this()](const boost::system::error_code& ec)
			{
				if (ec || cancelled_.load())
				{
					return;
				}

				// 执行回调
				callback_();

				// 重新调度
				if (!cancelled_.load())
				{
					scheduleRepeating();
				}
			});
	}

	void AsioTimer::handleTimeout(const boost::system::error_code& ec)
	{
		if (ec || cancelled_.load())
		{
			return;
		}

		// 执行回调
		callback_();

		// 单次定时器触发后自动从 EventLoop 的 timers_ map 中移除自身，防止泄漏
		if (!repeating_ && cleanupCallback_)
		{
			cleanupCallback_(timerId_);
		}
	}

	void AsioTimer::setCleanup(uint64_t id, std::function<void(uint64_t)> cleanup)
	{
		timerId_ = id;
		cleanupCallback_ = std::move(cleanup);
	}

} // namespace hical
