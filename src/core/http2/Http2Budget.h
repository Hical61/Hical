/**
 * @file Http2Budget.h
 * @brief HTTP/2 协议流、应用任务和 payload 的有限额度
 */

#pragma once

#ifdef HICAL_HAS_HTTP2

	#include "core/Http2Options.h"
	#include <array>
	#include <atomic>
	#include <cassert>
	#include <limits>
	#include <memory>
	#include <utility>

namespace hical::detail
{
	/**
	 * @brief 多个连接和工作线程共享的应用任务额度，各连接使用同一实例。
	 * 原子只保护计数，不发布任务数据，因此使用 relaxed 内存序。
	 */
	class Http2ServiceBudget
	{
		struct State
		{
			explicit State(size_t maximum) : limit(maximum)
			{
			}

			bool acquire() noexcept
			{
				auto current = used.load(std::memory_order_relaxed);
				while (current < limit)
				{
					if (used.compare_exchange_weak(current, current + 1, std::memory_order_relaxed))
					{
						return true;
					}
				}
				return false;
			}

			void release() noexcept
			{
				[[maybe_unused]] auto previous = used.fetch_sub(1, std::memory_order_relaxed);
				assert(previous != 0);
			}

			const size_t limit;
			std::atomic<size_t> used {0};
		};

	public:
		/**
		 * @brief 设置服务级任务上限；上限为零时抛出 std::invalid_argument。
		 * @param maxTasks 整个服务仍在运行的应用任务上限
		 */
		explicit Http2ServiceBudget(size_t maxTasks)
		{
			if (maxTasks == 0)
			{
				throw std::invalid_argument("HTTP/2 服务应用任务上限必须为正");
			}
			state_ = std::make_shared<State>(maxTasks);
		}

		/**
		 * @brief 查询当前占用。
		 * @return 尚未归还的应用任务数；可从任意线程查询。
		 */
		[[nodiscard]] size_t used() const noexcept
		{
			return state_->used.load(std::memory_order_relaxed);
		}

		/**
		 * @brief 查询服务容量。
		 * @return 构造时指定的应用任务上限。
		 */
		[[nodiscard]] size_t limit() const noexcept
		{
			return state_->limit;
		}

	private:
		friend class Http2Budget;
		std::shared_ptr<State> state_;
	};

	/**
	 * @brief 连接内预算与其租约均由所属 loop 串行访问，single-threaded, no lock needed。
	 * 仅启用 HTTP/2 的连接创建本对象。额度计算保留的 payload 字节，不包含 string 容量、
	 * 分配器元数据或 handler 自行分配的内存，因此不表示进程 RSS 的硬上限。
	 */
	class Http2Budget
	{
		enum class Resource : size_t
		{
			hProtocolStream,
			hApplicationTask,
			hReceiveBytes,
			hSendBytes,
			hCount
		};

		struct State
		{
			std::array<size_t, static_cast<size_t>(Resource::hCount)> used {};
			std::array<size_t, static_cast<size_t>(Resource::hCount)> limits {};
			size_t streamReceiveLimit {0};
			std::shared_ptr<Http2ServiceBudget::State> service;
		};

	public:
		enum class Role : uint8_t
		{
			hClient,
			hServer
		};

		struct Usage
		{
			size_t protocolStreams;
			size_t applicationTasks;
			size_t receiveBytes;
			size_t sendBytes;
		};

		/**
		 * @brief 独占一份额度；移动转移所有权，reset/析构至多归还一次。
		 * 持有共享 State，连接或服务对象先销毁也能归还额度；仍须在连接所属 loop 操作。
		 */
		class Lease
		{
		public:
			Lease() = default;

			~Lease()
			{
				reset();
			}

			Lease(const Lease&) = delete;
			Lease& operator=(const Lease&) = delete;

			/**
			 * @brief 接管另一租约。
			 * @param other 被清空的源租约
			 */
			Lease(Lease&& other) noexcept
				: state_(std::move(other.state_))
				, resource_(other.resource_)
				, amount_(std::exchange(other.amount_, 0))
				, limit_(other.limit_)
			{
			}

			/**
			 * @brief 归还自身原额度后接管另一租约。
			 * @param other 被清空的源租约；允许自身移动
			 * @return 当前租约。
			 */
			Lease& operator=(Lease&& other) noexcept
			{
				if (this != &other)
				{
					reset();
					state_ = std::move(other.state_);
					resource_ = other.resource_;
					amount_ = std::exchange(other.amount_, 0);
					limit_ = other.limit_;
				}
				return *this;
			}

			/**
			 * @brief 查询是否持有有效租约；零字节 payload 租约仍有效。
			 * @return 是否可继续使用或归还。
			 */
			[[nodiscard]] explicit operator bool() const noexcept
			{
				return static_cast<bool>(state_);
			}

			/**
			 * @brief 查询本租约占用的额度。
			 * @return payload 字节数或一个槽位；空租约返回零。
			 */
			[[nodiscard]] size_t amount() const noexcept
			{
				return amount_;
			}

			/**
			 * @brief 在存储 payload 前调整额度；槽位租约不能调整。
			 * @param bytes 新的总字节数，零表示释放字节但保留租约
			 * @return 成功返回 true；失败时本租约及连接原额度均不改变。
			 */
			[[nodiscard]] bool tryResize(size_t bytes) noexcept
			{
				if (!state_ || resource_ < Resource::hReceiveBytes || bytes > limit_)
				{
					return false;
				}
				auto index = static_cast<size_t>(resource_);
				auto& used = state_->used[index];
				if (bytes > amount_)
				{
					auto growth = bytes - amount_;
					if (growth > state_->limits[index] - used)
					{
						return false;
					}
					used += growth;
				}
				else
				{
					used -= amount_ - bytes;
				}
				amount_ = bytes;
				return true;
			}

			/**
			 * @brief 归还本租约；重复调用没有效果。
			 */
			void reset() noexcept
			{
				if (state_)
				{
					auto& used = state_->used[static_cast<size_t>(resource_)];
					assert(used >= amount_);
					used -= amount_;
					if (resource_ == Resource::hApplicationTask && state_->service)
					{
						state_->service->release();
					}
					state_.reset();
					amount_ = 0;
				}
			}

		private:
			friend class Http2Budget;

			Lease(std::shared_ptr<State> state, Resource resource, size_t amount, size_t limit) noexcept
				: state_(std::move(state)), resource_(resource), amount_(amount), limit_(limit)
			{
			}

			std::shared_ptr<State> state_;
			Resource resource_ {Resource::hProtocolStream};
			size_t amount_ {0};
			size_t limit_ {0};
		};

		/**
		 * @brief 创建连接预算；无效配置或服务端未提供共享服务预算时抛出 std::invalid_argument。
		 * @param role 服务端入站或客户端出站角色
		 * @param options 已配置的单流与连接上限
		 * @param service 服务端必须注入同一服务实例；客户端可为空，非空时也受此总额约束
		 */
		Http2Budget(Role role, const Http2Options& options, const Http2ServiceBudget* service = nullptr) : role_(role)
		{
			options.validate();
			if ((role != Role::hClient && role != Role::hServer) || (role == Role::hServer && !service)
				|| (service && !service->state_))
			{
				throw std::invalid_argument("HTTP/2 预算角色无效或服务端缺少共享服务预算");
			}
			state_ = std::make_shared<State>();
			auto server = role == Role::hServer;
			state_->limits = {options.maxConcurrentStreams,
							  options.maxApplicationTasks,
							  server ? options.maxConnectionReceiveBytes : options.maxClientConnectionReceiveBytes,
							  options.maxConnectionSendBytes};
			state_->streamReceiveLimit = server ? options.maxBodyBytes : options.maxResponseBytes;
			state_->service = service ? service->state_ : nullptr;
		}

		/**
		 * @brief 在接纳协议流前占一个槽；RST 只应归还此租约。
		 * @param peerLimit 对端 SETTINGS 的出站流上限，仅客户端使用；服务端忽略此值
		 * @return 成功租约或不改变额度的空租约。
		 */
		[[nodiscard]] Lease tryAcquireProtocolStream(uint32_t peerLimit = std::numeric_limits<uint32_t>::max()) noexcept
		{
			if (role_ == Role::hClient && state_->used[static_cast<size_t>(Resource::hProtocolStream)] >= peerLimit)
			{
				return {};
			}
			return acquire(Resource::hProtocolStream, 1, 1);
		}

		/**
		 * @brief 在启动应用任务前占槽；只能在任务真正退出后归还。
		 * @return 同时满足连接和服务总额的租约，失败返回空租约。
		 */
		[[nodiscard]] Lease tryAcquireApplicationTask() noexcept
		{
			return acquire(Resource::hApplicationTask, 1, 1);
		}

		/**
		 * @brief 为一条流保留接收 payload；每流持有一个租约并通过 tryResize 增减。
		 * @param bytes 写入流 buffer 前需要保留的字节数
		 * @return 同时满足单流和连接总额的租约，失败返回空租约。
		 */
		[[nodiscard]] Lease tryAcquireReceive(size_t bytes = 0) noexcept
		{
			return acquire(Resource::hReceiveBytes, bytes, state_->streamReceiveLimit);
		}

		/**
		 * @brief 为一条流保留待发 payload，包含响应排队和文件预取。
		 * @param bytes 放入发送存储前需要保留的字节数
		 * @return 满足连接总额的租约，失败返回空租约。
		 */
		[[nodiscard]] Lease tryAcquireSend(size_t bytes = 0) noexcept
		{
			return acquire(Resource::hSendBytes, bytes, state_->limits[static_cast<size_t>(Resource::hSendBytes)]);
		}

		/**
		 * @brief 查询所属 loop 上的连接占用。
		 * @return 各类资源当前占用的额度。
		 */
		[[nodiscard]] Usage usage() const noexcept
		{
			return {state_->used[static_cast<size_t>(Resource::hProtocolStream)],
					state_->used[static_cast<size_t>(Resource::hApplicationTask)],
					state_->used[static_cast<size_t>(Resource::hReceiveBytes)],
					state_->used[static_cast<size_t>(Resource::hSendBytes)]};
		}

	private:
		Lease acquire(Resource resource, size_t amount, size_t limit) noexcept
		{
			auto index = static_cast<size_t>(resource);
			if (amount > limit || amount > state_->limits[index] - state_->used[index])
			{
				return {};
			}
			if (resource == Resource::hApplicationTask && state_->service && !state_->service->acquire())
			{
				return {};
			}
			state_->used[index] += amount;
			return {state_, resource, amount, limit};
		}

		Role role_;
		std::shared_ptr<State> state_;
	};
} // namespace hical::detail

#endif
