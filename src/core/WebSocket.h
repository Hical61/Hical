/**
 * @file WebSocket.h
 * @brief WebSocket 会话管理与消息收发
 */

#pragma once

#include "Coroutine.h"
#include "WsDeflate.h"
#include "WsFrame.h"
#include <boost/asio.hpp>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace hical
{

	/**
	 * @brief WebSocket 压缩配置
	 * 由 WsRoute 传入，用于配置 permessage-deflate。
	 */
	struct WsCompressionConfig
	{
		bool enabled = false;
		int serverMaxWindowBits = 15;
		int clientMaxWindowBits = 15;
		bool serverNoContextTakeover = false;
	};

	struct WsDeflateNegotiation; // 前向声明，定义在 WsHandshake.h

	/**
	 * @brief WebSocket 消息（含类型信息）
	 * 用于区分 Text/Binary 消息类型。
	 */
	struct WsMessage
	{
		WsOpcode type = WsOpcode::hText; ///< hText 或 hBinary
		std::string data;
	};

	/**
	 * @brief WebSocket 会话封装
	 * 对原始 TCP socket 的 WebSocket 协议封装（RFC 6455）。
	 * 提供协程化的 send/receive 接口，自研帧解析/构造。
	 * 写侧合批：消息循环处理同一批读入时（beginBatch() 到 endBatch() 之间），
	 * send()/sendBinary()/sendPing() 生成的帧先攒进连接级出站缓冲，一批回调跑完
	 * 才落地一次 async_write。所以这段区间的 co_await send() 语义是「已入队」，
	 * 不是「已经写到线上」：协程一返回只代表帧进了缓冲，不代表数据出了内核。
	 * 背压影响：对端不收数据时，以前 send() 会立刻卡在 async_write 上，现在要等
	 * 缓冲攒到 kWriteBatchBytes 才卡第一次，也就是说「发不出去」这个信号滞后一批
	 * 才传到业务协程，期间内存占用最多多出一个批次的量。要确保某个响应已经落地
	 * （比如自己关 socket、或者在批末发关闭帧），先 co_await flushBatch()。
	 * 合批窗口只包住回调执行期，等数据那段不合批——所以心跳 Ping、WsHub 广播
	 * 平时照旧立刻发，只有正好落在某个批窗口里的会跟着排到批末。
	 */
	class WebSocketSession
	{
	public:
		// 默认最大消息大小 1MB，防止恶意客户端发送超大帧导致 OOM
		static constexpr size_t hDefaultMaxMessageSize = 1024 * 1024;

		// 出站缓冲上限：攒到这个量就提前落盘，免得客户端灌数据时内存无限涨
		static constexpr size_t kWriteBatchBytes = 64 * 1024;

		// 一批读入最多解这么多帧就得把线程让出去。8KB 读缓冲塞满 7 字节的小帧
		// 就是上千条消息，不设上限的话一条连接能在同一个协程里连着跑完上千次
		// 用户回调，同 worker 上别的连接全跟着堵。剩下的字节留在读缓冲里，
		// 下一轮接着解，不影响任何一帧的完整性
		static constexpr size_t kMaxFramesPerBatch = 256;

		/**
		 * @brief 从已完成 WebSocket 握手的 TCP socket 构造
		 * @param socket      已完成握手的 TCP socket
		 * @param maxMessageSize 最大消息大小（字节，默认 1MB）
		 * @param compression 压缩配置
		 * @param deflateNeg  permessage-deflate 协商结果（nullptr 时不启用压缩）
		 */
		explicit WebSocketSession(boost::asio::ip::tcp::socket socket,
								  size_t maxMessageSize = hDefaultMaxMessageSize,
								  WsCompressionConfig compression = {},
								  const WsDeflateNegotiation* deflateNeg = nullptr);

		/**
		 * @brief 发送文本消息
		 * @param msg 消息内容
		 */
		Awaitable<void> send(const std::string& msg);

		/**
		 * @brief 发送二进制消息（WsOpcode::hBinary）
		 * @param data 二进制数据
		 */
		Awaitable<void> sendBinary(std::string_view data);

		/**
		 * @brief 发送 Ping 帧
		 * @param payload Ping 载荷（最大 125 字节，默认空）
		 */
		Awaitable<void> sendPing(std::string_view payload = {});

		/**
		 * @brief 接收消息（仅返回文本内容，向后兼容）
		 * @return 接收到的消息内容，连接关闭时返回 std::nullopt
		 */
		Awaitable<std::optional<std::string>> receive();

		/**
		 * @brief 接收带类型的消息（区分 Text/Binary）
		 * @return WsMessage 含 type 和 data，连接关闭时返回 std::nullopt
		 */
		Awaitable<std::optional<WsMessage>> receiveMessage();

		/**
		 * @brief 批量接收：把读缓冲区里能完整解出来的消息一次全拿走
		 * 缓冲区里还有完整帧就继续解，碰到半帧或者解够 kMaxFramesPerBatch 帧就返回，
		 * 不会为了凑数多读 socket。解够上限时剩下的字节留在读缓冲里，下一轮接着解。
		 * 控制帧（ping/pong/close）就地处理：pong 回帧按位置排进同批出站缓冲，
		 * close 回帧排到批末（endBatch() 时补发），保证不影响分片重组。
		 * @param out 输出消息列表（进入时清空）
		 * @return true 连接还活着；false 表示连接已经结束（close 帧 / EOF / 协议错误）
		 */
		Awaitable<bool> receiveBatch(std::vector<WsMessage>& out);

		/**
		 * @brief 进入合批写模式（框架内部使用）
		 * 之后的 send() 系列只入队不出网，直到 endBatch() 或缓冲超过上限。
		 */
		void beginBatch();

		/**
		 * @brief 退出合批写模式并把缓冲落盘（框架内部使用）
		 * 批内响应先发，close 回帧最后发；写失败只标记连接关闭，不往上层抛。
		 */
		Awaitable<void> endBatch();

		/**
		 * @brief 合批缓冲里还有没有待发的东西
		 * endBatch() 是协程，光调一下就得分协程帧，空判断塞函数体里救不了这次分配，
		 * 所以调用点先问一句。读写都跑在同一条 executor 线程上，不加锁。
		 * @return true 表示攒着待发帧，或者 close 回帧还等着批末补发
		 */
		[[nodiscard]] bool hasPendingBatchWork() const noexcept
		{
			return !writeBatch_.empty() || closeReplyPending_;
		}

		/**
		 * @brief 立刻把出站缓冲发出去（不退出合批模式）
		 */
		Awaitable<void> flushBatch();

		/**
		 * @brief 协程式关闭 WebSocket 连接（默认 Normal Closure）
		 */
		Awaitable<void> closeAsync();

		/**
		 * @brief 协程式关闭 WebSocket 连接（自定义关闭码）
		 * @param code 关闭码（RFC 6455 §7.4.1）
		 * @param reason 关闭原因（最大 123 字节）
		 */
		Awaitable<void> closeAsync(WsCloseCode code, std::string_view reason = {});

		/**
		 * @brief 同步关闭 WebSocket 连接
		 * @warning 不发送 close 帧，直接关闭底层 socket。
		 * 推荐使用 closeAsync() 替代。
		 */
		void close();

		/**
		 * @brief 连接是否仍然打开
		 * @return true 如果连接打开
		 */
		[[nodiscard]] bool isOpen() const;

		/**
		 * @brief 获取底层 TCP socket 引用
		 * 用于超时场景关闭底层 socket 以中断 async_read。
		 * @return TCP socket 引用
		 */
		[[nodiscard]] boost::asio::ip::tcp::socket& socket();

		/**
		 * @brief 获取压缩配置
		 */
		[[nodiscard]] const WsCompressionConfig& compressionConfig() const;

		// ============ 连接上下文 ============

		/**
		 * @brief 设置用户自定义上下文（类型擦除）
		 * @param ctx 任意类型的 shared_ptr
		 */
		void setContext(std::shared_ptr<void> ctx);

		/**
		 * @brief 获取用户自定义上下文
		 * @tparam T 原始类型
		 * @return static_pointer_cast 后的 shared_ptr
		 */
		template <typename T>
		[[nodiscard]] std::shared_ptr<T> getContext() const
		{
			return std::static_pointer_cast<T>(context_);
		}

		/// 检查是否已设置上下文
		[[nodiscard]] bool hasContext() const;

		/// 清除上下文
		void clearContext();

		// ============ 心跳支持 ============

		/// 获取最后收到 Pong 的时间点
		[[nodiscard]] std::chrono::steady_clock::time_point lastPongTime() const;

		// ============ 子协议 ============

		/// 获取协商的子协议（空 = 未协商）
		[[nodiscard]] std::string_view subprotocol() const;

		/// 已发起的 async_write 次数（合批效果诊断 / 测试用）
		[[nodiscard]] size_t writeCount() const;

		/// 设置子协议（框架内部使用）
		void setSubprotocol(std::string proto);

	private:
		/// 接收消息的内核实现（receive/receiveMessage 共用）
		Awaitable<std::optional<WsMessage>> receiveInternal();

		/// 记录收到 Pong（由 receiveInternal() 内部调用）
		void recordPongReceived();

		/// 确保读缓冲区中至少有 n 字节可用数据
		Awaitable<void> ensureBytes(size_t n);

		/// 丢弃读缓冲区前 n 字节（memmove 剩余数据）
		void consumeBytes(size_t n);

		/// 构造并发送 WebSocket 帧
		Awaitable<void> sendFrame(WsOpcode opcode, std::string_view payload, bool fin = true, bool rsv1 = false);

		/// 发送 close 帧的便捷方法
		Awaitable<void> sendCloseFrame(WsCloseCode code, std::string_view reason = {});

		/// 已持有写锁时把出站缓冲落盘（写失败就地吞掉，只标记连接关闭）
		Awaitable<void> writeBatchLocked();

		/// 把一帧追加编码到 dst（纯内存操作，共用缓冲的调用方自己拿写锁）
		static void encodeFrame(std::string& dst, WsOpcode opcode, std::string_view payload, bool fin, bool rsv1);

		/// 已持有写锁时把帧追加到出站缓冲，超过上限就地落盘
		Awaitable<void> queueFrameLocked(WsOpcode opcode, std::string_view payload, bool fin, bool rsv1);

		/// 把控制帧攒进出站缓冲（解帧阶段用，不跟着 batching_ 走）
		/// payload 是视图：本函数内部有挂起点，它必须活到协程返回
		Awaitable<void> queueFrame(WsOpcode opcode, std::string_view payload);

		/// 记下 close 回帧，等批内响应发完再补发（不能插队）
		Awaitable<void> replyClose(std::string_view payload);

		/// 写锁 RAII：异常/取消路径也要 releaseWrite()，否则写锁会永久卡死
		struct WriteLock
		{
			WebSocketSession& self;

			~WriteLock()
			{
				self.releaseWrite();
			}
		};

		boost::asio::ip::tcp::socket socket_;
		std::atomic<bool> open_ {true};
		WsCompressionConfig compression_;
		size_t maxMessageSize_;

		// 连接级读缓冲区（跨 receive() 调用复用）
		std::vector<uint8_t> readBuf_;
		size_t readBufUsed_ = 0;

		// 分片消息重组
		std::string fragmentBuf_;
		WsOpcode fragmentOpcode_ = WsOpcode::hText;
		bool fragmentCompressed_ = false; ///< 当前分片消息的首帧是否带 RSV1（压缩）

		// 帧发送复用缓冲，省掉每帧一次的 std::string 堆分配
		std::string frameBuf_;

		// 实际发起的 async_write 次数，用来确认合批有没有生效（诊断/测试用）
		size_t writeCount_ = 0;

		// 合批写：批内帧先攒进出站缓冲，一批处理完再落一次 async_write
		std::string writeBatch_;         ///< 出站累积缓冲（合批模式下追加）
		bool batching_ = false;          ///< 合批写模式开关
		std::string closeReplyBuf_;      ///< 批内待补发的 close 回帧载荷
		bool closeReplyPending_ = false; ///< close 回帧是否还等着批末补发

		// 单条 API（receive/receiveMessage）的剩帧：一次 drain 出多条时只交第一条
		std::vector<WsMessage> pending_;
		size_t pendingPos_ = 0;

		// permessage-deflate 上下文（仅压缩启用时构造）
		std::unique_ptr<WsDeflateContext> deflateCtx_;

		// 写串行化：防止心跳 Ping 与消息发送并发 async_write
		bool writePending_ = false;
		size_t waiters_ = 0; ///< 正在写锁上排队的协程数（单连接跑在单线程上，不用原子）
		std::unique_ptr<boost::asio::steady_timer> writeReady_;

		/// 获取写权限（等待前一个 async_write 完成）
		Awaitable<void> acquireWrite();

		/// 释放写权限（唤醒等待的协程）
		void releaseWrite();

		// 连接上下文（Feature 6）
		std::shared_ptr<void> context_;

		// 心跳 Pong 时间戳（Feature 1）
		std::chrono::steady_clock::time_point lastPongTime_ {std::chrono::steady_clock::now()};

		// 协商的子协议（Feature 5）
		std::string subprotocol_;
	};

} // namespace hical
