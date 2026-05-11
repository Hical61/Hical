#include "DbConnectionPool.h"
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <stdexcept>

namespace hical::db
{

	DbConnectionPool::DbConnectionPool(boost::asio::io_context& ioCtx, DbConfig config, DbConnectionFactory factory)
		: m_ioCtx(ioCtx), m_config(std::move(config)), m_factory(std::move(factory))
	{
	}

	DbConnectionPool::~DbConnectionPool()
	{
		m_shutdown.store(true, std::memory_order_relaxed);
	}

	Awaitable<void> DbConnectionPool::init()
	{
		if (m_initialized)
		{
			throw std::logic_error("DbConnectionPool::init() called more than once");
		}
		m_initialized = true;

		// 预创建 minConnections 个连接
		for (size_t i = 0; i < m_config.minConnections; ++i)
		{
			auto conn = co_await m_factory(m_ioCtx, m_config);
			if (conn)
			{
				conn->touch();
				std::lock_guard lock(m_mutex);
				m_idle.push_back(std::move(conn));
			}
		}

		// 启动空闲回收定时器（仅当 idleCheckInterval > 0 时）
		if (m_config.idleCheckInterval.count() > 0)
		{
			startIdleChecker();
		}

		// 启动后台健康检查（仅当 healthCheckInterval > 0 时）
		if (m_config.healthCheckInterval.count() > 0)
		{
			startHealthChecker();
		}
	}

	Awaitable<std::shared_ptr<DbConnection>> DbConnectionPool::acquire()
	{
		if (m_shutdown.load(std::memory_order_relaxed))
		{
			throw std::runtime_error("DbConnectionPool: pool is shut down");
		}

		std::unique_lock lock(m_mutex);

		// 1. 有空闲连接 → 直接返回
		while (!m_idle.empty())
		{
			auto conn = std::move(m_idle.back());
			m_idle.pop_back();
			++m_activeCount;
			lock.unlock();

			// 若健康检查已启用且连接最近被 ping 过，跳过 ping
			bool skipPing = m_config.healthCheckInterval.count() > 0 && m_config.pingGracePeriod.count() > 0;
			if (skipPing)
			{
				auto sinceLastPing = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now()
																					  - conn->lastPingTime());
				if (sinceLastPing < m_config.pingGracePeriod)
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
			--m_activeCount;
		}

		// 2. 未达上限 → 创建新连接
		if (m_activeCount + m_idle.size() < m_config.maxConnections)
		{
			++m_activeCount;
			lock.unlock();

			try
			{
				auto conn = co_await m_factory(m_ioCtx, m_config);
				conn->touch();
				co_return conn;
			}
			catch (...)
			{
				std::lock_guard rollback(m_mutex);
				--m_activeCount;
				throw;
			}
		}

		// 3. 池满 → 协程挂起等待归还
		auto timer = std::make_shared<boost::asio::steady_timer>(m_ioCtx, m_config.acquireTimeout);
		auto result = std::make_shared<std::shared_ptr<DbConnection>>();
		m_waiters.push_back({timer, result});
		lock.unlock();

		boost::system::error_code ec;
		co_await timer->async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, ec));

		if (*result)
		{
			// release() 已将连接转交
			std::lock_guard countLock(m_mutex);
			++m_activeCount;
			(*result)->touch();
			co_return std::move(*result);
		}

		// 超时：尝试从等待队列移除自己（按 timer 匹配）
		bool removed = false;
		{
			std::lock_guard cleanLock(m_mutex);
			auto before = m_waiters.size();
			std::erase_if(m_waiters,
						  [&timer](const Waiter& w)
						  {
							  return w.timer == timer;
						  });
			removed = (m_waiters.size() < before);
		}

		// 如果未从队列移除，说明 release() 已 pop 了我们——检查是否已转交连接
		if (!removed && *result)
		{
			// 连接在我们检查之前的瞬间被转交，正常使用
			std::lock_guard countLock(m_mutex);
			++m_activeCount;
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

		// 如果连接残留事务，异步回滚后再减 activeCount（保持计数一致性）
		if (conn->inTransaction())
		{
			// 不在此处减 m_activeCount！连接仍计为"活跃"直到回滚完成，
			// 避免 totalCount() 低估导致创建超出 maxConnections 的连接。
			auto connPtr = std::move(conn);
			auto self = shared_from_this();
			coSpawn(
				m_ioCtx,
				[self, connPtr]() mutable -> Awaitable<void>
				{
					try
					{
						co_await connPtr->rollback();
					}
					catch (...)
					{
						// 回滚失败，连接不可复用，直接丢弃并减计数
						std::lock_guard lock(self->m_mutex);
						if (self->m_activeCount > 0)
						{
							--self->m_activeCount;
						}
						co_return;
					}
					// 回滚成功，减计数并归入空闲池或转交等待者
					connPtr->touch();
					std::lock_guard lock(self->m_mutex);
					if (self->m_activeCount > 0)
					{
						--self->m_activeCount;
					}
					if (self->m_shutdown.load(std::memory_order_relaxed))
					{
						co_return;
					}
					if (!self->m_waiters.empty())
					{
						self->wakeOneWaiter(std::move(connPtr));
					}
					else
					{
						self->m_idle.push_back(std::move(connPtr));
					}
				},
				[](std::exception_ptr)
				{
				});
			return;
		}

		std::lock_guard lock(m_mutex);

		if (m_activeCount > 0)
		{
			--m_activeCount;
		}

		// 如果有等待者，直接转交连接
		if (!m_waiters.empty())
		{
			auto waiter = std::move(m_waiters.front());
			m_waiters.pop_front();
			*(waiter.result) = std::move(conn);
			waiter.timer->cancel();
			return;
		}

		// 无等待者 → 归入空闲池
		if (!m_shutdown.load(std::memory_order_relaxed))
		{
			conn->touch();
			m_idle.push_back(std::move(conn));
		}
	}

	Awaitable<void> DbConnectionPool::shutdown()
	{
		m_shutdown.store(true, std::memory_order_relaxed);

		// 取消后台循环 timer，使其立即退出
		if (m_idleCheckTimer)
		{
			m_idleCheckTimer->cancel();
		}
		if (m_healthCheckTimer)
		{
			m_healthCheckTimer->cancel();
		}

		std::lock_guard lock(m_mutex);

		// 唤醒所有等待者（result 为空，acquire 侧会抛异常）
		for (auto& waiter : m_waiters)
		{
			waiter.timer->cancel();
		}
		m_waiters.clear();

		// 清理空闲连接
		m_idle.clear();

		co_return;
	}

	size_t DbConnectionPool::activeCount() const
	{
		std::lock_guard lock(m_mutex);
		return m_activeCount;
	}

	size_t DbConnectionPool::idleCount() const
	{
		std::lock_guard lock(m_mutex);
		return m_idle.size();
	}

	size_t DbConnectionPool::waitingCount() const
	{
		std::lock_guard lock(m_mutex);
		return m_waiters.size();
	}

	size_t DbConnectionPool::totalCount() const
	{
		std::lock_guard lock(m_mutex);
		return m_activeCount + m_idle.size();
	}

	void DbConnectionPool::wakeOneWaiter(std::shared_ptr<DbConnection> conn)
	{
		// 已在锁内调用
		if (!m_waiters.empty())
		{
			auto waiter = std::move(m_waiters.front());
			m_waiters.pop_front();
			*(waiter.result) = std::move(conn);
			waiter.timer->cancel();
		}
	}

	void DbConnectionPool::startIdleChecker()
	{
		auto self = shared_from_this();
		hical::coSpawn(m_ioCtx,
					   [self]() -> Awaitable<void>
					   {
						   co_await self->idleCheckLoop();
					   });
	}

	Awaitable<void> DbConnectionPool::idleCheckLoop()
	{
		m_idleCheckTimer = std::make_shared<boost::asio::steady_timer>(m_ioCtx);
		while (!m_shutdown.load(std::memory_order_relaxed))
		{
			m_idleCheckTimer->expires_after(m_config.idleCheckInterval);
			boost::system::error_code ec;
			co_await m_idleCheckTimer->async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, ec));

			if (m_shutdown.load(std::memory_order_relaxed))
			{
				break;
			}

			auto now = std::chrono::steady_clock::now();
			std::lock_guard lock(m_mutex);

			// 移除超时连接，保留至少 minConnections 个
			size_t remaining = m_idle.size();
			for (auto it = m_idle.begin(); it != m_idle.end();)
			{
				if (remaining <= m_config.minConnections)
				{
					break;
				}
				auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - (*it)->lastActiveTime());
				if (elapsed >= m_config.idleTimeout)
				{
					it = m_idle.erase(it);
					--remaining;
				}
				else
				{
					++it;
				}
			}
		}
	}

	void DbConnectionPool::startHealthChecker()
	{
		auto self = shared_from_this();
		hical::coSpawn(m_ioCtx,
					   [self]() -> Awaitable<void>
					   {
						   co_await self->healthCheckLoop();
					   });
	}

	Awaitable<void> DbConnectionPool::healthCheckLoop()
	{
		m_healthCheckTimer = std::make_shared<boost::asio::steady_timer>(m_ioCtx);
		while (!m_shutdown.load(std::memory_order_relaxed))
		{
			m_healthCheckTimer->expires_after(m_config.healthCheckInterval);
			boost::system::error_code ec;
			co_await m_healthCheckTimer->async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, ec));

			if (m_shutdown.load(std::memory_order_relaxed))
			{
				break;
			}

			// 将空闲连接移出（非拷贝），防止 acquire() 在 ping 期间取到同一连接
			std::vector<std::shared_ptr<DbConnection>> checking;
			{
				std::lock_guard lock(m_mutex);
				checking = std::move(m_idle);
				m_idle.clear();
				m_activeCount += checking.size();
			}

			// 逐个 ping（不持锁，连接已从池中移出，不会被 acquire 取到）
			std::vector<std::shared_ptr<DbConnection>> alive;
			alive.reserve(checking.size());
			for (auto& conn : checking)
			{
				if (m_shutdown.load(std::memory_order_relaxed))
				{
					// shutdown 时将存活连接放回（由 shutdown 清理），同时归还检查期间占用的计数
					std::lock_guard lock(m_mutex);
					for (auto& c : alive)
					{
						m_idle.push_back(std::move(c));
					}
					if (m_activeCount >= checking.size())
					{
						m_activeCount -= checking.size();
					}
					else
					{
						m_activeCount = 0;
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

			// 将存活连接放回空闲池，计算补充数量
			size_t deficit = 0;
			{
				std::lock_guard lock(m_mutex);
				// 先减去检查期间占用的计数
				if (m_activeCount >= checking.size())
				{
					m_activeCount -= checking.size();
				}
				else
				{
					m_activeCount = 0;
				}
				for (auto& conn : alive)
				{
					m_idle.push_back(std::move(conn));
				}
				size_t total = m_idle.size() + m_activeCount;
				if (total < m_config.minConnections)
				{
					deficit = m_config.minConnections - total;
				}
			}

			// 补充连接到 minConnections
			for (size_t i = 0; i < deficit; ++i)
			{
				if (m_shutdown.load(std::memory_order_relaxed))
				{
					co_return;
				}
				try
				{
					auto newConn = co_await m_factory(m_ioCtx, m_config);
					if (newConn)
					{
						newConn->touch();
						std::lock_guard lock(m_mutex);
						m_idle.push_back(std::move(newConn));
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
