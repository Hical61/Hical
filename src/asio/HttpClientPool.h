/**
 * @file HttpClientPool.h
 * @brief 协程 HTTP 连接池：连接复用与有界请求调度
 */

#pragma once

#include "HttpClient.h"
#include <boost/asio/io_context.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace hical
{

	/**
	 * @brief 连接池配置
	 * 配置空闲回收和每桶保留数量；请求的超时、TLS 和协议配置由调用方另行传入。
	 */
	struct HttpClientPoolOptions
	{
		/// 空闲连接默认保留时长
		static constexpr auto kDefaultIdleTimeout = std::chrono::seconds {30};

		/// 清理扫描默认周期
		static constexpr auto kDefaultCleanupInterval = std::chrono::seconds {10};

		/// 单条空闲连接超过这个时长没人用就关掉
		std::chrono::steady_clock::duration idleTimeout {kDefaultIdleTimeout};

		/// 清理扫描周期（一个 timer 管整池）
		std::chrono::steady_clock::duration cleanupInterval {kDefaultCleanupInterval};

		/// 每个连接身份桶保留的空闲连接上限，超过时移除最老的
		std::size_t maxIdlePerBucket {8};
	};

	/**
	 * @brief 协程 HTTP 连接池
	 * 按目标地址、TLS context、有效 SNI、验证模式、协议策略及生效的 H2 配置分桶。取连接时优先
	 * 复用桶里还活着的，没有才新建；高层 H2 请求共享活动连接并按容量有界排队。
	 * acquire/release 始终独占租借，不与高层 H2 调度共享连接。
	 * 每个 io_context 使用一个池，连接和队列只在该事件循环访问，不加锁。
	 * 整个池共用一个清理 timer，析构或 shutdown() 时取消，避免阻止 io_context::run() 返回。
	 * request/get/post 返回前持有参数和内部状态，并记录本次调用的绝对请求期限。
	 * 析构和 shutdown() 可从其他线程调用，清理会 post 回所属事件循环。
	 * io_context 须继续运行到清理完成；若已经停止，需再次 run()/poll() 才会执行清理。
	 */
	class HttpClientPool
	{
	public:
		/**
		 * @brief 构造连接池并启动清理协程
		 * @param ioCtx 事件循环，须存活到本池的请求和清理操作全部结束
		 * @param options 池子配置；cleanupInterval / idleTimeout 非正会抛 std::invalid_argument
		 */
		explicit HttpClientPool(boost::asio::io_context& ioCtx, HttpClientPoolOptions options = {});

		/**
		 * @brief 投递停机清理并释放池持有的内部状态
		 * 请求和清理协程各自持有状态，退出后才会释放最后一份引用。
		 */
		~HttpClientPool();

		HttpClientPool(const HttpClientPool&) = delete;
		HttpClientPool& operator=(const HttpClientPool&) = delete;
		HttpClientPool(HttpClientPool&&) = delete;
		HttpClientPool& operator=(HttpClientPool&&) = delete;

		/**
		 * @brief 取一条到目标 host:port 的可用连接
		 * 优先复用对应身份桶中的空闲连接，否则新建；使用完成后调用 release() 归还。
		 * @param host 目标主机
		 * @param port 目标端口
		 * @param options 客户端配置；tlsContext 非空即走 HTTPS（跟明文分桶）
		 * @return 独占租借的连接实例；停机后返回空指针
		 */
		std::shared_ptr<HttpClient> acquire(const std::string& host,
											uint16_t port,
											const HttpClientOptions& options = {});

		/**
		 * @brief 把连接还回池子
		 * 还活着的放回对应桶，坏掉的（出错 / 超时被置 closed）直接销毁。
		 * 有保存或运行中请求时仍由调用方管理，不作为空闲容量，也不主动关闭。
		 * 外来实例和其他池的连接不能进入本池。
		 * @param client 之前 acquire 出来的连接
		 */
		void release(std::shared_ptr<HttpClient> client);

		/**
		 * @brief 发起请求并由连接池管理连接
		 * 最多重试一次，默认仅限 GET/HEAD/PUT/DELETE/OPTIONS；POST/PATCH 不自动重放。
		 * H2 还须有未发送的瞬态传输错误，或 REFUSED_STREAM/GOAWAY 的明确未处理证据；
		 * H1 仅保留复用陈旧连接的瞬态传输错误重试。status 为零本身不构成重试依据。
		 * 请求体由入口持有，可供重放；重试沿用原请求期限和首次实际排队的期限。
		 * @param host 目标主机
		 * @param port 目标端口
		 * @param method HTTP 方法
		 * @param target 请求目标，如 "/api/notice"
		 * @param body 请求体；为空则不带 body
		 * @param headers 附加请求头
		 * @param options 客户端配置
		 * @return 请求结果；网络错误在 result.error 里，不抛异常
		 */
		Awaitable<HttpClientResult> request(const std::string& host,
											uint16_t port,
											HttpMethod method,
											std::string_view target,
											std::string_view body = {},
											const HeaderMap& headers = {},
											const HttpClientOptions& options = {});

		/**
		 * @brief 发起 GET 请求
		 * @param host 目标主机
		 * @param port 目标端口
		 * @param target 请求目标
		 * @param headers 附加请求头
		 * @param options 客户端配置
		 * @return 完整响应或网络错误
		 */
		Awaitable<HttpClientResult> get(const std::string& host,
										uint16_t port,
										std::string_view target,
										const HeaderMap& headers = {},
										const HttpClientOptions& options = {});

		/**
		 * @brief 发起 POST 请求
		 * @param host 目标主机
		 * @param port 目标端口
		 * @param target 请求目标
		 * @param body 请求体
		 * @param contentType Content-Type，默认 application/json
		 * @param options 客户端配置
		 * @return 完整响应或网络错误
		 */
		Awaitable<HttpClientResult> post(const std::string& host,
										 uint16_t port,
										 std::string_view target,
										 std::string_view body,
										 std::string_view contentType = "application/json",
										 const HttpClientOptions& options = {});

		/**
		 * @brief 主动停机：取消本池高层请求、关闭空闲连接并取消清理 timer
		 * acquire() 借出的连接仍由持有者管理，不主动关闭其业务交换。
		 * 幂等，可从任意线程调；具体动作 post 回 io_context 线程执行，
		 * 事件循环不跑了就等下一次 run()/poll()。
		 */
		void shutdown();

		/**
		 * @brief 查询池中的空闲连接总数
		 * @return 当前空闲连接数；仅在所属事件循环调用
		 */
		[[nodiscard]] std::size_t idleCount() const noexcept;

		/** 协商模式的调度统计，包含退回 H1 的连接；不统计 acquire() 租借和 H1-only 桶。 */
		struct Stats
		{
			std::size_t managedConnections {0};    ///< 当前受调度器跟踪的连接，含首请求预留的建连槽
			std::size_t connectingConnections {0}; ///< 正在建连或首请求尚未进入建连的连接
			std::size_t activeRequests {0};        ///< 已获连接预留的请求，含尚未建立协议流的首请求
			std::size_t queuedRequests {0};        ///< 尚未获得连接/流容量的有限队列总长度
			std::size_t peakQueuedRequests {0};    ///< 池生命周期内所有 origin 同时排队的峰值
		};

		/**
		 * @brief 查询当前调度负载，用于观察连接预算及队列拥塞。
		 * @return 所属 io_context 上的一致快照；H2 未编译或未使用协商入口时为零。
		 * @note 仅在所属事件循环调用，不支持跨线程读取；峰值不因队列排空而清零。
		 */
		[[nodiscard]] Stats stats() const noexcept;

	private:
		/// 连接及其是否来自空闲队列。
		struct Lease
		{
			std::shared_ptr<HttpClient> client;
			bool reused {false};
		};

		/// 请求和清理协程共同持有，覆盖池对象析构后的异步收尾。
		struct Impl;
		struct OwnedRequest;

		/// 取连接并区分新建 / 复用（静态，不碰 this，方便请求协程按值持有 Impl）
		static Lease acquireFrom(Impl& impl,
								 const std::string& host,
								 uint16_t port,
								 const HttpClientOptions& options,
								 bool managed = false);

		/// 还连接：活着的入桶，坏的丢掉（静态，理由同上）
		static void releaseTo(Impl& impl, std::shared_ptr<HttpClient> client);

		/// 请求入口都转到这里，协程帧按值持有 Impl 和参数，不捕获裸 this。
		static Awaitable<HttpClientResult> doRequest(std::shared_ptr<Impl> impl, OwnedRequest request);
#ifdef HICAL_HAS_HTTP2
		static Awaitable<HttpClientResult> doManagedRequest(std::shared_ptr<Impl> impl, OwnedRequest request);
#endif

		/// 置停机标志并把「关空闲连接 + 取消 timer」post 到 io_context 线程执行
		void scheduleStop();

		/// 清理循环：等一个 cleanupInterval → 扫一遍所有桶 → 再等下一轮，直到 stopping
		static Awaitable<void> cleanupLoop(std::shared_ptr<Impl> impl);

		std::shared_ptr<Impl> impl_;
	};

} // namespace hical
