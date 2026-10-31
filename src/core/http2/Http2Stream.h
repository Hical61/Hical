/**
 * @file Http2Stream.h
 * @brief HTTP/2 流状态、任务生命周期和 body 完成通知
 */

#pragma once

#ifdef HICAL_HAS_HTTP2

	#include "Http2Budget.h"
	#include "Http2Headers.h"
	#include "core/HttpRequest.h"
	#include "core/HttpResponse.h"
	#include <boost/asio/any_io_executor.hpp>
	#include <boost/asio/awaitable.hpp>
	#include <boost/asio/steady_timer.hpp>
	#include <chrono>
	#include <limits>
	#include <list>
	#include <memory>
	#include <string>
	#include <string_view>

namespace hical::detail
{
	/**
	 * @brief 一个协议流的稳定拥有者；所有操作和析构在同一个 owner loop，single-threaded, no lock needed。
	 * 初始头、尾头、暂存 body 与映射对象均独立于连接读缓冲和 HPACK 回调。RST 只结束协议输出，
	 * ApplicationGuard 必须随业务协程活到自然退出，不能通过销毁协程帧归还资源。
	 */
	class Http2Stream : public std::enable_shared_from_this<Http2Stream>
	{
	public:
		using Clock = std::chrono::steady_clock;
		using TimePoint = Clock::time_point;

		struct Config
		{
			int32_t streamId {0};
			Http2Codec::HeaderKind incomingKind {Http2Codec::HeaderKind::hRequest};
			uint32_t peerLimit {std::numeric_limits<uint32_t>::max()};
			Http2Options options;
		};

		/**
		 * @brief 跨挂起点保活流并持有应用配额；析构、reset 或移动替换时归还一次。
		 */
		class ApplicationGuard
		{
		public:
			ApplicationGuard() = default;
			~ApplicationGuard();
			ApplicationGuard(const ApplicationGuard&) = delete;
			ApplicationGuard& operator=(const ApplicationGuard&) = delete;
			/**
			 * @brief 转移正在运行的应用任务。
			 * @param other 转出后成为空 guard
			 */
			ApplicationGuard(ApplicationGuard&& other) noexcept;
			/**
			 * @brief 结束自身任务后接管另一个任务。
			 * @param other 转出后成为空 guard；允许自身移动
			 * @return 当前 guard。
			 */
			ApplicationGuard& operator=(ApplicationGuard&& other) noexcept;
			/**
			 * @brief 查询是否仍持有应用任务。
			 * @return 持有任务为 true。
			 */
			[[nodiscard]] explicit operator bool() const noexcept;
			/**
			 * @brief 结束应用任务，释放保活引用；重复调用没有效果。
			 */
			void reset() noexcept;

		private:
			friend class Http2Stream;
			explicit ApplicationGuard(std::shared_ptr<Http2Stream> stream) noexcept;
			std::shared_ptr<Http2Stream> stream_;
		};

		/**
		 * @brief 检查配置并预留协议、应用槽后创建流；头部尚未完成也占应用槽。
		 * @param executor 所属 loop 的执行器
		 * @param config 流 ID、接收角色及资源配置；无效值抛出 std::invalid_argument
		 * @param budget 按值共享 State，不借用连接 facade；服务端必须已注入共享服务预算
		 * @return 有稳定所有权的流；额度不足返回空指针且不留下部分预留。
		 */
		[[nodiscard]] static std::shared_ptr<Http2Stream> create(boost::asio::any_io_executor executor,
																 const Config& config,
																 Http2Budget budget);
		/**
		 * @brief 释放未转出的资源；业务任务和等待者持有共享所有权时不会进入析构。
		 */
		~Http2Stream();
		Http2Stream(const Http2Stream&) = delete;
		Http2Stream& operator=(const Http2Stream&) = delete;

		/**
		 * @brief 获取协议流 ID。
		 * @return 正数流 ID。
		 */
		[[nodiscard]] int32_t id() const noexcept;
		/**
		 * @brief 获取初始头存储，封存后才能映射视图。
		 * @return 此流独立的初始头。
		 */
		[[nodiscard]] Http2Headers& initialHeaders() noexcept;
		/**
		 * @brief 获取独立尾头存储。
		 * @return 此流的尾头，不能混入初始头。
		 */
		[[nodiscard]] Http2Headers& trailers() noexcept;
		/**
		 * @brief 获取请求映射上下文；body 在 publishBody 成功前保持不可见。
		 * @return 由流拥有的请求对象。
		 */
		[[nodiscard]] HttpRequest& request() noexcept;
		/**
		 * @brief 获取响应映射上下文；入站响应 body 在 publishBody 成功前保持不可见。
		 * @return 由流拥有的响应对象。
		 */
		[[nodiscard]] HttpResponse& response() noexcept;
		/**
		 * @brief 启动唯一应用任务，返回的 guard 必须跨越所有业务挂起点。
		 * @return 首次启动且流未失败时的保活 guard；重复、已关闭或已结束时为空。
		 */
		[[nodiscard]] ApplicationGuard startApplication();
		/**
		 * @brief 在接收存储前保留额度，再复制 DATA payload；不做协议补窗。
		 * @param bytes 不含 padding 的应用数据
		 * @return 成功或流错误；超额使流失败，已接收字节保留到拥有者退出。
		 */
		NetworkError appendBody(std::string_view bytes);
		/**
		 * @brief 标记 body 完成并唤醒等待者，只保留首次结果；此时不向请求或响应发布 body。
		 * @return body 完成结果，失败后的迟到完成不能改写结果。
		 */
		NetworkError completeBody();
		/**
		 * @brief 在 body 门成功且中间件放行后，向角色对应的映射对象发布完整 body。
		 * @return 成功或尚未完成、流已失败的错误；重复发布不改变数据。
		 */
		NetworkError publishBody();
		/**
		 * @brief 获取成功完成的 body，映射对象发布前后都可使用。
		 * @return 尚未完成或接收失败时为空；视图不能跨越 publishBody、body 修改或拥有者销毁。
		 */
		[[nodiscard]] std::string_view body() const noexcept;
		/**
		 * @brief 等待 body 首次完成；调用时便捕获强引用，支持多个等待者。
		 * @param deadline 绝对截止点，按首次完整接收时刻判断；超时或调用者取消使未关闭流失败
		 * @return 首次 body 完成结果；必须从所属 loop 等待。
		 */
		[[nodiscard]] boost::asio::awaitable<NetworkError> waitBody(TimePoint deadline = TimePoint::max());
		/**
		 * @brief 以首次错误结束协议并唤醒未完成 body 等待；不强停正在运行的应用任务。
		 * @param error 连接或流错误；无错误值按本地取消处理
		 * @return 流的首个错误；正常关闭后的迟到失败不改变结果。
		 */
		NetworkError fail(NetworkError error);
		/**
		 * @brief 复位协议流；应用和内存额度随实际拥有者收尾。
		 * @param code 本地或对端 RST_STREAM 的 RFC 错误码
		 */
		void reset(Http2Code code = Http2Code::hCancel);
		/**
		 * @brief 结束正常协议流；若 body 尚未完成，按截断流失败处理。
		 */
		void closeProtocol();
		/**
		 * @brief 调整本流待发 payload 额度；生产者必须在保存输出前调用。
		 * @param bytes 当前仍拥有的待发总字节数；关闭后只允许减少
		 * @return 是否成功，失败保持原额度。
		 */
		[[nodiscard]] bool reserveSend(size_t bytes) noexcept;
		/**
		 * @brief 查询协议是否还允许输出。
		 * @return 尚未 reset、失败或正常关闭为 true。
		 */
		[[nodiscard]] bool protocolOpen() const noexcept;
		/**
		 * @brief 查询 body 门是否已有最终结果，成功和失败都算完成。
		 * @return 等待者无需继续等待为 true。
		 */
		[[nodiscard]] bool bodyComplete() const noexcept;
		/**
		 * @brief 查询仍由流拥有的接收 payload 计费。
		 * @return 暂存或已发布 body 的字节数。
		 */
		[[nodiscard]] size_t receivedBytes() const noexcept;
		/**
		 * @brief 查询流的首次失败。
		 * @return 稳定的错误引用，成功时为空错误。
		 */
		[[nodiscard]] const NetworkError& error() const noexcept;

	private:
		struct Reservations;
		Http2Stream(boost::asio::any_io_executor executor, const Config& config, Reservations reservations);
		void completeBodyOnce(bool failed);
		NetworkError bodyResult(TimePoint deadline);
		void finishApplication() noexcept;
		static boost::asio::awaitable<NetworkError> waitBodyOwned(std::shared_ptr<Http2Stream> stream,
																  TimePoint deadline);

		boost::asio::any_io_executor executor_;
		int32_t id_;
		Http2Codec::HeaderKind incomingKind_;
		Http2Headers initialHeaders_;
		Http2Headers trailers_;
		Http2Budget::Lease protocol_;
		Http2Budget::Lease application_;
		Http2Budget::Lease receive_;
		Http2Budget::Lease send_;
		HttpRequest request_;
		HttpResponse response_;
		std::string pendingBody_;
		std::list<std::shared_ptr<boost::asio::steady_timer>> bodyWaiters_;
		NetworkError error_;
		TimePoint bodyCompletedAt_ {};
		bool bodyComplete_ {false};
		bool bodyFailed_ {false};
		bool bodyPublished_ {false};
		bool applicationStarted_ {false};
		bool applicationFinished_ {false};
	};
} // namespace hical::detail

#endif
