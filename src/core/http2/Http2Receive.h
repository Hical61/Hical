/**
 * @file Http2Receive.h
 * @brief HTTP/2 接收缓冲、body 完成通知和头部事件处理。
 */

#pragma once

#ifdef HICAL_HAS_HTTP2

	#include "Http2Stream.h"
	#include <functional>
	#include <memory>
	#include <optional>
	#include <unordered_map>

namespace hical::detail
{
	/**
	 * @brief 由连接唯一监听器转发事件的接收适配器；所有操作与析构均在所属 loop 串行执行。
	 * 不执行 socket I/O，也不启动业务协程。DATA 先成功保存再补窗，拒流字节由 codec 归还连接额度。
	 * body 收齐只完成门，调用方仍须显式 publishBody 才能向业务映射对象发布。
	 */
	class Http2Receive
	{
	public:
		struct Config
		{
			Http2Codec::Role role {Http2Codec::Role::hServer};
			Http2Options options;
		};

		struct ResponsePolicy
		{
			/** HEAD 的 Content-Length 描述表示长度，不描述入站 DATA 长度。 */
			bool headResponse {false};
		};

		struct Callbacks
		{
			/** 最终初始头已封存，body 可能尚未收齐；回调应只安排独立业务任务。 */
			std::function<void(std::shared_ptr<Http2Stream>)> onHeadersReady;
			/** 多次 1xx 各自通知；头存储只在本次回调有效，不能保留其中视图。 */
			std::function<void(std::shared_ptr<Http2Stream>, const Http2Headers&)> onInformationalHeaders;
		};

		/**
		 * @brief 创建接收适配器，复制配置并共享连接预算。
		 * @param executor 所属 loop 的执行器
		 * @param codec 借用协议引擎，其生命周期覆盖本对象及事件调用
		 * @param budget 与流共享的连接预算；服务端须已注入共享服务预算
		 * @param config 接收角色和资源配置
		 * @param callbacks 同步头部通知，可为空；异常由 codec 的 C 回调屏障截住
		 */
		Http2Receive(boost::asio::any_io_executor executor,
					 Http2Codec& codec,
					 Http2Budget budget,
					 Config config,
					 Callbacks callbacks = {});
		/**
		 * @brief 结束仍在表内的流并唤醒等待者；应用 guard 持有的流继续自然收尾。
		 */
		~Http2Receive();
		Http2Receive(const Http2Receive&) = delete;
		Http2Receive& operator=(const Http2Receive&) = delete;

		/**
		 * @brief 登记已创建的客户端出站流，必须在接收响应前调用。
		 * @param stream 入站类型为响应、初始头尚未封存的稳定流，使用同一连接预算
		 * @param policy 发出请求的方法对应的无正文策略
		 * @return 成功或重复流、配置及连接错误；失败不接管调用方流
		 */
		NetworkError addClientStream(std::shared_ptr<Http2Stream> stream, ResponsePolicy policy);
		/**
		 * @brief 查询表内流并取得强引用。
		 * @param streamId 流 ID
		 * @return 稳定流拥有者，已拒绝或已协议关闭时为空
		 */
		[[nodiscard]] std::shared_ptr<Http2Stream> find(int32_t streamId) const;
		/**
		 * @brief 查询接收表当前拥有的协议流数。
		 * @return 表内流数量
		 */
		[[nodiscard]] size_t size() const noexcept;
		/**
		 * @brief 开始独立头部块，服务端在首次接纳时预留流和应用额度。
		 * @param streamId 流 ID
		 * @return 接受或拒流错误码
		 */
		Http2Code onBeginHeaders(int32_t streamId);
		/**
		 * @brief 在有界稳定存储中保存一个头字段。
		 * @param streamId 流 ID
		 * @param header 仅本次回调有效的字段视图
		 * @return 接受或拒流错误码
		 */
		Http2Code onHeader(int32_t streamId, Http2Codec::Header header);
		/**
		 * @brief 封存头部块，分别处理 1xx、最终初始头和尾头。
		 * @param block 头块类型、状态及结束标志
		 * @return 接受或拒流错误码
		 */
		Http2Code onEndHeaders(const Http2Codec::HeaderBlock& block);
		/**
		 * @brief DATA 保存成功后归还对应流和连接的接收额度。
		 * @param streamId 流 ID
		 * @param bytes 不含 padding 的应用数据
		 * @return 接受或拒流错误码；拒绝的数据不得由调用方重复 consume
		 */
		Http2Code onData(int32_t streamId, std::string_view bytes);
		/**
		 * @brief 校验消息长度后只完成该流 body 门；失败时提交 RST_STREAM。
		 * @param streamId 入站消息已结束的流 ID
		 */
		void onEndStream(int32_t streamId);
		/**
		 * @brief 从接收表移除已关闭的流，并唤醒仍在等待的任务。
		 * @param streamId 流 ID
		 * @param code 正常关闭或对端复位错误码
		 */
		void onStreamClose(int32_t streamId, Http2Code code);
		/**
		 * @brief 按错误元数据终结对应流或全部流。
		 * @param error 流错误或连接错误；空错误不改变状态
		 */
		void onError(const NetworkError& error);
		/**
		 * @brief 本地取消单流，提交复位并释放接收表持有。
		 * @param streamId 流 ID
		 * @param code 本地复位错误码
		 */
		void cancel(int32_t streamId, Http2Code code = Http2Code::hCancel);
		/**
		 * @brief 连接失败时移出全部流，再逐流失败；保留首个连接错误。
		 * @param error 连接失败原因，空错误按本地取消处理
		 */
		void fail(NetworkError error);

	private:
		struct Entry
		{
			std::shared_ptr<Http2Stream> stream;
			std::unique_ptr<Http2Headers> pendingHeaders;
			Http2Codec::HeaderKind pendingKind {Http2Codec::HeaderKind::hRequest};
			std::optional<uint64_t> expectedLength;
			bool headResponse {false};
			bool bodyForbidden {false};
		};

		Http2Code reject(int32_t streamId, NetworkError error);
		void checkOperation(NetworkError error);
		Http2Code finishInitial(Entry& entry, const Http2Codec::HeaderBlock& block);

		boost::asio::any_io_executor executor_;
		Http2Codec& codec_;
		Http2Budget budget_;
		Config config_;
		Callbacks callbacks_;
		std::unordered_map<int32_t, Entry> streams_;
		NetworkError connectionError_;
		int32_t lastServerStreamId_ {0};
	};
} // namespace hical::detail

#endif
