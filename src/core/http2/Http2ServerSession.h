/**
 * @file Http2ServerSession.h
 * @brief HTTP/2 服务端会话与 TLS 连接接入
 */

#pragma once

#ifdef HICAL_HAS_HTTP2

	#include "Http2Connection.h"
	#include "Http2Receive.h"
	#include "core/InetAddress.h"
	#include <exception>
	#include <functional>
	#include <memory>
	#include <unordered_map>

namespace hical
{
	class Router;
	class MiddlewarePipeline;
} // namespace hical

namespace hical::detail
{
	/**
	 * @brief 在 socket 所属 loop 串行使用；借用外层 socket，不接管关闭守卫或连接名额。
	 * run 会等收发泵和业务任务全部退出，返回后外层才能销毁 socket 和 idle 条目。
	 */
	class Http2ServerSession
		: private Http2Codec::Listener
		, public std::enable_shared_from_this<Http2ServerSession>
	{
	public:
		struct Context
		{
			Router& router;
			MiddlewarePipeline& middleware;
			InetAddress peerAddr;
			std::function<HttpResponse(const std::exception&, const HttpRequest&)> errorHandler;
			std::shared_ptr<Http2ServiceBudget> serviceBudget;
			std::function<void()> onActivity;
		};

		/**
		 * @brief 为已握手的 TLS socket 创建服务端会话，设置监听器后才启动 I/O。
		 * @param socket 地址稳定且由外层持有的 TLS socket
		 * @param options 启动时已冻结的有效资源配置
		 * @param context 只读路由/中间件、对端地址和跨连接共享的服务预算
		 * @return 会话拥有者；配置或初始化失败抛出异常。
		 */
		static std::shared_ptr<Http2ServerSession> create(Http2Connection::TlsSocket& socket,
														  const Http2Options& options,
														  Context context);
		~Http2ServerSession();
		Http2ServerSession(const Http2ServerSession&) = delete;
		Http2ServerSession& operator=(const Http2ServerSession&) = delete;

		/**
		 * @brief 调用时持有自身引用，等 reader、writer 和业务任务全部退出后返回。
		 * @return 连接首次错误；正常排空返回成功。
		 */
		Awaitable<NetworkError> run();
		/**
		 * @brief 在所属 loop 停止接纳新流并发送最终 GOAWAY，重复通知不重复提交。
		 */
		void drain();
		/**
		 * @brief 在首次派发前将已封存的请求头映射为流拥有的请求上下文，不发布 body。
		 * 请求视图借用该流的稳定头存储，应用协程必须持有流的 ApplicationGuard。
		 * CONNECT 和扩展方法映射为 hUnknown，原始方法仍从 initialHeaders 读取，由分发层处理。
		 * @param stream 初始头已封存且尚未映射、未关闭的入站请求流
		 * @param peerAddr 连接对端地址，复制到请求中
		 * @return 成功或状态/配置错误；失败不修改已有请求。
		 */
		[[nodiscard]] static NetworkError mapRequest(Http2Stream& stream, const InetAddress& peerAddr);
		/**
		 * @brief 同步保活会话和流，以预构建中间件及缓存路由结果分发已映射的请求。
		 * body 门仅等待此流接收完成，不读取 socket；普通前置中间件在门前始终只见头部。
		 * 入口同步取得唯一 ApplicationGuard，排队和应用链自然返回前均占任务配额；调用方不重复取得。
		 * 返回响应后应用任务已结束，发送方仍需持有流直到响应发送完成。
		 * @param stream 已由 mapRequest 映射、尚未启动应用任务的请求流
		 * @param bodyDeadline 接纳初始头时确定的绝对截止点，中间件挂起不会延长该时限
		 * @return 独立的应用响应；调用方必须在所属 loop 执行并收拢此操作。
		 */
		[[nodiscard]] Awaitable<HttpResponse> dispatch(std::shared_ptr<Http2Stream> stream,
													   Http2Stream::TimePoint bodyDeadline);

	private:
		struct Job;
		Http2ServerSession(Context context, boost::asio::any_io_executor executor, const Http2Options& options);
		static Awaitable<NetworkError> runOwned(std::shared_ptr<Http2ServerSession> self);
		static Awaitable<HttpResponse> dispatchOwned(std::shared_ptr<Http2ServerSession> self,
													 std::shared_ptr<Http2Stream> stream,
													 Http2Stream::TimePoint bodyDeadline,
													 Http2Stream::ApplicationGuard application);
		static Awaitable<void> runJob(std::shared_ptr<Http2ServerSession> self,
									  std::shared_ptr<Job> job,
									  Awaitable<HttpResponse> response);
		void jobFinished(int32_t streamId, std::exception_ptr exception);
		void responseWritten(int32_t streamId);
		void transportFailed(const NetworkError& error);
		void resetStream(int32_t streamId, Http2Code code);
		Job* discardedRequest(int32_t streamId) const;
		void headersReady(std::shared_ptr<Http2Stream> stream);
		Http2Code onBeginHeaders(int32_t streamId) override;
		Http2Code onHeader(int32_t streamId, Http2Codec::Header header) override;
		Http2Code onEndHeaders(const Http2Codec::HeaderBlock& block) override;
		Http2Code onData(int32_t streamId, std::string_view bytes) override;
		void onEndStream(int32_t streamId) override;
		void onStreamClose(int32_t streamId, Http2Code code) override;
		void onFrameReceived(const Http2Codec::Frame& frame) override;
		void onError(const NetworkError& error) override;

		Context context_;
		Http2Options options_;
		std::shared_ptr<Http2Connection> connection_;
		std::unique_ptr<Http2Receive> receive_;
		boost::asio::steady_timer jobsJoin_;
		std::unordered_map<int32_t, std::shared_ptr<Job>> jobs_;
		int32_t lastAccepted_ {0};
		bool draining_ {false};
	};
} // namespace hical::detail

#endif
