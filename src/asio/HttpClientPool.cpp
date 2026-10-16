/**
 * @file HttpClientPool.cpp
 * @brief 连接池实现：分桶复用、空闲清理、生命周期托管
 * 分桶键带上 TLS 标记，明文和 HTTPS 到同一个 host:port 不算一个桶。
 * 清理协程自己持有一份 Impl 的引用，池子析构后它还能把最后一轮跑完再释放，
 * 避免「析构了但协程下次醒来还在读 this」的悬空。
 */

#include "HttpClientPool.h"
#include <boost/asio/post.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <atomic>
#include <deque>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace hical
{

	struct HttpClientPool::Impl
	{
		/// 一条空闲连接：本体 + 进队时刻（判断有没有超时靠它）
		struct IdleConn
		{
			std::shared_ptr<HttpClient> client;
			std::chrono::steady_clock::time_point idleSince;
		};

		Impl(boost::asio::io_context& ctx, HttpClientPoolOptions opts) : ioCtx(ctx), options(std::move(opts))
		{
		}

		/// 扫一遍所有桶，把超时 / 已经死掉的空闲连接清出去
		void sweep()
		{
			auto now = std::chrono::steady_clock::now();
			for (auto it = buckets.begin(); it != buckets.end();)
			{
				auto& queue = it->second;
				// 队列按进队时间递增，front 最老；碰到还没到点的新连接就可以收手了
				while (!queue.empty())
				{
					const auto& front = queue.front();
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
		std::unordered_map<std::string, std::deque<IdleConn>> buckets;

		/// 借出去的连接 -> 它属于哪个桶。release 时按裸指针找回桶键（含 TLS 标记）
		std::unordered_map<const HttpClient*, std::string> checkedOut;
	};

	std::string HttpClientPool::makeBucketKey(std::string_view host, uint16_t port, bool tls)
	{
		std::string key;
		key.reserve(host.size() + 8);
		key.append(host);
		key.push_back(':');
		key.append(std::to_string(port));
		if (tls)
		{
			key.append(":tls");
		}
		return key;
	}

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

		// 清理动作全 post 到 io_context 线程上做——timer cancel、关连接都不是
		// 线程安全的，析构可能从别的线程进来。清理协程自己持有一份 Impl 的引用，
		// 所以这里丢掉池子的引用是安全的：等协程收到取消、退出之后 Impl 才真正释放。
		scheduleStop();
		impl_.reset();
	}

	void HttpClientPool::scheduleStop()
	{
		auto impl = impl_;
		impl->stopping.store(true, std::memory_order_relaxed);

		boost::asio::post(impl->ioCtx,
						  [impl]()
						  {
							  // 关掉空闲连接让对端立刻看到关闭，不用干等空闲超时
							  for (auto& [key, queue] : impl->buckets)
							  {
								  for (auto& conn : queue)
								  {
									  conn.client->close();
								  }
							  }
							  impl->buckets.clear();
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
													  const HttpClientOptions& options)
	{
		std::string key = makeBucketKey(host, port, options.tlsContext != nullptr);

		auto bucketIt = impl.buckets.find(key);
		if (bucketIt != impl.buckets.end())
		{
			auto& queue = bucketIt->second;
			while (!queue.empty())
			{
				auto conn = std::move(queue.front());
				queue.pop_front();

				if (conn.client->isAlive())
				{
					impl.checkedOut[conn.client.get()] = key;
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
		impl.checkedOut[client.get()] = key;
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

		// 借出去时记了桶键，这里按裸指针取回来
		std::string key;
		auto tracked = impl.checkedOut.find(client.get());
		if (tracked != impl.checkedOut.end())
		{
			key = std::move(tracked->second);
			impl.checkedOut.erase(tracked);
		}
		else
		{
			// 不是本池 acquire 出来的，退化成 host:port（丢了 TLS 标记，正常路径不会走到这）
			key = std::string(client->key());
		}

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
			queue.front().client->close();
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

	Awaitable<HttpClientResult> HttpClientPool::doRequest(std::shared_ptr<Impl> impl,
														  const std::string& host,
														  uint16_t port,
														  HttpMethod method,
														  std::string_view target,
														  std::string_view body,
														  const HeaderMap& headers,
														  const HttpClientOptions& options)
	{
		auto lease = acquireFrom(*impl, host, port, options);
		auto result = co_await lease.client->request(method, target, body, headers);

		// 复用的连接可能早被对端悄悄关了：这时候写入 / 读出会失败，但 status 还是 0
		// （一个字节响应都没收到）。这种大多是陈旧 keep-alive 连接——请求还没到对端连接
		// 就没了，丢掉重开一条重试一次即可，别让调用方为它买单。status != 0 说明已经聊到
		// 一半了，不重试。注意这等于对非幂等方法给的是「至少一次」语义：极端情况下（请求
		// 到了对端、对端处理完但回响应前连接断），可能重发一次，需要严格一次的场景别依赖它。
		if (lease.reused && result.isNetworkError() && result.status == 0
			&& !impl->stopping.load(std::memory_order_relaxed))
		{
			releaseTo(*impl, std::move(lease.client));
			lease = acquireFrom(*impl, host, port, options);
			result = co_await lease.client->request(method, target, body, headers);
		}

		releaseTo(*impl, std::move(lease.client));
		co_return result;
	}

	Awaitable<HttpClientResult> HttpClientPool::request(const std::string& host,
														uint16_t port,
														HttpMethod method,
														std::string_view target,
														std::string_view body,
														const HeaderMap& headers,
														const HttpClientOptions& options)
	{
		// 按值抓住 Impl，协程帧里就不会存裸 this——池子先析构也不至于悬空
		auto impl = impl_;
		co_return co_await doRequest(impl, host, port, method, target, body, headers, options);
	}

	Awaitable<HttpClientResult> HttpClientPool::get(const std::string& host,
													uint16_t port,
													std::string_view target,
													const HeaderMap& headers,
													const HttpClientOptions& options)
	{
		auto impl = impl_;
		co_return co_await doRequest(impl, host, port, HttpMethod::hGet, target, {}, headers, options);
	}

	Awaitable<HttpClientResult> HttpClientPool::post(const std::string& host,
													 uint16_t port,
													 std::string_view target,
													 std::string_view body,
													 std::string_view contentType,
													 const HttpClientOptions& options)
	{
		auto impl = impl_;
		// 局部头表活在协程帧里，跨 co_await 安全
		HeaderMap headers;
		headers.set("Content-Type", contentType);
		co_return co_await doRequest(impl, host, port, HttpMethod::hPost, target, body, headers, options);
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

} // namespace hical
