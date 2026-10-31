/**
 * @file Http2Output.h
 * @brief HTTP/2 DATA 源、输出批次和实际写出确认
 */

#pragma once

#ifdef HICAL_HAS_HTTP2

	#include "Http2Budget.h"
	#include "Http2Codec.h"
	#include "core/Coroutine.h"
	#include <chrono>
	#include <functional>
	#include <memory>
	#include <span>
	#include <string>
	#include <string_view>
	#include <vector>

namespace hical::detail
{
	/**
	 * @brief 所属 loop 串行使用的输出调度；不执行网络或文件 I/O。
	 * 必须从 codec 的第一个输出批次开始独占 memSend，codec 生命周期覆盖本对象。
	 * wire 预留和 producer 共享同一 send 预算，预留确保满载生产者不阻塞控制帧及首 DATA。
	 */
	class Http2Output
	{
		struct State;
		struct Impl;

	public:
		class Producer final
			: public Http2Codec::DataSource
			, public std::enable_shared_from_this<Producer>
		{
			struct Impl;

		public:
			/**
			 * @brief 在读文件前持有发送额度；即使生产者取消，也保持到在途读取退出。
			 */
			class ReadPermit
			{
			public:
				ReadPermit() = default;
				~ReadPermit();
				ReadPermit(const ReadPermit&) = delete;
				ReadPermit& operator=(const ReadPermit&) = delete;
				ReadPermit(ReadPermit&& other) noexcept;
				ReadPermit& operator=(ReadPermit&& other) noexcept;
				/**
				 * @brief 查询可分配和读取的最大字节数。
				 * @return 已经计入连接发送预算的字节数。
				 */
				[[nodiscard]] size_t size() const noexcept;

			private:
				friend class Producer;
				ReadPermit(std::shared_ptr<State> state, Http2Budget::Lease lease, int32_t streamId);
				void release() noexcept;
				std::shared_ptr<State> state_;
				Http2Budget::Lease lease_;
				int32_t streamId_ {0};
			};

			struct ReadResult
			{
				ReadPermit permit;
				NetworkError error;
			};

			~Producer() override;
			/**
			 * @brief 等待窗口、上一块消费及连接额度，再预留一次有界文件读取。
			 * 入口同步持有生产者；等待使用独立定时器，不替换 writer 唤醒函数。
			 * @param maximum 本次读取上限，必须为正且不超过配置的文件预取上限
			 * @param deadline 整个响应沿用的绝对截止点
			 * @return 预留的读取额度或取消、超时错误。
			 */
			Awaitable<ReadResult> reserveRead(size_t maximum, std::chrono::steady_clock::time_point deadline);
			/**
			 * @brief 在同一 loop 内将预取额度转给发送队列，期间不挂起。
			 * @param bytes 不大于预留额度的已读数据，失败不移动
			 * @param permit 来自本生产者的在途额度
			 * @return 转交或状态错误；调用期间不重复计费。
			 */
			NetworkError appendReserved(std::string&& bytes, ReadPermit permit);
			/**
			 * @brief 为 handler 返回后的文件任务保留连接和服务应用配额。
			 * @return 至少保持到文件任务及物理 END 收尾的租约，超额时为空。
			 */
			[[nodiscard]] Http2Budget::Lease acquireApplicationTask();
			/**
			 * @brief 配置文件生产者的取消通知；回调只能发信号，不执行文件 I/O。
			 * @param callback 所属 loop 中执行的无异常通知，空函数撤销观察
			 */
			void setCancelHandler(std::function<void()> callback);
			/**
			 * @brief 预算通过后复制 payload；额度不足时明确失败，可待额度释放后重试。
			 * @param bytes 待发字节，空输入不产生队列项
			 * @return 超额、已结束、已取消或连接错误。
			 */
			NetworkError append(std::string_view bytes);
			/**
			 * @brief 预算通过后接管 payload；失败时不移动原字符串。
			 * @param bytes 待转交的自有字符串
			 * @return 超额、已结束、已取消或连接错误。
			 */
			NetworkError appendOwned(std::string&& bytes);
			/**
			 * @brief 按总字节数预留额度，再把收集到的数据块合并到一个队列项。
			 * @param chunks 调用期间有效的原始数据块，空块不占额外队列项
			 * @return 超额、已结束、已取消或连接错误；失败不修改输入。
			 */
			NetworkError appendChunks(std::span<const std::string> chunks);
			/**
			 * @brief 封存尾字段并声明生产结束；复制前校验头和预算。
			 * @param trailers 独立尾字段，可为空；禁止伪头和连接专用字段
			 * @return 封存错误；失败时仍允许重试。
			 */
			NetworkError finish(std::span<const Http2Codec::Header> trailers = {});
			/**
			 * @brief 停止本生产者并归还未发 payload；不直接复位 codec 或关闭 socket。
			 */
			void cancel();
			/**
			 * @brief 查询仍拥有并计费的 payload；部分读取的块在整块移除前继续计费。
			 * @return 当前队列及尾字段计费字节。
			 */
			[[nodiscard]] size_t queuedBytes() const noexcept;
			Http2Codec::DataRead read(std::span<char> output) override;
			std::span<const Http2Codec::Header> trailers() const noexcept override;
			void onClose(Http2Code code) override;

		private:
			friend class Http2Output;
			explicit Producer(std::shared_ptr<State> state);
			void stop(bool cancelPending);
			static Awaitable<ReadResult> reserveOwned(std::shared_ptr<Producer> producer,
													  size_t maximum,
													  std::chrono::steady_clock::time_point deadline);
			std::unique_ptr<Impl> impl_;
		};

		struct OpenResult
		{
			int32_t streamId {0};
			std::shared_ptr<Producer> producer;
			NetworkError error;
		};

		struct BatchResult
		{
			std::string_view bytes;
			NetworkError error;
		};

		struct AckResult
		{
			std::vector<int32_t> endedStreams;
			NetworkError error;
		};

		/**
		 * @brief 预留有界头部/批次的 wire 空间；无法同时保留 payload 推进空间时抛 invalid_argument。
		 * @param codec 独占其 memSend，必须比本对象后析构
		 * @param budget 与所有流共用的连接预算
		 * @param options 有限批次和字段配置
		 */
		Http2Output(Http2Codec& codec, Http2Budget budget, const Http2Options& options = {});
		~Http2Output();
		Http2Output(const Http2Output&) = delete;
		Http2Output& operator=(const Http2Output&) = delete;
		/**
		 * @brief 配置生产者就绪和取消时的唤醒；回调必须不抛异常。
		 * @param callback 所属 loop 中执行的唤醒函数，允许为空
		 */
		void setWakeup(std::function<void()> callback);
		/**
		 * @brief 提交尚未结束的请求及其 DATA 生产者。
		 * @param headers 调用期间复制的初始头
		 * @return 流、生产者或错误。
		 */
		OpenResult submitRequest(std::span<const Http2Codec::Header> headers);
		/**
		 * @brief 提交尚未结束的响应及其 DATA 生产者。
		 * @param streamId 请求流
		 * @param headers 调用期间复制的初始响应头
		 * @return 流、生产者或错误。
		 */
		OpenResult submitResponse(int32_t streamId, std::span<const Http2Codec::Header> headers);
		/**
		 * @brief 提交在初始 HEADERS 结束的响应，不创建 DATA 生产者。
		 * @param streamId 请求流
		 * @param headers 调用期间复制的初始响应头
		 * @return 成功时包含流号且 producer 为空；物理完成仍由 acknowledge 报告。
		 */
		OpenResult submitResponseHeaders(int32_t streamId, std::span<const Http2Codec::Header> headers);
		/**
		 * @brief 取消指定流的输出跟踪及生产者，不提交协议复位。
		 * @param streamId 已复位或不再需要发送的流，也支持无生产者的响应
		 */
		void cancelStream(int32_t streamId);
		/**
		 * @brief 取得当前输出批次的剩余视图；本批未写完时不推进 codec。
		 * @return 直到 acknowledge 完成本批或析构前有效的字节；close 不使在途视图悬垂。
		 */
		BatchResult nextBatch();
		/**
		 * @brief 确认实际写出的字节，按完整帧和 CONTINUATION 边界报告 END_STREAM。
		 * @param bytesWritten 本批实际写出的数量，不得超过剩余字节
		 * @return 首次完成物理写的流及错误；关闭后只释放批次，不报告成功。
		 */
		AckResult acknowledge(size_t bytesWritten);
		/**
		 * @brief 转发协议关闭，可在 codec 回调中调用；Producer 也会自动接收此通知。
		 * @param streamId 关闭的协议流
		 * @param code 正常关闭或复位错误码
		 */
		void onStreamClose(int32_t streamId, Http2Code code = Http2Code::hNoError);
		/**
		 * @brief 取消所有生产者和未来输出；当前批次保留到写回调收尾或析构。
		 */
		void close();
		/**
		 * @brief 查询所有生产者仍持有的 payload。
		 * @return 字节总数，不含固定 wire 预留。
		 */
		[[nodiscard]] size_t queuedBytes() const noexcept;
		/**
		 * @brief 查询当前拥有的 wire 字节，含本批已写前缀和 codec 余留。
		 * @return 不超过预留容量的 wire 字节。
		 */
		[[nodiscard]] size_t bufferedWireBytes() const noexcept;
		/**
		 * @brief 查询尚未完成物理发送的本地输出，不等待对端的消息结束。
		 * @return 有待生产/待窗口的流、正在序列化的输出或未写完的 wire 时为 true。
		 */
		[[nodiscard]] bool hasPendingOutput() const noexcept;

	private:
		std::unique_ptr<Impl> impl_;
	};
} // namespace hical::detail

#endif
