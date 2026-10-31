/**
 * @file Http2ClientSession.h
 * @brief HTTP/2 客户端会话，持有 TLS 连接并等待收发任务结束
 */

#pragma once

#ifdef HICAL_HAS_HTTP2

	#include "HttpClient.h"
	#include "core/http2/Http2Connection.h"
	#include <functional>
	#include <memory>

namespace hical::detail
{
	/**
	 * @brief 在所属 loop 串行运行的客户端会话，不负责建连和协议选择
	 * socket 晚于借用它的 connection 析构，run 的协程帧持有会话直到收发任务退出。
	 * 会话不保存持有自身的后台任务；启动方须持有上层状态，直到 run 返回。
	 */
	class Http2ClientSession : public std::enable_shared_from_this<Http2ClientSession>
	{
	public:
		using TlsSocket = Http2Connection::TlsSocket;
		using TimePoint = std::chrono::steady_clock::time_point;

		/** 请求持有参数和绝对期限，后续排队或取消不能重置期限。 */
		struct Request
		{
			HttpMethod method {HttpMethod::hGet};
			std::string authority;
			std::string target;
			std::string body;
			HeaderMap headers;
			TimePoint deadline {TimePoint::max()};
		};

		/** 所属 loop 的瞬时容量；已分配但尚未发出 HEADERS 的流同样占 admitted。 */
		struct Capacity
		{
			bool settingsSeen {false};
			bool accepting {false};
			uint32_t localLimit {0};
			uint32_t peerLimit {0};
			size_t admitted {0};
			size_t available {0};
		};

		/**
		 * @brief 创建尚未启动收发的客户端会话；不执行网络 I/O。
		 * @param socket 已握手且已选择 H2 的稳定 TLS 对象；空对象抛 invalid_argument
		 * @param options 有限协议资源及收发时限
		 * @return 持有传输和协议状态的会话。
		 */
		static std::shared_ptr<Http2ClientSession> create(std::shared_ptr<TlsSocket> socket,
														  const Http2Options& options = {});
		~Http2ClientSession();
		Http2ClientSession(const Http2ClientSession&) = delete;
		Http2ClientSession& operator=(const Http2ClientSession&) = delete;

		/**
		 * @brief 返回前持有会话，异步运行并等待 reader 和 writer 退出
		 * @return 首次连接错误；重复运行返回状态错误。
		 */
		Awaitable<NetworkError> run();
		/**
		 * @brief 返回前持有参数和会话，收到首个 SETTINGS 后按容量接纳请求
		 * @param request 自有请求及原绝对期限；满槽只在有限应用队列等待，不排入协议库队列
		 * @return 完整响应或带真实协议证据的错误；仅完整成功响应发布 body。
		 */
		Awaitable<HttpClientResult> request(Request request);
		/**
		 * @brief 查询本地与对端共同约束的容量，不执行 I/O。
		 * @return 只在所属 loop 使用的瞬时快照。
		 */
		[[nodiscard]] Capacity capacity() const noexcept;
		/**
		 * @brief 设置容量变化通知，用于唤醒等待容量的请求
		 * @param callback 所属 loop 的无异常通知；不能重入收发泵或强持本会话形成环
		 */
		void setCapacityChanged(std::function<void()> callback);
		/**
		 * @brief 在所属 loop 取消收发，run 返回后传输才可释放；可重复调用。
		 * @param error 连接终止原因，空值表示主动取消
		 */
		void close(NetworkError error = {});
		/**
		 * @brief 获取底层连接，供所属 loop 的协议回调使用
		 * @return 会话持有的连接引用，有效期不超过会话
		 */
		[[nodiscard]] Http2Connection& connection() noexcept;

	private:
		struct Impl;
		Http2ClientSession(std::shared_ptr<TlsSocket> socket, const Http2Options& options);
		static Awaitable<NetworkError> runOwned(std::shared_ptr<Http2ClientSession> self);
		static Awaitable<HttpClientResult> requestOwned(std::shared_ptr<Http2ClientSession> self, Request request);

		std::shared_ptr<TlsSocket> socket_;
		std::unique_ptr<Impl> impl_;
	};
} // namespace hical::detail

#endif
