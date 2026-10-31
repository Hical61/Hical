/**
 * @file Http2Connection.h
 * @brief 已握手 TLS 连接的唯一 HTTP/2 读写泵
 */

#pragma once

#ifdef HICAL_HAS_HTTP2

	#include "Http2Budget.h"
	#include "Http2Codec.h"
	#include "Http2Output.h"
	#include "core/Coroutine.h"
	#include <boost/asio/ip/tcp.hpp>
	#include <boost/asio/ssl/stream.hpp>
	#include <boost/asio/steady_timer.hpp>
	#include <exception>
	#include <functional>
	#include <memory>

namespace hical::detail
{
	/**
	 * @brief 所有操作在 socket 所属 loop 串行执行；每个连接最多一读一写。
	 * 借用已握手的 socket，不再执行握手。调用方必须等待 run 返回后才能销毁 socket、
	 * listener 和回调引用的对象；业务任务的收尾由角色会话负责。
	 */
	class Http2Connection
		: private Http2Codec::Listener
		, public std::enable_shared_from_this<Http2Connection>
	{
	public:
		using TlsSocket = boost::asio::ssl::stream<boost::asio::ip::tcp::socket>;

		struct Callbacks
		{
			std::function<void()> onActivity;
			std::function<void(int32_t)> onWritten;
			std::function<void(const NetworkError&)> onFailure;
		};

		/**
		 * @brief 创建尚未开始 I/O 的连接。
		 * @param socket 已握手且地址稳定的 TLS 流
		 * @param role 协议角色
		 * @param options 有限缓冲及阶段超时
		 * @param budget 共享同一连接计数的预算；服务端已注入共享服务额度
		 * @param callbacks 所属 loop 上的活跃刷新、物理发送完成和首次故障通知
		 * @return 连接拥有者；无效配置或分配失败抛异常。
		 */
		static std::shared_ptr<Http2Connection> create(TlsSocket& socket,
													   Http2Codec::Role role,
													   const Http2Options& options,
													   Http2Budget budget,
													   Callbacks callbacks = {});

		~Http2Connection();
		Http2Connection(const Http2Connection&) = delete;
		Http2Connection& operator=(const Http2Connection&) = delete;

		/**
		 * @brief 在启动前设置借用的角色监听器，连接负责转发所有协议事件。
		 * @param listener 覆盖 run 整个执行期的对象，可为空
		 * @return 启动后返回状态错误。
		 */
		NetworkError setListener(Http2Codec::Listener* listener);
		/**
		 * @brief 调用时捕获强引用，运行并等待 reader、writer 全部退出。
		 * @return 首次连接故障；finish 正常收尾返回成功。只能运行一次。
		 */
		Awaitable<NetworkError> run();
		/**
		 * @brief 在所属 loop 立即关闭传输，唤醒所有协议等待；重复调用无效果。
		 * @param error 首次故障；空错误表示主动取消
		 */
		void close(NetworkError error = {});
		/**
		 * @brief 发送完当前输出后结束；调用前须保证不会再有业务生产数据。
		 */
		void finish();
		/**
		 * @brief 唤醒 writer；提交控制帧或收到可写窗口后调用。
		 */
		void wakeWriter();
		/**
		 * @brief 获取只在所属 loop 使用的协议引擎。
		 * @return 内部协议对象，禁止另建 reader/writer 或直接 memSend。
		 */
		[[nodiscard]] Http2Codec& codec() noexcept;
		/**
		 * @brief 获取唯一发送队列。
		 * @return 角色会话与文件生产者提交数据的入口。
		 */
		[[nodiscard]] Http2Output& output() noexcept;
		/**
		 * @brief 查询是否已开始且尚未结束传输。
		 * @return 所属 loop 上的运行状态。
		 */
		[[nodiscard]] bool running() const noexcept;
		/**
		 * @brief 获取首次连接错误。
		 * @return 正常状态下为空。
		 */
		[[nodiscard]] const NetworkError& error() const noexcept;

	private:
		Http2Connection(TlsSocket& socket,
						Http2Codec::Role role,
						const Http2Options& options,
						Http2Budget budget,
						Callbacks callbacks);
		static Awaitable<NetworkError> runOwned(std::shared_ptr<Http2Connection> self);
		static Awaitable<void> writeOwned(std::shared_ptr<Http2Connection> self);
		Awaitable<void> readLoop();
		Awaitable<void> writeLoop();
		Awaitable<void> writeBatch(std::string_view bytes);
		void stopTransport(NetworkError error, bool notify);
		void writerDone(std::exception_ptr exception);
		void touch();

		Http2Code onBeginHeaders(int32_t streamId) override;
		Http2Code onHeader(int32_t streamId, Http2Codec::Header header) override;
		Http2Code onEndHeaders(const Http2Codec::HeaderBlock& block) override;
		Http2Code onData(int32_t streamId, std::string_view bytes) override;
		void onEndStream(int32_t streamId) override;
		void onStreamClose(int32_t streamId, Http2Code code) override;
		void onHeaderBlock(int32_t streamId, bool complete) override;
		void onFrameReceived(const Http2Codec::Frame& frame) override;
		void onFrameSent(const Http2Codec::Frame& frame) override;
		void onSettings(bool ack) override;
		void onGoaway(int32_t lastStreamId, Http2Code code, std::string_view debugData) override;
		void onError(const NetworkError& error) override;

		TlsSocket& socket_;
		Http2Options options_;
		Callbacks callbacks_;
		Http2Codec codec_;
		Http2Output output_;
		Http2Codec::Listener* listener_ {nullptr};
		boost::asio::steady_timer writerWake_;
		boost::asio::steady_timer writerJoin_;
		boost::asio::steady_timer negotiationTimer_;
		boost::asio::steady_timer headerTimer_;
		boost::asio::steady_timer writeTimer_;
		uint64_t writeGeneration_ {0};
		uint64_t headerGeneration_ {0};
		int32_t headerStreamId_ {0};
		NetworkError error_;
		NetworkError protocolError_;
		bool started_ {false};
		bool stopped_ {false};
		bool finishing_ {false};
		bool writerActive_ {false};
		bool peerSettings_ {false};
	};
} // namespace hical::detail

#endif
