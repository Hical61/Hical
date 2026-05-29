/**
 * @file DbConnectionPool.cpp
 * @brief 连接池实现
 */

#include "DbConnectionPool.h"
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <stdexcept>

namespace hical::db
{

	DbConnectionPool::DbConnectionPool(boost::asio::io_context& ioCtx, DbConfig config, DbConnectionFactory factory)
		: ioCtx_(ioCtx), config_(std::move(config)), factory_(std::move(factory))
	{
	}

	DbConnectionPool::~DbConnectionPool()
	{
		shutdown_.store(true, std::memory_order_relaxed);
	}

	Awaitable<void> DbConnectionPool::init()
	{
		if (initialized_)
		{
			throw std::logic_error("DbConnectionPool::init() called more than once");
		}
		initialized_ = true;

		// 预创建 minConnections 个连接
		for (size_t i = 0; i < config_.minConnections; ++i)
		{
			auto conn = co_await factory_(ioCtx_, config_);
			if (conn)
			{
				conn->touch();
				std::lock_guard lock(mutex_);
				idle_.push_back(std::move(conn));
			}
		}

		// 启动空闲回收定时器（仅当 idleCheckInterval > 0 时）
		if (config_.idleCheckInterval.count() > 0)
		{
			startIdleChecker();
		}

		// 启动后台健康检查（仅当 healthCheckInterval > 0 时）
		if (config_.healthCheckInterval.count() > 0)
		{
			startHealthChecker();
		}
	}

	Awaitable<std::shared_ptr<DbConnection>> DbConnectionPool::acquire()
	{
		if (shutdown_.load(std::memory_order_relaxed))
		{
			throw std::runtime_error("DbConnectionPool: pool is shut down");
		}

		std::unique_lock lock(mutex_);
		// 死连接先攒着，出了锁再析构，免得持锁时卡在 socket 关闭上
		std::vector<std::shared_ptr<DbConnection>> deadConns;

		// 1. 有空闲连接 → 直接返回
		while (!idle_.empty())
		{
			auto conn = std::move(idle_.back());
			idle_.pop_back();

			// 先看一眼本地状态，死了就扔掉，省得白 ping
			if (!conn->isAlive())
			{
				deadConns.push_back(std::move(conn));
				continue;
			}

			++activeCount_;
			lock.unlock();
			deadConns.clear(); // 锁外析构已收集的死连接

			// 若健康检查已启用且连接最近被 ping 过，跳过 ping
			bool skipPing = config_.healthCheckInterval.count() > 0 && config_.pingGracePeriod.count() > 0;
			if (skipPing)
			{
				auto sinceLastPing = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now()
																					  - conn->lastPingTime());
				if (sinceLastPing < config_.pingGracePeriod)
				{
					conn->touch();
					co_return conn;
				}
			}

			// 回退路径：ping 检查活性
			bool alive = false;
			try
			{
				alive = co_await conn->ping();
			}
			catch (...)
			{
				alive = false;
			}

			if (alive)
			{
				conn->touch();
				co_return conn;
			}

			// 连接已死，减计数，重新尝试
			lock.lock();
			--activeCount_;
		}

		// 2. 未达上限 → 创建新连接
		if (activeCount_ + idle_.size() < config_.maxConnections)
		{
			++activeCount_;
			lock.unlock();

			try
			{
				auto conn = co_await factory_(ioCtx_, config_);
				conn->touch();
				co_return conn;
			}
			catch (...)
			{
				std::lock_guard rollback(mutex_);
				--activeCount_;
				throw;
			}
		}

		// 3. 池满 → 协程挂起等待归还
		auto timer = std::make_shared<boost::asio::steady_timer>(ioCtx_, config_.acquireTimeout);
		auto result = std::make_shared<std::shared_ptr<DbConnection>>();
		waiters_.push_back({timer, result});
		lock.unlock();

		boost::system::error_code ec;
		co_await timer->async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, ec));

		if (*result)
		{
			// release() 已将连接转交
			std::lock_guard countLock(mutex_);
			++activeCount_;
			(*result)->touch();
			co_return std::move(*result);
		}

		// 超时：尝试从等待队列移除自己（按 timer 匹配）
		bool removed = false;
		{
			std::lock_guard cleanLock(mutex_);
			auto before = waiters_.size();
			std::erase_if(waiters_,
						  [&timer](const Waiter& w)
						  {
							  return w.timer == timer;
						  });
			removed = (waiters_.size() < before);
		}

		// 如果未从队列移除，说明 release() 已 pop 了我们——检查是否已转交连接
		if (!removed && *result)
		{
			// 连接在我们检查之前的瞬间被转交，正常使用
			std::lock_guard countLock(mutex_);
			++activeCount_;
			(*result)->touch();
			co_return std::move(*result);
		}

		// 如果连接被转交但我们要超时，归还它避免泄漏
		if (*result)
		{
			release(std::move(*result));
		}

		throw std::runtime_error("DbConnectionPool: acquire timeout");
	}

	void DbConnectionPool::release(std::shared_ptr<DbConnection> conn)
	{
		if (!conn)
		{
			return;
		}

		// 还在事务里的连接，先 rollback 再归还
		if (conn->inTransaction())
		{
			// 先不减 activeCount_，回滚完才算真正归还
			auto connPtr = std::move(conn);
			auto self = shared_from_this();
			coSpawn(
				ioCtx_,
				[self, connPtr]() mutable -> Awaitable<void>
				{
					try
					{
						co_await connPtr->rollback();
					}
					catch (...)
					{
						// 回滚失败，连接不可复用，直接丢弃并减计数
						std::lock_guard lock(self->mutex_);
						if (self->activeCount_ > 0)
						{
							--self->activeCount_;
						}
						co_return;
					}
					// 回滚成功，减计数并归入空闲池或转交等待者
					connPtr->touch();
					std::lock_guard lock(self->mutex_);
					if (self->activeCount_ > 0)
					{
						--self->activeCount_;
					}
					if (self->shutdown_.load(std::memory_order_relaxed))
					{
						co_return;
					}
					if (!self->waiters_.empty())
					{
						self->wakeOneWaiter(std::move(connPtr));
					}
					else
					{
						self->idle_.push_back(std::move(connPtr));
					}
				},
				[](std::exception_ptr)
				{
				});
			return;
		}

		std::lock_guard lock(mutex_);

		if (activeCount_ > 0)
		{
			--activeCount_;
		}

		// 如果有等待者，直接转交连接
		if (!waiters_.empty())
		{
			auto waiter = std::move(waiters_.front());
			waiters_.pop_front();
			*(waiter.result) = std::move(conn);
			waiter.timer->cancel();
			return;
		}

		// 无等待者 → 归入空闲池
		if (!shutdown_.load(std::memory_order_relaxed))
		{
			conn->touch();
			idle_.push_back(std::move(conn));
		}
	}

	Awaitable<void> DbConnectionPool::shutdown()
	{
		shutdown_.store(true, std::memory_order_relaxed);

		// 取消后台循环 timer，使其立即退出
		if (idleCheckTimer_)
		{
			idleCheckTimer_->cancel();
		}
		if (healthCheckTimer_)
		{
			healthCheckTimer_->cancel();
		}

		std::lock_guard lock(mutex_);

		// 唤醒所有等待者（result 为空，acquire 侧会抛异常）
		for (auto& waiter : waiters_)
		{
			waiter.timer->cancel();
		}
		waiters_.clear();

		// 清理空闲连接
		idle_.clear();

		co_return;
	}

	size_t DbConnectionPool::activeCount() const
	{
		std::lock_guard lock(mutex_);
		return activeCount_;
	}

	size_t DbConnectionPool::idleCount() const
	{
		std::lock_guard lock(mutex_);
		return idle_.size();
	}

	size_t DbConnectionPool::waitingCount() const
	{
		std::lock_guard lock(mutex_);
		return waiters_.size();
	}

	size_t DbConnectionPool::totalCount() const
	{
		std::lock_guard lock(mutex_);
		return activeCount_ + idle_.size();
	}

	void DbConnectionPool::wakeOneWaiter(std::shared_ptr<DbConnection> conn)
	{
		// 已在锁内调用
		if (!waiters_.empty())
		{
			auto waiter = std::move(waiters_.front());
			waiters_.pop_front();
			*(waiter.result) = std::move(conn);
			waiter.timer->cancel();
		}
	}

	// 后台协程持有 self，不调 shutdown() 池对象不会析构
	void DbConnectionPool::startIdleChecker()
	{
		auto self = shared_from_this();
		hical::coSpawn(ioCtx_,
					   [self]() -> Awaitable<void>
					   {
						   co_await self->idleCheckLoop();
					   });
	}

	Awaitable<void> DbConnectionPool::idleCheckLoop()
	{
		idleCheckTimer_ = std::make_shared<boost::asio::steady_timer>(ioCtx_);
		while (!shutdown_.load(std::memory_order_relaxed))
		{
			// 算出最近哪个连接要过期，timer 直接设到那个时间点，别傻等固定间隔
			std::chrono::steady_clock::time_point nextExpire;
			{
				std::lock_guard lock(mutex_);
				if (idle_.empty())
				{
					// 池空了就按默认间隔睡一觉
					nextExpire = std::chrono::steady_clock::now() + config_.idleCheckInterval;
				}
				else
				{
					// 找最老的连接（最早过期）
					auto oldest = std::min_element(idle_.begin(),
												   idle_.end(),
												   [](const auto& a, const auto& b)
												   {
													   return a->lastActiveTime() < b->lastActiveTime();
												   });
					nextExpire = (*oldest)->lastActiveTime() + config_.idleTimeout;

					// 但也别等太久，最多就 idleCheckInterval，不然新还回来的连接没人管
					auto maxWait = std::chrono::steady_clock::now() + config_.idleCheckInterval;
					if (nextExpire > maxWait)
					{
						nextExpire = maxWait;
					}
				}
			}

			idleCheckTimer_->expires_at(nextExpire);
			boost::system::error_code ec;
			co_await idleCheckTimer_->async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, ec));

			if (shutdown_.load(std::memory_order_relaxed))
			{
				break;
			}

			auto now = std::chrono::steady_clock::now();
			std::lock_guard lock(mutex_);

			// 移除超时连接，保留至少 minConnections 个
			size_t remaining = idle_.size();
			for (auto it = idle_.begin(); it != idle_.end();)
			{
				if (remaining <= config_.minConnections)
				{
					break;
				}
				auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - (*it)->lastActiveTime());
				if (elapsed >= config_.idleTimeout)
				{
					it = idle_.erase(it);
					--remaining;
				}
				else
				{
					++it;
				}
			}
		}
	}

	// 同上，协程持有 self
	void DbConnectionPool::startHealthChecker()
	{
		auto self = shared_from_this();
		hical::coSpawn(ioCtx_,
					   [self]() -> Awaitable<void>
					   {
						   co_await self->healthCheckLoop();
					   });
	}

	Awaitable<void> DbConnectionPool::healthCheckLoop()
	{
		healthCheckTimer_ = std::make_shared<boost::asio::steady_timer>(ioCtx_);
		while (!shutdown_.load(std::memory_order_relaxed))
		{
			healthCheckTimer_->expires_after(config_.healthCheckInterval);
			boost::system::error_code ec;
			co_await healthCheckTimer_->async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, ec));

			if (shutdown_.load(std::memory_order_relaxed))
			{
				break;
			}

			// 先把空闲连接全拿出来，ping 期间 acquire 就碰不到它们了
			std::vector<std::shared_ptr<DbConnection>> checking;
			{
				std::lock_guard lock(mutex_);
				checking = std::move(idle_);
				idle_.clear();
				activeCount_ += checking.size();
			}

			// 逐个 ping，这时候不持锁
			std::vector<std::shared_ptr<DbConnection>> alive;
			alive.reserve(checking.size());
			for (auto& conn : checking)
			{
				if (shutdown_.load(std::memory_order_relaxed))
				{
					// shutdown 了，把存活的放回去让 shutdown 清
					std::lock_guard lock(mutex_);
					for (auto& c : alive)
					{
						idle_.push_back(std::move(c));
					}
					if (activeCount_ >= checking.size())
					{
						activeCount_ -= checking.size();
					}
					else
					{
						activeCount_ = 0;
					}
					co_return;
				}
				bool ok = false;
				try
				{
					ok = co_await conn->ping();
				}
				catch (...)
				{
					ok = false;
				}
				if (ok)
				{
					alive.push_back(std::move(conn));
				}
				// 死连接直接丢弃（shared_ptr 析构释放）
			}

			// 活着的放回去，看看要不要补
			size_t deficit = 0;
			{
				std::lock_guard lock(mutex_);
				// 还回检查期间临时占的计数
				if (activeCount_ >= checking.size())
				{
					activeCount_ -= checking.size();
				}
				else
				{
					activeCount_ = 0;
				}
				for (auto& conn : alive)
				{
					idle_.push_back(std::move(conn));
				}
				size_t total = idle_.size() + activeCount_;
				if (total < config_.minConnections)
				{
					deficit = config_.minConnections - total;
				}
			}

			// 补充连接到 minConnections
			for (size_t i = 0; i < deficit; ++i)
			{
				if (shutdown_.load(std::memory_order_relaxed))
				{
					co_return;
				}
				try
				{
					auto newConn = co_await factory_(ioCtx_, config_);
					if (newConn)
					{
						newConn->touch();
						std::lock_guard lock(mutex_);
						idle_.push_back(std::move(newConn));
					}
				}
				catch (...)
				{
					// 补充失败，下次循环重试
				}
			}
		}
	}

} // namespace hical::db
