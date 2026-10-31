/**
 * @file HttpClientPool.cpp
 * @brief 连接池实现：分桶复用、空闲清理、生命周期托管
 * 分桶身份持有 TLS context，安全策略与生效的 H2 配置逐字段隔离。
 * 请求和清理协程持有 Impl，池对象析构后仍可完成异步收尾。
 */

#include "HttpClientPool.h"
#include <boost/asio/post.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <algorithm>
#include <atomic>
#include <deque>
#include <iterator>
#include <list>
#include <optional>
#include <stdexcept>
#include <tuple>
#include <unordered_map>
#include <utility>

namespace hical
{
	namespace
	{
		using Clock = std::chrono::steady_clock;

		auto http2Fields(const Http2Options& options) noexcept
		{
			return std::tie(options.maxConcurrentStreams,
							options.maxApplicationTasks,
							options.maxServerApplicationTasks,
							options.maxHeaderBytes,
							options.maxHeaderFields,
							options.maxBodyBytes,
							options.maxConnectionReceiveBytes,
							options.maxConnectionSendBytes,
							options.maxOutputBatchBytes,
							options.maxFilePrefetchBytes,
							options.maxResponseBytes,
							options.maxClientConnectionReceiveBytes,
							options.maxConnectionsPerOrigin,
							options.maxQueuedRequestsPerOrigin,
							options.initialStreamWindow,
							options.initialConnectionWindow,
							options.maxFrameBytes,
							options.headerTableBytes,
							options.negotiationTimeout,
							options.headerTimeout,
							options.bodyTimeout,
							options.writeTimeout,
							options.queueTimeout);
		}

		struct PoolIdentity
		{
			std::string host;
			uint16_t port;
			std::shared_ptr<SslContext> tlsContext;
			std::string sni;
			bool verifyPeer;
			HttpProtocolMode mode;
			Http2Options http2;

			PoolIdentity(const std::string& target, uint16_t targetPort, const HttpClientOptions& options)
				: host(target)
				, port(targetPort)
				, tlsContext(options.tlsContext)
				, sni(options.sniHost.empty() ? target : options.sniHost)
				, verifyPeer(options.verifyPeer)
				, mode(options.protocolMode)
				, http2(mode == HttpProtocolMode::hHttp1Only ? Http2Options {} : options.http2)
			{
			}

			bool operator==(const PoolIdentity& other) const noexcept
			{
				return host == other.host && port == other.port && tlsContext == other.tlsContext && sni == other.sni
					   && verifyPeer == other.verifyPeer && mode == other.mode
					   && http2Fields(http2) == http2Fields(other.http2);
			}
		};

		struct IdentityHash
		{
			template <typename Value>
			static void append(size_t& seed, const Value& value) noexcept
			{
				constexpr size_t kHashMix = 0x9e3779b9U;
				size_t hash = 0;
				if constexpr (requires { value.count(); })
				{
					hash = std::hash<decltype(value.count())> {}(value.count());
				}
				else
				{
					hash = std::hash<Value> {}(value);
				}
				seed ^= hash + kHashMix + (seed << 6U) + (seed >> 2U);
			}

			size_t operator()(const PoolIdentity& identity) const noexcept
			{
				size_t seed = 0;
				append(seed, identity.host);
				append(seed, identity.port);
				append(seed, identity.tlsContext.get());
				append(seed, identity.sni);
				append(seed, identity.verifyPeer);
				append(seed, identity.mode);
				std::apply(
					[&seed](const auto&... values)
					{
						(append(seed, values), ...);
					},
					http2Fields(identity.http2));
				return seed;
			}
		};

		NetworkError poolCancelled()
		{
			return {ErrorCode::hOperationAborted, "HTTP 连接池请求已取消"};
		}

		Clock::time_point requestDeadline(Clock::duration timeout) noexcept
		{
			const auto now = Clock::now();
			return timeout > Clock::duration::zero() ? now + std::min(timeout, Clock::time_point::max() - now)
													 : Clock::time_point::max();
		}

		bool isTransientTransportError(ErrorCode code) noexcept
		{
			switch (code)
			{
				case ErrorCode::hEof:
				case ErrorCode::hConnectionReset:
				case ErrorCode::hConnectionRefused:
				case ErrorCode::hConnectionAborted:
				case ErrorCode::hNetworkUnreachable:
				case ErrorCode::hHostUnreachable:
				case ErrorCode::hBrokenPipe:
					return true;
				default:
					return false;
			}
		}

		bool canRetry(const HttpClientResult& result, HttpMethod method, bool reused, bool managed) noexcept
		{
			if (!isHttpMethodIdempotent(method) || !result.error)
			{
				return false;
			}
			const bool transient = isTransientTransportError(result.error.code);
			if (managed && result.requestDisposition == HttpRequestDisposition::hNotSent && transient)
			{
				// 建连尚未协商协议时 result.protocol 仍为默认 H1，未发送证据依然有效。
				return true;
			}
			if (result.protocol == HttpProtocol::hHttp1)
			{
				return reused && result.status == 0 && transient;
			}
			if (result.requestDisposition != HttpRequestDisposition::hNotProcessed || !result.error.http2)
			{
				return false;
			}
			const auto& raw = *result.error.http2;
			constexpr uint32_t kRefusedStream = 7;
			return (result.error.code == ErrorCode::hHttp2StreamError && raw.streamId > 0 && raw.code == kRefusedStream)
				   || (result.error.code == ErrorCode::hHttp2ConnectionError && raw.streamId == 0);
		}
	} // namespace

	struct HttpClientPool::OwnedRequest
	{
		std::string host;
		uint16_t port;
		HttpMethod method;
		std::string target;
		std::string body;
		HeaderMap headers;
		HttpClientOptions options;
		Clock::time_point deadline;

#ifdef HICAL_HAS_HTTP2
		std::optional<Clock::time_point> queueDeadline {};
#endif
	};

	struct HttpClientPool::Impl : std::enable_shared_from_this<Impl>
	{
		/// 一条空闲连接：本体 + 进队时刻（判断有没有超时靠它）
		struct IdleConn
		{
			std::shared_ptr<HttpClient> client;
			std::chrono::steady_clock::time_point idleSince;
		};

		struct Checkout
		{
			PoolIdentity identity;
			std::weak_ptr<HttpClient> instance;
			bool managed;
		};

#ifdef HICAL_HAS_HTTP2
		struct ManagedConnection
		{
			std::shared_ptr<HttpClient> client;
			size_t active {0};
			Clock::time_point idleSince {Clock::now()};
			std::optional<Clock::time_point> connectedAt;
		};

		struct ManagedWaiter;
		using ManagedWaiters = std::list<std::shared_ptr<ManagedWaiter>>;

		struct ManagedWaiter
		{
			ManagedWaiter(boost::asio::io_context& io, Clock::time_point deadline, Clock::duration timeout)
				: ready(io, deadline), queueDeadline(deadline), connectTimeout(timeout)
			{
			}

			boost::asio::steady_timer ready;
			Clock::time_point queueDeadline;
			Clock::time_point connectDeadline {Clock::time_point::max()};
			Clock::duration connectTimeout;
			std::shared_ptr<ManagedConnection> assigned;
			NetworkError error;
			ManagedWaiters::iterator position;
			bool queued {false};
			bool assignedConnected {false};
		};

		struct ManagedOrigin
		{
			ManagedOrigin(PoolIdentity key, HttpClientOptions opts) : identity(std::move(key)), options(std::move(opts))
			{
			}

			PoolIdentity identity;
			HttpClientOptions options;
			std::vector<std::shared_ptr<ManagedConnection>> connections;
			ManagedWaiters waiters;
			bool scheduled {false};
		};

		struct ManagedLease
		{
			ManagedLease() = default;
			ManagedLease(Impl& pool, std::shared_ptr<ManagedOrigin> bucket, std::shared_ptr<ManagedConnection> entry);
			~ManagedLease();
			ManagedLease(ManagedLease&& other) noexcept;
			ManagedLease(const ManagedLease&) = delete;
			ManagedLease& operator=(const ManagedLease&) = delete;
			ManagedLease& operator=(ManagedLease&&) = delete;

			Impl* owner {nullptr};
			std::shared_ptr<ManagedOrigin> origin;
			std::shared_ptr<ManagedConnection> connection;
			NetworkError error;
			bool reused {false};
		};

		std::shared_ptr<ManagedOrigin> managedOrigin(const OwnedRequest& request);
		std::shared_ptr<ManagedConnection> selectManaged(const std::shared_ptr<ManagedOrigin>& origin,
														 Clock::duration connectTimeout);
		void refreshConnectDeadline(const ManagedOrigin& origin, ManagedWaiter& waiter);
		void scheduleManaged(const std::shared_ptr<ManagedOrigin>& origin);
		void drainManaged(const std::shared_ptr<ManagedOrigin>& origin);
		void releaseManaged(const std::shared_ptr<ManagedOrigin>& origin,
							const std::shared_ptr<ManagedConnection>& connection);
		void removeWaiter(ManagedOrigin& origin, const std::shared_ptr<ManagedWaiter>& waiter);
		void requeueManagedGrant(const std::shared_ptr<ManagedOrigin>& origin,
								 const std::shared_ptr<ManagedWaiter>& waiter);
		void stopManaged();
		void sweepManaged();
		static Awaitable<ManagedLease> acquireManaged(std::shared_ptr<Impl> self,
													  std::shared_ptr<ManagedOrigin> origin,
													  Clock::time_point deadline,
													  Clock::duration connectTimeout,
													  std::optional<Clock::time_point>& queueDeadline);
#endif

		Impl(boost::asio::io_context& ctx, HttpClientPoolOptions opts) : ioCtx(ctx), options(std::move(opts))
		{
		}

		/// 扫一遍所有桶，把超时 / 已经死掉的空闲连接清出去
		void sweep()
		{
#ifdef HICAL_HAS_HTTP2
			sweepManaged();
#endif
			for (auto it = checkedOut.begin(); it != checkedOut.end();)
			{
				it = it->second.instance.expired() ? checkedOut.erase(it) : std::next(it);
			}
			auto now = std::chrono::steady_clock::now();
			for (auto it = buckets.begin(); it != buckets.end();)
			{
				auto& queue = it->second;
				// 队列按进队时间递增，front 最老；碰到还没到点的新连接就可以收手了
				while (!queue.empty())
				{
					const auto& front = queue.front();
					if (front.client->hasPendingRequests())
					{
						checkedOut.insert_or_assign(front.client.get(), Checkout {it->first, front.client, false});
						queue.pop_front();
						continue;
					}
					if (front.client->isAlive() && now - front.idleSince < options.idleTimeout)
					{
						break;
					}
					front.client->close();
					queue.pop_front();
				}
				if (queue.empty())
				{
					it = buckets.erase(it);
				}
				else
				{
					++it;
				}
			}
		}

		boost::asio::io_context& ioCtx;
		HttpClientPoolOptions options;

		/// 停机标志。清理协程每轮读它，析构 / shutdown() 置位
		std::atomic<bool> stopping {false};

		/// 清理 timer。协程体内建，池子存一份句柄好在析构时 cancel
		std::shared_ptr<boost::asio::steady_timer> timer;

		/// 分桶：bucketKey -> 空闲连接队列
		std::unordered_map<PoolIdentity, std::deque<IdleConn>, IdentityHash> buckets;

		/// 地址只用于索引；完整身份与 weak 实例共同核验归还，过期租借由 sweep 清理。
		std::unordered_map<const HttpClient*, Checkout> checkedOut;
#ifdef HICAL_HAS_HTTP2
		std::unordered_map<PoolIdentity, std::shared_ptr<ManagedOrigin>, IdentityHash> managed;
		size_t queuedRequests {0};
		size_t peakQueuedRequests {0};
#endif
	};

#ifdef HICAL_HAS_HTTP2
	HttpClientPool::Impl::ManagedLease::ManagedLease(Impl& pool,
													 std::shared_ptr<ManagedOrigin> bucket,
													 std::shared_ptr<ManagedConnection> entry)
		: owner(&pool), origin(std::move(bucket)), connection(std::move(entry)), reused(connection->client->isAlive())
	{
	}

	HttpClientPool::Impl::ManagedLease::~ManagedLease()
	{
		if (owner)
		{
			owner->releaseManaged(origin, connection);
		}
	}

	HttpClientPool::Impl::ManagedLease::ManagedLease(ManagedLease&& other) noexcept
		: owner(std::exchange(other.owner, nullptr))
		, origin(std::move(other.origin))
		, connection(std::move(other.connection))
		, error(std::move(other.error))
		, reused(other.reused)
	{
	}

	std::shared_ptr<HttpClientPool::Impl::ManagedOrigin> HttpClientPool::Impl::managedOrigin(
		const OwnedRequest& request)
	{
		PoolIdentity key(request.host, request.port, request.options);
		auto found = managed.find(key);
		if (found != managed.end())
		{
			return found->second;
		}
		auto origin = std::make_shared<ManagedOrigin>(key, request.options);
		managed.emplace(std::move(key), origin);
		return origin;
	}

	std::shared_ptr<HttpClientPool::Impl::ManagedConnection> HttpClientPool::Impl::selectManaged(
		const std::shared_ptr<ManagedOrigin>& origin,
		Clock::duration connectTimeout)
	{
		bool starting = false;
		for (auto position = origin->connections.begin(); position != origin->connections.end();)
		{
			const auto& entry = *position;
			const auto capacity = entry->client->poolCapacity();
			if (!capacity.connected)
			{
				if (entry->active == 0 && !capacity.connecting)
				{
					entry->client->setPoolCapacityChanged({});
					position = origin->connections.erase(position);
					continue;
				}
				starting = true;
			}
			else if (capacity.protocol == HttpProtocol::hHttp1)
			{
				if (entry->active == 0)
				{
					return entry;
				}
			}
			else
			{
				starting = starting || !capacity.settingsSeen;
				// active 也包含已交给 HttpClient、尚未由会话计入 admitted 的预留请求。
				const auto reserved = entry->active > capacity.admitted ? entry->active - capacity.admitted : 0;
				if (capacity.accepting && capacity.available > reserved)
				{
					return entry;
				}
			}
			++position;
		}
		if (starting || origin->connections.size() >= origin->options.http2.maxConnectionsPerOrigin)
		{
			return {};
		}
		auto entry = std::make_shared<ManagedConnection>();
		auto options = origin->options;
		options.connectTimeout = connectTimeout;
		entry->client =
			std::make_shared<HttpClient>(ioCtx, origin->identity.host, origin->identity.port, std::move(options));
		std::weak_ptr<Impl> weak = weak_from_this();
		std::weak_ptr<ManagedOrigin> weakOrigin = origin;
		std::weak_ptr<ManagedConnection> weakEntry = entry;
		entry->client->setPoolCapacityChanged(
			[weak, weakOrigin, weakEntry]
			{
				// 记录连接就绪的时刻，再 post 调度；回调延迟不能改变建连是否超时的判断。
				if (auto connection = weakEntry.lock())
				{
					if (!connection->client->poolCapacity().connected)
					{
						connection->connectedAt.reset();
					}
					else if (!connection->connectedAt)
					{
						connection->connectedAt = Clock::now();
					}
				}
				if (auto owner = weak.lock())
				{
					if (auto bucket = weakOrigin.lock())
					{
						owner->scheduleManaged(bucket);
					}
				}
			});
		origin->connections.push_back(entry);
		return entry;
	}

	void HttpClientPool::Impl::refreshConnectDeadline(const ManagedOrigin& origin, ManagedWaiter& waiter)
	{
		for (const auto& entry : origin.connections)
		{
			if (!entry->client->poolCapacity().connected || !entry->connectedAt)
			{
				continue;
			}
			if (waiter.connectDeadline == Clock::time_point::max())
			{
				return;
			}
			if (*entry->connectedAt > waiter.connectDeadline)
			{
				waiter.error = {ErrorCode::hTimedOut, "HTTP/2 连接池建连等待子期限已过"};
			}
			else
			{
				waiter.connectDeadline = Clock::time_point::max();
				waiter.ready.expires_at(waiter.queueDeadline);
			}
			return;
		}
		if (waiter.connectDeadline == Clock::time_point::max() && waiter.connectTimeout > Clock::duration::zero())
		{
			// 原本只等流槽位的调用遇到断连后，也要启动自己的建连预算；后续通知不能续期。
			waiter.connectDeadline = requestDeadline(waiter.connectTimeout);
			waiter.ready.expires_at(std::min(waiter.queueDeadline, waiter.connectDeadline));
		}
	}

	void HttpClientPool::Impl::scheduleManaged(const std::shared_ptr<ManagedOrigin>& origin)
	{
		if (origin->scheduled || stopping.load(std::memory_order_acquire))
		{
			return;
		}
		origin->scheduled = true;
		std::weak_ptr<Impl> weak = weak_from_this();
		std::weak_ptr<ManagedOrigin> weakOrigin = origin;
		boost::asio::post(ioCtx,
						  [weak, weakOrigin]
						  {
							  auto owner = weak.lock();
							  auto bucket = weakOrigin.lock();
							  if (owner && bucket)
							  {
								  bucket->scheduled = false;
								  owner->drainManaged(bucket);
							  }
						  });
	}

	void HttpClientPool::Impl::removeWaiter(ManagedOrigin& origin, const std::shared_ptr<ManagedWaiter>& waiter)
	{
		if (waiter->queued)
		{
			origin.waiters.erase(waiter->position);
			waiter->queued = false;
			--queuedRequests;
		}
	}

	void HttpClientPool::Impl::drainManaged(const std::shared_ptr<ManagedOrigin>& origin)
	{
		for (const auto& waiter : origin->waiters)
		{
			refreshConnectDeadline(*origin, *waiter);
		}
		while (!origin->waiters.empty())
		{
			auto waiter = origin->waiters.front();
			if (stopping.load(std::memory_order_acquire))
			{
				waiter->error = poolCancelled();
			}
			else if (!waiter->error && Clock::now() >= waiter->ready.expiry())
			{
				waiter->error = {ErrorCode::hTimedOut, "HTTP/2 连接池排队期限已过"};
			}
			else if (!waiter->error)
			{
				auto entry = selectManaged(origin, waiter->connectTimeout);
				if (!entry)
				{
					break;
				}
				++entry->active;
				waiter->assignedConnected = entry->client->poolCapacity().connected;
				waiter->assigned = std::move(entry);
			}
			removeWaiter(*origin, waiter);
			waiter->ready.cancel();
		}
	}

	void HttpClientPool::Impl::requeueManagedGrant(const std::shared_ptr<ManagedOrigin>& origin,
												   const std::shared_ptr<ManagedWaiter>& waiter)
	{
		// 旧连接上的预留请求须重新参与调度，不能沿用首个调用的预算自行重连。
		auto previous = std::exchange(waiter->assigned, {});
		releaseManaged(origin, previous);
		waiter->assignedConnected = false;
		if (origin->waiters.size() >= origin->options.http2.maxQueuedRequestsPerOrigin)
		{
			waiter->error = {ErrorCode::hResourceLimitExceeded, "HTTP/2 连接池有限等待队列已满"};
			return;
		}
		if (waiter->connectDeadline == Clock::time_point::max())
		{
			waiter->connectDeadline = requestDeadline(waiter->connectTimeout);
		}
		waiter->ready.expires_at(std::min(waiter->queueDeadline, waiter->connectDeadline));
		origin->waiters.push_back(waiter);
		waiter->position = std::prev(origin->waiters.end());
		waiter->queued = true;
		peakQueuedRequests = std::max(peakQueuedRequests, ++queuedRequests);
	}

	void HttpClientPool::Impl::releaseManaged(const std::shared_ptr<ManagedOrigin>& origin,
											  const std::shared_ptr<ManagedConnection>& connection)
	{
		--connection->active;
		if (connection->active == 0)
		{
			connection->idleSince = Clock::now();
			const auto capacity = connection->client->poolCapacity();
			if (capacity.connected && capacity.protocol == HttpProtocol::hHttp2 && capacity.settingsSeen
				&& !capacity.accepting)
			{
				connection->client->close();
			}
		}
		scheduleManaged(origin);
	}

	Awaitable<HttpClientPool::Impl::ManagedLease> HttpClientPool::Impl::acquireManaged(
		std::shared_ptr<Impl> self,
		std::shared_ptr<ManagedOrigin> origin,
		Clock::time_point deadline,
		Clock::duration connectTimeout,
		std::optional<Clock::time_point>& queueDeadline)
	{
		auto cancellation = co_await boost::asio::this_coro::cancellation_state;
		ManagedLease rejected;
		if (self->stopping.load(std::memory_order_acquire))
		{
			rejected.error = poolCancelled();
			co_return rejected;
		}
		if (Clock::now() >= deadline)
		{
			rejected.error = {ErrorCode::hTimedOut, "HTTP/2 连接池请求期限已过"};
			co_return rejected;
		}
		self->drainManaged(origin);
		if (origin->waiters.empty())
		{
			if (auto entry = self->selectManaged(origin, connectTimeout))
			{
				++entry->active;
				co_return ManagedLease(*self, std::move(origin), std::move(entry));
			}
		}
		if (origin->waiters.size() >= origin->options.http2.maxQueuedRequestsPerOrigin)
		{
			rejected.error = {ErrorCode::hResourceLimitExceeded, "HTTP/2 连接池有限等待队列已满"};
			co_return rejected;
		}
		if (!queueDeadline)
		{
			// 只在首次实际排队时开始计时；后续重试不能重获完整的队列预算。
			queueDeadline = requestDeadline(origin->options.http2.queueTimeout);
		}
		auto waiter = std::make_shared<ManagedWaiter>(self->ioCtx, std::min(deadline, *queueDeadline), connectTimeout);
		const bool connected = std::any_of(origin->connections.begin(),
										   origin->connections.end(),
										   [](const auto& entry)
										   {
											   return entry->client->poolCapacity().connected;
										   });
		if (!connected && connectTimeout > Clock::duration::zero())
		{
			waiter->connectDeadline = requestDeadline(connectTimeout);
			waiter->ready.expires_at(std::min(waiter->queueDeadline, waiter->connectDeadline));
		}
		origin->waiters.push_back(waiter);
		waiter->position = std::prev(origin->waiters.end());
		waiter->queued = true;
		self->peakQueuedRequests = std::max(self->peakQueuedRequests, ++self->queuedRequests);

		struct Registration
		{
			Impl& owner;
			std::shared_ptr<ManagedOrigin>& origin;
			std::shared_ptr<ManagedWaiter>& waiter;

			~Registration()
			{
				owner.removeWaiter(*origin, waiter);
				if (waiter->assigned)
				{
					owner.releaseManaged(origin, waiter->assigned);
				}
				owner.scheduleManaged(origin);
			}
		} registration {*self, origin, waiter};

		boost::system::error_code error;
		for (;;)
		{
			co_await waiter->ready.async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, error));
			self->refreshConnectDeadline(*origin, *waiter);
			if (self->stopping.load(std::memory_order_acquire)
				|| cancellation.cancelled() != boost::asio::cancellation_type::none || waiter->error
				|| Clock::now() >= waiter->ready.expiry())
			{
				break;
			}
			if (waiter->assigned)
			{
				if (waiter->assignedConnected && !waiter->assigned->client->poolCapacity().connected)
				{
					self->requeueManagedGrant(origin, waiter);
					if (!waiter->error)
					{
						continue;
					}
				}
				break;
			}
			if (error != boost::asio::error::operation_aborted)
			{
				break;
			}
			// 连接已按时就绪，不再检查建连期限；请求总期限和排队期限仍然有效。
		}
		// 授予时已摘队的 waiter 也必须重查停机；其预留仍由 Registration 归还。
		if (self->stopping.load(std::memory_order_acquire)
			|| cancellation.cancelled() != boost::asio::cancellation_type::none)
		{
			rejected.error = poolCancelled();
		}
		else if (waiter->error)
		{
			rejected.error = waiter->error;
		}
		else if (Clock::now() >= waiter->ready.expiry())
		{
			rejected.error = {ErrorCode::hTimedOut, "HTTP/2 连接池排队期限已过"};
		}
		else if (waiter->assigned)
		{
			co_return ManagedLease(*self, origin, std::exchange(waiter->assigned, {}));
		}
		else
		{
			rejected.error = error ? toNetworkError(error) : poolCancelled();
		}
		co_return rejected;
	}

	void HttpClientPool::Impl::stopManaged()
	{
		for (auto& [key, origin] : managed)
		{
			for (const auto& entry : origin->connections)
			{
				entry->client->close();
			}
			drainManaged(origin);
		}
	}

	void HttpClientPool::Impl::sweepManaged()
	{
		const auto now = Clock::now();
		for (auto position = managed.begin(); position != managed.end();)
		{
			auto& origin = position->second;
			std::erase_if(origin->connections,
						  [&](const auto& entry)
						  {
							  if (entry->active != 0 || !origin->waiters.empty())
							  {
								  return false;
							  }
							  if (entry->client->isAlive() && now - entry->idleSince < options.idleTimeout)
							  {
								  return false;
							  }
							  entry->client->setPoolCapacityChanged({});
							  entry->client->close();
							  return true;
						  });
			if (origin->connections.empty() && origin->waiters.empty())
			{
				position = managed.erase(position);
			}
			else
			{
				++position;
			}
		}
	}
#endif

	HttpClientPool::HttpClientPool(boost::asio::io_context& ioCtx, HttpClientPoolOptions options)
		: impl_(std::make_shared<Impl>(ioCtx, std::move(options)))
	{
		if (impl_->options.cleanupInterval <= std::chrono::steady_clock::duration::zero())
		{
			throw std::invalid_argument("HttpClientPool: cleanupInterval must be > 0");
		}
		if (impl_->options.idleTimeout <= std::chrono::steady_clock::duration::zero())
		{
			throw std::invalid_argument("HttpClientPool: idleTimeout must be > 0");
		}

		coSpawn(ioCtx, cleanupLoop(impl_));
	}

	HttpClientPool::~HttpClientPool()
	{
		if (!impl_)
		{
			return;
		}

		// 析构可能来自其他线程，取消 timer 和关闭连接须 post 回所属 io_context。
		// 清理协程持有 Impl，退出后才释放最后一份引用。
		scheduleStop();
		impl_.reset();
	}

	void HttpClientPool::scheduleStop()
	{
		auto impl = impl_;
		impl->stopping.store(true, std::memory_order_release);

		boost::asio::post(impl->ioCtx,
						  [impl]()
						  {
#ifdef HICAL_HAS_HTTP2
							  impl->stopManaged();
#endif
							  // 关掉空闲连接让对端立刻看到关闭，不用干等空闲超时
							  for (auto& [key, queue] : impl->buckets)
							  {
								  for (auto& conn : queue)
								  {
									  if (!conn.client->hasPendingRequests())
									  {
										  conn.client->close();
									  }
								  }
							  }
							  impl->buckets.clear();
							  for (const auto& [pointer, checkout] : impl->checkedOut)
							  {
								  if (checkout.managed)
								  {
									  if (auto client = checkout.instance.lock())
									  {
										  client->close();
									  }
								  }
							  }
							  impl->checkedOut.clear();

							  if (impl->timer)
							  {
								  impl->timer->cancel();
							  }
						  });
	}

	void HttpClientPool::shutdown()
	{
		if (!impl_)
		{
			return;
		}
		scheduleStop();
	}

	HttpClientPool::Lease HttpClientPool::acquireFrom(Impl& impl,
													  const std::string& host,
													  uint16_t port,
													  const HttpClientOptions& options,
													  bool managed)
	{
		if (impl.stopping.load(std::memory_order_acquire))
		{
			return {};
		}
		PoolIdentity key(host, port, options);

		auto bucketIt = impl.buckets.find(key);
		if (bucketIt != impl.buckets.end())
		{
			auto& queue = bucketIt->second;
			while (!queue.empty())
			{
				auto conn = std::move(queue.front());
				queue.pop_front();

				if (conn.client->hasPendingRequests())
				{
					impl.checkedOut.insert_or_assign(conn.client.get(), Impl::Checkout {key, conn.client, false});
					continue;
				}
				if (conn.client->isAlive())
				{
					impl.checkedOut.insert_or_assign(conn.client.get(), Impl::Checkout {key, conn.client, managed});
					if (queue.empty())
					{
						impl.buckets.erase(bucketIt);
					}
					return Lease {std::move(conn.client), true};
				}
				// 死连接直接丢，接着看下一条
			}
			// 队列清空还留了个空桶，抹掉
			impl.buckets.erase(bucketIt);
		}

		auto client = std::make_shared<HttpClient>(impl.ioCtx, host, port, options);
		impl.checkedOut.insert_or_assign(client.get(), Impl::Checkout {std::move(key), client, managed});
		return Lease {std::move(client), false};
	}

	std::shared_ptr<HttpClient> HttpClientPool::acquire(const std::string& host,
														uint16_t port,
														const HttpClientOptions& options)
	{
		return acquireFrom(*impl_, host, port, options).client;
	}

	void HttpClientPool::releaseTo(Impl& impl, std::shared_ptr<HttpClient> client)
	{
		if (!client)
		{
			return;
		}

		auto tracked = impl.checkedOut.find(client.get());
		if (tracked == impl.checkedOut.end())
		{
			return;
		}
		if (tracked->second.instance.lock() != client)
		{
			impl.checkedOut.erase(tracked);
			return;
		}
		// ticket 覆盖尚未启动的 awaitable；保留租借记录，业务完成后仍可显式归还。
		if (client->hasPendingRequests())
		{
			return;
		}
		auto key = std::move(tracked->second.identity);
		impl.checkedOut.erase(tracked);

		// 坏连接 / 正在停机：一律不回收，让它随最后一个 shared_ptr 释放
		if (!client->isAlive() || impl.stopping.load(std::memory_order_relaxed))
		{
			return;
		}

		auto& queue = impl.buckets[key];
		queue.push_back(Impl::IdleConn {std::move(client), std::chrono::steady_clock::now()});

		// 超上限就把最老的挤出去
		while (queue.size() > impl.options.maxIdlePerBucket)
		{
			if (!queue.front().client->hasPendingRequests())
			{
				queue.front().client->close();
			}
			queue.pop_front();
		}
	}

	void HttpClientPool::release(std::shared_ptr<HttpClient> client)
	{
		if (!impl_)
		{
			return;
		}
		releaseTo(*impl_, std::move(client));
	}

	Awaitable<HttpClientResult> HttpClientPool::doRequest(std::shared_ptr<Impl> impl, OwnedRequest request)
	{
#ifdef HICAL_HAS_HTTP2
		if (request.options.protocolMode == HttpProtocolMode::hHttp2Only
			|| request.options.protocolMode == HttpProtocolMode::hNegotiateHttp2)
		{
			co_return co_await doManagedRequest(std::move(impl), std::move(request));
		}
#endif
		HttpClientResult result;
		result.requestDisposition = HttpRequestDisposition::hNotSent;
		Lease lease;
		try
		{
			auto cancellation = co_await boost::asio::this_coro::cancellation_state;
			for (unsigned int attempt = 0; attempt < 2; ++attempt)
			{
				result = {};
				result.requestDisposition = HttpRequestDisposition::hNotSent;
				if (attempt != 0)
				{
					// 旧连接已归还，先处理所属 loop 中已排队的取消或停机，再尝试发送。
					co_await boost::asio::post(boost::asio::use_awaitable);
				}
				if (impl->stopping.load(std::memory_order_acquire)
					|| cancellation.cancelled() != boost::asio::cancellation_type::none)
				{
					result.error = poolCancelled();
					break;
				}
				if (Clock::now() >= request.deadline)
				{
					result.error = {ErrorCode::hTimedOut, "HTTP 连接池请求期限已过"};
					break;
				}
				lease = acquireFrom(*impl, request.host, request.port, request.options, true);
				if (!lease.client)
				{
					result.error = poolCancelled();
					break;
				}
				result = co_await lease.client->requestUntil(request.method,
															 request.target,
															 request.body,
															 request.headers,
															 request.deadline);
				if (impl->stopping.load(std::memory_order_acquire))
				{
					result.error = poolCancelled();
					break;
				}
				if (attempt != 0 || !canRetry(result, request.method, lease.reused, false))
				{
					break;
				}
				releaseTo(*impl, std::move(lease.client));
			}
		}
		catch (const boost::system::system_error& exception)
		{
			result.error = exception.code() == boost::asio::error::operation_aborted ? poolCancelled()
																					 : toNetworkError(exception.code());
		}
		releaseTo(*impl, std::move(lease.client));
		co_return result;
	}

#ifdef HICAL_HAS_HTTP2
	Awaitable<HttpClientResult> HttpClientPool::doManagedRequest(std::shared_ptr<Impl> impl, OwnedRequest request)
	{
		HttpClientResult result;
		result.requestDisposition = HttpRequestDisposition::hNotSent;
		if (!request.options.tlsContext)
		{
			result.error = {ErrorCode::hInvalidConfiguration, "HTTP/2 客户端必须配置 TLS 上下文"};
			co_return result;
		}
		if (const auto error = request.options.http2.validationError(); !error.empty())
		{
			result.error = {ErrorCode::hInvalidConfiguration, std::string(error)};
			co_return result;
		}
		try
		{
			auto cancellation = co_await boost::asio::this_coro::cancellation_state;
			auto origin = impl->managedOrigin(request);
			for (unsigned int attempt = 0; attempt < 2; ++attempt)
			{
				result = {};
				result.requestDisposition = HttpRequestDisposition::hNotSent;
				if (attempt != 0)
				{
					co_await boost::asio::post(boost::asio::use_awaitable);
				}
				if (impl->stopping.load(std::memory_order_acquire)
					|| cancellation.cancelled() != boost::asio::cancellation_type::none)
				{
					result.error = poolCancelled();
					co_return result;
				}
				if (Clock::now() >= request.deadline)
				{
					result.error = {ErrorCode::hTimedOut, "HTTP 连接池请求期限已过"};
					co_return result;
				}
				auto lease = co_await Impl::acquireManaged(impl,
														   origin,
														   request.deadline,
														   request.options.connectTimeout,
														   request.queueDeadline);
				if (lease.error)
				{
					result.error = std::move(lease.error);
					co_return result;
				}
				result = co_await lease.connection->client->requestUntil(request.method,
																		 request.target,
																		 request.body,
																		 request.headers,
																		 request.deadline);
				if (impl->stopping.load(std::memory_order_acquire))
				{
					result.error = poolCancelled();
					co_return result;
				}
				if (attempt != 0 || !canRetry(result, request.method, lease.reused, true))
				{
					co_return result;
				}
			}
		}
		catch (const boost::system::system_error& exception)
		{
			result.error = exception.code() == boost::asio::error::operation_aborted ? poolCancelled()
																					 : toNetworkError(exception.code());
		}
		co_return result;
	}
#endif

	Awaitable<HttpClientResult> HttpClientPool::request(const std::string& host,
														uint16_t port,
														HttpMethod method,
														std::string_view target,
														std::string_view body,
														const HeaderMap& headers,
														const HttpClientOptions& options)
	{
		auto impl = impl_;
		const auto deadline = requestDeadline(options.requestTimeout);
		OwnedRequest owned {host, port, method, std::string(target), std::string(body), headers, options, deadline};
		return doRequest(std::move(impl), std::move(owned));
	}

	Awaitable<HttpClientResult> HttpClientPool::get(const std::string& host,
													uint16_t port,
													std::string_view target,
													const HeaderMap& headers,
													const HttpClientOptions& options)
	{
		return request(host, port, HttpMethod::hGet, target, {}, headers, options);
	}

	Awaitable<HttpClientResult> HttpClientPool::post(const std::string& host,
													 uint16_t port,
													 std::string_view target,
													 std::string_view body,
													 std::string_view contentType,
													 const HttpClientOptions& options)
	{
		HeaderMap headers;
		headers.set("Content-Type", contentType);
		return request(host, port, HttpMethod::hPost, target, body, headers, options);
	}

	Awaitable<void> HttpClientPool::cleanupLoop(std::shared_ptr<Impl> impl)
	{
		auto executor = co_await boost::asio::this_coro::executor;
		auto timer = std::make_shared<boost::asio::steady_timer>(executor);
		impl->timer = timer;

		while (!impl->stopping.load(std::memory_order_relaxed))
		{
			timer->expires_after(impl->options.cleanupInterval);
			boost::system::error_code ec;
			co_await timer->async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, ec));

			// cancel 会让 async_wait 带 operation_aborted 返回；stopping 是双保险
			if (ec || impl->stopping.load(std::memory_order_relaxed))
			{
				break;
			}
			impl->sweep();
		}

		// 收尾：放掉 timer 引用。池子析构侧可能已经 cancel 过它，这里只是释放
		impl->timer.reset();
	}

	std::size_t HttpClientPool::idleCount() const noexcept
	{
		if (!impl_)
		{
			return 0;
		}
		std::size_t total = 0;
		for (const auto& [key, queue] : impl_->buckets)
		{
			total += queue.size();
		}
		return total;
	}

	HttpClientPool::Stats HttpClientPool::stats() const noexcept
	{
		Stats result;
#ifdef HICAL_HAS_HTTP2
		if (impl_)
		{
			result.queuedRequests = impl_->queuedRequests;
			result.peakQueuedRequests = impl_->peakQueuedRequests;
			for (const auto& [key, origin] : impl_->managed)
			{
				result.managedConnections += origin->connections.size();
				for (const auto& entry : origin->connections)
				{
					const auto capacity = entry->client->poolCapacity();
					result.activeRequests += entry->active;
					result.connectingConnections += capacity.connecting || (!capacity.connected && entry->active != 0);
				}
			}
		}
#endif
		return result;
	}

} // namespace hical
