/**
 * @file HttpClientPool.h
 * @brief 协程 HTTP 连接池：按 host:port 分桶复用连接
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
	 * 只管池子自己的事（多久清一次、空闲多久算过期、每桶最多留几条），
	 * 跟单个客户端的超时 / TLS 配置分开——那些按 acquire / post 调用时传。
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

		/// 每个 host:port 桶最多留几条空闲连接，超了挤掉最老的
		std::size_t maxIdlePerBucket {8};
	};

	/**
	 * @brief 协程 HTTP 连接池
	 * 按 "host:port"（再加是否 TLS）分桶，每桶一个空闲连接队列。取连接时优先
	 * 复用桶里还活着的，没有才新建；还回来时连接已经坏掉就直接销毁、不回队列。
	 * 定位是「每个 io_context 一个池、不跨线程共享」——池子所有状态都跑在构造时
	 * 传入的那个 io_context 上，不加锁。多线程场景就每个 loop 各建一个池。
	 * 空闲清理靠一个 timer 管整池（照 IdleScanner 的思路，不给每条连接配 timer）。
	 * 那个 timer 由清理协程持有、池子也存了一份句柄，析构 / shutdown() 时能主动
	 * cancel 掉——不这么做的话，一个还挂着的 steady_timer 会让 io_context::run()
	 * 一直不返回（这个坑项目里 SSE 心跳、WS ping 都踩过）。
	 * 生命周期：池子所有读写连接的操作都跑在构造时那个 io_context 上。析构 / shutdown()
	 * 会把「取消 timer、关空闲连接」post 回 io_context 线程执行，所以从别的线程调它们是
	 * 安全的；但事件循环若不跑了，这些动作要等下一次 run()/poll() 才会落地。
	 */
	class HttpClientPool
	{
	public:
		/**
		 * @brief 构造连接池并启动清理协程
		 * @param ioCtx 事件循环，生命周期必须长于本对象
		 * @param options 池子配置；cleanupInterval / idleTimeout 非正会抛 std::invalid_argument
		 */
		explicit HttpClientPool(boost::asio::io_context& ioCtx, HttpClientPoolOptions options = {});

		/**
		 * @brief 析构：停掉清理 timer、放掉池子引用
		 * 清理协程自己持有一份内部状态，所以这里丢掉引用是安全的——协程收到取消后
		 * 才把状态真正释放掉，不会留下悬空。
		 */
		~HttpClientPool();

		HttpClientPool(const HttpClientPool&) = delete;
		HttpClientPool& operator=(const HttpClientPool&) = delete;
		HttpClientPool(HttpClientPool&&) = delete;
		HttpClientPool& operator=(HttpClientPool&&) = delete;

		/**
		 * @brief 取一条到目标 host:port 的可用连接
		 * 桶里有还活着的空闲连接就复用，否则新建一条。拿到的连接用完必须还回来
		 * （release），别自己留着——池子靠还回来的连接维持复用。
		 * @param host 目标主机
		 * @param port 目标端口
		 * @param options 客户端配置；tlsContext 非空即走 HTTPS（跟明文分桶）
		 * @return 连接实例
		 */
		std::shared_ptr<HttpClient> acquire(const std::string& host,
											uint16_t port,
											const HttpClientOptions& options = {});

		/**
		 * @brief 把连接还回池子
		 * 还活着的放回对应桶，坏掉的（出错 / 超时被置 closed）直接销毁。
		 * @param client 之前 acquire 出来的连接
		 */
		void release(std::shared_ptr<HttpClient> client);

		/**
		 * @brief 发一次请求，内部自动 acquire / release
		 * 复用的连接可能已经被对端悄悄关了——这种情况下会丢掉死连接、重开一条重试
		 * 一次，免得把一次本可成功的请求因为陈旧 keep-alive 连接而判死。
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
		 * @brief 便捷 GET，内部自动 acquire / release
		 */
		Awaitable<HttpClientResult> get(const std::string& host,
										uint16_t port,
										std::string_view target,
										const HeaderMap& headers = {},
										const HttpClientOptions& options = {});

		/**
		 * @brief 便捷 POST，内部自动 acquire / release
		 * @param contentType Content-Type，默认 application/json
		 */
		Awaitable<HttpClientResult> post(const std::string& host,
										 uint16_t port,
										 std::string_view target,
										 std::string_view body,
										 std::string_view contentType = "application/json",
										 const HttpClientOptions& options = {});

		/**
		 * @brief 主动停机：关掉所有空闲连接并取消清理 timer
		 * 幂等，可从任意线程调；具体动作 post 回 io_context 线程执行，
		 * 事件循环不跑了就等下一次 run()/poll()。
		 */
		void shutdown();

		/**
		 * @brief 当前桶里空闲连接总数（主要给测试用）
		 */
		[[nodiscard]] std::size_t idleCount() const noexcept;

	private:
		/// 一次取连接的结果：连接本体 + 是不是从空闲队列里翻出来的
		struct Lease
		{
			std::shared_ptr<HttpClient> client;
			bool reused {false};
		};

		/// 池子内部状态。用 shared_ptr 持有，清理协程和各请求协程也各留一份引用，
		/// 这样池子析构后协程还能安全收尾，不会踩到已经没了的状态。
		struct Impl;

		/// 取连接并区分新建 / 复用（静态，不碰 this，方便请求协程按值持有 Impl）
		static Lease acquireFrom(Impl& impl, const std::string& host, uint16_t port, const HttpClientOptions& options);

		/// 还连接：活着的入桶，坏的丢掉（静态，理由同上）
		static void releaseTo(Impl& impl, std::shared_ptr<HttpClient> client);

		/// 发请求的实打实逻辑。入口 get/post/request 都委托到它，协程帧里
		/// 只按值持有 shared_ptr<Impl>，不捕获裸 this。
		static Awaitable<HttpClientResult> doRequest(std::shared_ptr<Impl> impl,
													 const std::string& host,
													 uint16_t port,
													 HttpMethod method,
													 std::string_view target,
													 std::string_view body,
													 const HeaderMap& headers,
													 const HttpClientOptions& options);

		/// 置停机标志并把「关空闲连接 + 取消 timer」post 到 io_context 线程执行
		void scheduleStop();

		/// 清理循环：等一个 cleanupInterval → 扫一遍所有桶 → 再等下一轮，直到 stopping
		static Awaitable<void> cleanupLoop(std::shared_ptr<Impl> impl);

		/// 拼分桶键："host:port"，TLS 再加 ":tls" 后缀
		static std::string makeBucketKey(std::string_view host, uint16_t port, bool tls);

		std::shared_ptr<Impl> impl_;
	};

} // namespace hical
