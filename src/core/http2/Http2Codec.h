/**
 * @file Http2Codec.h
 * @brief 不执行网络 I/O 的 HTTP/2 协议引擎。
 */

#pragma once

#ifdef HICAL_HAS_HTTP2

	#include "core/Error.h"
	#include "core/Http2Options.h"
	#include <cstddef>
	#include <cstdint>
	#include <limits>
	#include <memory>
	#include <span>
	#include <string>
	#include <string_view>

namespace hical::detail
{

	/**
	 * @brief RFC 9113 的错误码，与具体协议库分开。
	 */
	enum class Http2Code : uint32_t
	{
		hNoError = 0,
		hProtocolError = 1,
		hInternalError = 2,
		hFlowControlError = 3,
		hSettingsTimeout = 4,
		hStreamClosed = 5,
		hFrameSizeError = 6,
		hRefusedStream = 7,
		hCancel = 8,
		hCompressionError = 9,
		hConnectError = 10,
		hEnhanceYourCalm = 11,
		hInadequateSecurity = 12,
		hHttp11Required = 13
	};

	/**
	 * @brief SETTINGS 标识；MAX_CONCURRENT_STREAMS 约束发送者允许的入站流。
	 */
	enum class Http2Setting : uint16_t
	{
		hHeaderTableSize = 1,
		hEnablePush = 2,
		hMaxConcurrentStreams = 3,
		hInitialWindowSize = 4,
		hMaxFrameSize = 5,
		hMaxHeaderListSize = 6
	};

	/**
	 * @brief 单线程串行使用的协议状态；监听器及 codec 必须活到正在执行的调用结束。
	 * 不允许从回调重入 memRecv/memSend；可以提交响应、复位或补窗。
	 * 本类不创建协程，也不保存请求级 PMR 或调用方的头字段视图。
	 */
	class Http2Codec
	{
	public:
		enum class Role : uint8_t
		{
			hClient,
			hServer
		};
		enum class HeaderKind : uint8_t
		{
			hRequest,
			hResponse,
			hTrailers
		};
		enum class FrameType : uint8_t
		{
			hData,
			hHeaders,
			hPriority,
			hRstStream,
			hSettings,
			hPushPromise,
			hPing,
			hGoaway,
			hWindowUpdate,
			hContinuation
		};
		static constexpr uint8_t kEndStream = 0x01;
		static constexpr uint8_t kAck = 0x01;

		struct Header
		{
			std::string_view name;
			std::string_view value;
		};

		struct DataRead
		{
			size_t bytes {0};
			bool eof {false};
			bool deferred {false};
			NetworkError error;
		};

		/**
		 * @brief 仅复制已就绪的 DATA，不得在 read 中执行 socket/文件 I/O 或重入收发泵。
		 * codec 保活到协议流关闭；回调抛异常会被 C 边界截住并使连接失败。
		 */
		class DataSource
		{
		public:
			virtual ~DataSource() = default;
			/**
			 * @brief 读取已拥有的字节，零字节且未结束时必须标记 deferred。
			 * @param output 供本次复制的缓冲；返回字节数不得超过容量
			 * @return 已复制数量、EOF、暂不可读或流错误。
			 */
			virtual DataRead read(std::span<char> output) = 0;
			/**
			 * @brief EOF 时取得已封存尾字段，codec 在 read 返回后同步复制。
			 * @return 不含伪头的稳定尾字段；为空时直接结束 DATA。
			 */
			virtual std::span<const Header> trailers() const noexcept;
			/**
			 * @brief 协议流关闭，迟到生产者必须停止输出。
			 * @param code 正常结束或 RST 错误码
			 */
			virtual void onClose(Http2Code code);
		};

		struct Setting
		{
			Http2Setting id;
			uint32_t value;
		};

		struct Frame
		{
			FrameType type;
			uint8_t flags;
			int32_t streamId;
			size_t length;
		};

		struct HeaderBlock
		{
			int32_t streamId;
			HeaderKind kind;
			uint16_t statusCode;
			bool endStream;
		};

		struct ReceiveResult
		{
			size_t consumed {0};
			NetworkError error;
		};

		struct SendResult
		{
			std::string bytes;
			NetworkError error;
		};

		struct SubmitResult
		{
			int32_t streamId {0};
			NetworkError error;
		};

		/**
		 * @brief 所有视图仅在回调期间有效，保留数据的监听器应立即复制到受预算约束的存储。
		 * 头/DATA 回调返回非 NO_ERROR 时拒流；拒绝的 DATA 不得已存储或已 consume。
		 * 回调可以抛异常，codec 会截住并将本次收发判为连接失败。
		 */
		class Listener
		{
		public:
			virtual ~Listener() = default;
			/**
			 * @brief 开始新头部块
			 * @param streamId 流
			 * @return 接受或拒流错误码。
			 */
			virtual Http2Code onBeginHeaders(int32_t streamId);
			/**
			 * @brief 接收单个字段
			 * @param streamId 流
			 * @param header 字段视图
			 * @return 接受或拒流错误码。
			 */
			virtual Http2Code onHeader(int32_t streamId, Header header);
			/**
			 * @brief 收齐头部块
			 * @param block 包含 1xx 状态码及最终头/尾头类别
			 * @return 接受或拒流错误码。
			 */
			virtual Http2Code onEndHeaders(const HeaderBlock& block);
			/**
			 * @brief 接收数据
			 * @param streamId 流
			 * @param bytes 借用数据
			 * @return 接受或拒流错误码。
			 */
			virtual Http2Code onData(int32_t streamId, std::string_view bytes);
			/**
			 * @brief 该流入站消息结束
			 * @param streamId 流。
			 */
			virtual void onEndStream(int32_t streamId);
			/**
			 * @brief 协议流关闭，应用任务可仍在运行
			 * @param streamId 流
			 * @param code 错误码。
			 */
			virtual void onStreamClose(int32_t streamId, Http2Code code);
			/**
			 * @brief 通知原始头块的开始或完整接收；包含已拒绝后仍需解压同步的头块。
			 * @param streamId 帧头中的流 ID，尚未替代协议库的语义校验
			 * @param complete false 表示九字节 HEADERS 帧头已齐，true 表示 END_HEADERS 载荷已消费
			 */
			virtual void onHeaderBlock(int32_t streamId, bool complete);
			/**
			 * @brief 处理完入站帧
			 * @param frame 帧元数据，可用 WINDOW_UPDATE 唤醒生产者。
			 */
			virtual void onFrameReceived(const Frame& frame);
			/**
			 * @brief 帧已序列化，连接层还须等待 writer 确认
			 * @param frame 含 END_STREAM 的元数据。
			 */
			virtual void onFrameSent(const Frame& frame);
			/**
			 * @brief 收到 SETTINGS 或其 ACK
			 * @param ack 是否确认本端设置，可直接查询当前对端设置。
			 */
			virtual void onSettings(bool ack);
			/**
			 * @brief 收到 GOAWAY
			 * @param lastStreamId 对端最后处理的流
			 * @param code 错误码
			 * @param debugData 借用诊断数据。
			 */
			virtual void onGoaway(int32_t lastStreamId, Http2Code code, std::string_view debugData);
			/**
			 * @brief 实际 RST/流关闭或 GOAWAY 的错误
			 * @param error 流与连接分类。
			 */
			virtual void onError(const NetworkError& error);
		};

		/**
		 * @brief 创建协议会话并排队初始 SETTINGS，非法配置或分配失败会抛异常。
		 * @param role 客户端或服务端；客户端始终禁止 PUSH。
		 * @param options 流、头及输出预算，构造时复制。
		 * @param listener 借用监听器，可为空；其寿命必须覆盖回调。
		 */
		explicit Http2Codec(Role role, const Http2Options& options = {}, Listener* listener = nullptr);
		~Http2Codec();
		Http2Codec(const Http2Codec&) = delete;
		Http2Codec& operator=(const Http2Codec&) = delete;
		Http2Codec(Http2Codec&&) = delete;
		Http2Codec& operator=(Http2Codec&&) = delete;

		/**
		 * @brief 在收发调用之外替换监听器
		 * @param listener 借用对象
		 * @return 成功或重入错误。
		 */
		NetworkError setListener(Listener* listener);
		/**
		 * @brief 处理入站字节，不保留输入视图
		 * @param bytes 输入
		 * @return 已消耗长度及致命连接错误。
		 */
		ReceiveResult memRecv(std::string_view bytes);
		/**
		 * @brief 返回大小受限的自有输出缓冲；超出本批的字节暂存，并计入连接预算。
		 * @param maxBytes 本次最多返回的字节数，另受 maxOutputBatchBytes 限制；零表示不推进。
		 * @param storageBudget 连接待发预算扣除外部队列后的剩余额度，包含 codec 当前余留字节。
		 * @return 自有输出缓冲及错误；协议库单块超出连接预算时在复制前失败。
		 */
		SendResult memSend(size_t maxBytes = Http2Options::kDefaultOutputBatchBytes,
						   size_t storageBudget = std::numeric_limits<size_t>::max());
		/**
		 * @brief 查询已拥有且尚未返回的输出
		 * @return 应计入连接待发预算的字节数。
		 */
		[[nodiscard]] size_t bufferedOutputBytes() const noexcept;
		/**
		 * @brief 提交请求头，调用期间复制字段
		 * @param headers 字段
		 * @param endStream 是否结束
		 * @return 流 ID 或错误。
		 */
		SubmitResult submitRequest(std::span<const Header> headers, bool endStream = true);
		/**
		 * @brief 提交请求并持有其 DATA 源。
		 * @param headers 调用期间复制的初始头
		 * @param source 非空 DATA 源，codec 保活到协议流关闭
		 * @return 新流 ID 或错误。
		 */
		SubmitResult submitRequest(std::span<const Header> headers, std::shared_ptr<DataSource> source);
		/**
		 * @brief 提交最终或 1xx 响应
		 * @param streamId 流
		 * @param headers 字段
		 * @param endStream 是否结束
		 * @return 错误。
		 */
		NetworkError submitResponse(int32_t streamId, std::span<const Header> headers, bool endStream = true);
		/**
		 * @brief 提交响应并持有其 DATA 源。
		 * @param streamId 对应请求流
		 * @param headers 调用期间复制的最终响应头
		 * @param source 非空 DATA 源，codec 保活到协议流关闭
		 * @return 提交错误。
		 */
		NetworkError submitResponse(int32_t streamId,
									std::span<const Header> headers,
									std::shared_ptr<DataSource> source);
		/**
		 * @brief 提交尾头并结束发送
		 * @param streamId 流
		 * @param headers 字段
		 * @return 错误。
		 */
		NetworkError submitTrailers(int32_t streamId, std::span<const Header> headers);
		/**
		 * @brief 提交新 SETTINGS
		 * @param settings 设置
		 * @return 配置或协议错误。
		 */
		NetworkError submitSettings(std::span<const Setting> settings);
		/**
		 * @brief 复位单流
		 * @param streamId 流
		 * @param code RFC 错误码
		 * @return 错误。
		 */
		NetworkError reset(int32_t streamId, Http2Code code = Http2Code::hCancel);
		/**
		 * @brief 提交最终 GOAWAY
		 * @param lastStreamId 最后接纳的流
		 * @param code RFC 错误码
		 * @return 错误。
		 */
		NetworkError goaway(int32_t lastStreamId, Http2Code code = Http2Code::hNoError);
		/**
		 * @brief 归还接收额度
		 * @param streamId 流，零表示只还连接额度
		 * @param bytes 已消费字节
		 * @return 错误。
		 */
		NetworkError consume(int32_t streamId, size_t bytes);
		/**
		 * @brief 恢复已延迟 DATA 的生产
		 * @param streamId 流
		 * @return 错误。
		 */
		NetworkError resume(int32_t streamId);
		/**
		 * @brief 终止输出并释放 codec 暂存的报文字节，之后收发泵返回取消错误。
		 * 不影响外部正在写出的拥有型批次；仅在所属 loop 使用。
		 */
		void abortOutput();
		/**
		 * @brief 查询会话角色。
		 * @return 构造时的客户端或服务端角色。
		 */
		[[nodiscard]] Role role() const noexcept;
		/**
		 * @brief 查询协议是否仍需读取
		 * @return 是否可继续接收入站字节。
		 */
		[[nodiscard]] bool wantRead() const noexcept;
		/**
		 * @brief 查询包含本地余留字节的输出
		 * @return 是否还有输出工作。
		 */
		[[nodiscard]] bool wantWrite() const noexcept;
		/**
		 * @brief 查询对端设置
		 * @param setting 设置标识
		 * @return 当前设置值。
		 */
		[[nodiscard]] uint32_t peerSetting(Http2Setting setting) const noexcept;
		/**
		 * @brief 查询对端允许本端发起的并发流上限
		 * @return 流容量，零表示暂停新流。
		 */
		[[nodiscard]] uint32_t peerMaxConcurrentStreams() const noexcept;
		/**
		 * @brief 查询已真正打开的本端请求流，包含 DATA 和仅头请求，不包含排队未打开的请求。
		 * @return 尚未协议关闭的本端流数；服务端为零。
		 */
		[[nodiscard]] size_t activeLocalStreamCount() const noexcept;
		/**
		 * @brief 查询可发送窗口
		 * @param streamId 流，零表示连接
		 * @return 窗口，流不存在为 -1。
		 */
		[[nodiscard]] int32_t peerWindow(int32_t streamId = 0) const noexcept;
		/**
		 * @brief 查询是否允许再排队请求，流槽位仍由上层计算
		 * @return GOAWAY 等状态是否允许新请求。
		 */
		[[nodiscard]] bool requestAllowed() const noexcept;

	private:
		struct Impl;
		std::unique_ptr<Impl> impl_;
	};

} // namespace hical::detail

#endif
