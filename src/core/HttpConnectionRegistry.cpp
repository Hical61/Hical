/**
 * @file HttpConnectionRegistry.cpp
 * @brief HTTP 连接控制登记及所属事件循环通知
 */

#include "HttpConnectionRegistry.h"

#ifdef HICAL_HAS_HTTP2

	#include <boost/asio/dispatch.hpp>
	#include <mutex>
	#include <stdexcept>
	#include <unordered_set>
	#include <utility>
	#include <vector>

namespace hical::detail
{
	struct HttpConnectionRegistry::Control
	{
		boost::asio::any_io_executor executor;
		Callback close;
		Callback drain;
		// 下列字段只在所属事件循环访问，无需加锁。
		bool active {true};
		bool closeRequested {false};
		bool drainRequested {false};
		bool drainDelivered {false};

		void deliverDrain()
		{
			if (active && !closeRequested && drainRequested && !drainDelivered && drain)
			{
				drainDelivered = true;
				// 回调可以同步注销自己；本地副本保证正在执行的闭包不会被销毁。
				auto callback = drain;
				callback();
			}
		}

		void notify(bool closing)
		{
			if (!active || closeRequested)
			{
				return;
			}
			if (closing)
			{
				closeRequested = true;
				auto callback = close;
				callback();
			}
			else
			{
				drainRequested = true;
				deliverDrain();
			}
		}
	};

	struct HttpConnectionRegistry::State
	{
		// 只保护连接集合及快照构造，不在持锁期间调用用户回调或投递事件。
		mutable std::mutex mutex;
		std::unordered_set<std::shared_ptr<Control>> controls;
	};

	HttpConnectionRegistry::HttpConnectionRegistry() : state_(std::make_shared<State>())
	{
	}

	HttpConnectionRegistry::~HttpConnectionRegistry() = default;

	HttpConnectionRegistry::Registration::Registration(std::shared_ptr<State> state, std::shared_ptr<Control> control)
		: state_(std::move(state)), control_(std::move(control))
	{
	}

	HttpConnectionRegistry::Registration::~Registration()
	{
		reset();
	}

	HttpConnectionRegistry::Registration::Registration(Registration&& other) noexcept
		: state_(std::move(other.state_)), control_(std::move(other.control_))
	{
	}

	HttpConnectionRegistry::Registration& HttpConnectionRegistry::Registration::operator=(Registration&& other) noexcept
	{
		if (this != &other)
		{
			reset();
			state_ = std::move(other.state_);
			control_ = std::move(other.control_);
		}
		return *this;
	}

	void HttpConnectionRegistry::Registration::reset() noexcept
	{
		if (control_)
		{
			control_->active = false;
			control_->close = {};
			control_->drain = {};
			{
				std::lock_guard lock(state_->mutex);
				state_->controls.erase(control_);
			}
			control_.reset();
			state_.reset();
		}
	}

	void HttpConnectionRegistry::Registration::setDrainHandler(Callback callback)
	{
		auto control = control_;
		if (control)
		{
			control->drain = std::move(callback);
			control->deliverDrain();
		}
	}

	bool HttpConnectionRegistry::Registration::draining() const noexcept
	{
		return control_ && control_->drainRequested;
	}

	HttpConnectionRegistry::Registration HttpConnectionRegistry::add(boost::asio::any_io_executor executor,
																	 Callback close)
	{
		if (!close)
		{
			throw std::invalid_argument("连接关闭回调不得为空");
		}
		auto control = std::make_shared<Control>(Control {std::move(executor), std::move(close), {}});
		{
			std::lock_guard lock(state_->mutex);
			state_->controls.insert(control);
		}
		return Registration(state_, std::move(control));
	}

	void HttpConnectionRegistry::requestClose()
	{
		notify(true);
	}

	void HttpConnectionRegistry::requestDrain()
	{
		notify(false);
	}

	size_t HttpConnectionRegistry::size() const
	{
		std::lock_guard lock(state_->mutex);
		return state_->controls.size();
	}

	void HttpConnectionRegistry::notify(bool close)
	{
		std::vector<std::shared_ptr<Control>> snapshot;
		{
			std::lock_guard lock(state_->mutex);
			snapshot.assign(state_->controls.begin(), state_->controls.end());
		}
		for (auto& control : snapshot)
		{
			boost::asio::dispatch(control->executor,
								  [control, close]()
								  {
									  control->notify(close);
								  });
		}
	}
} // namespace hical::detail

#endif
