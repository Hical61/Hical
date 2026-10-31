/**
 * @file Http2Receive.cpp
 * @brief HTTP/2 接收头块、DATA 存储准入与逐流结束校验。
 */

#include "Http2Receive.h"

#ifdef HICAL_HAS_HTTP2

	#include <stdexcept>
	#include <utility>

namespace hical::detail
{
	namespace
	{
		NetworkError streamError(int32_t id, Http2Code code, std::string message)
		{
			return {code == Http2Code::hEnhanceYourCalm ? ErrorCode::hResourceLimitExceeded
														: ErrorCode::hHttp2StreamError,
					std::move(message),
					Http2ErrorInfo {static_cast<uint32_t>(code), id}};
		}

		Http2Code rejectionCode(const NetworkError& error)
		{
			if (error.http2 && error.http2->code != static_cast<uint32_t>(Http2Code::hNoError))
			{
				return static_cast<Http2Code>(error.http2->code);
			}
			return error.code == ErrorCode::hResourceLimitExceeded ? Http2Code::hEnhanceYourCalm : Http2Code::hCancel;
		}
	} // namespace

	Http2Receive::Http2Receive(boost::asio::any_io_executor executor,
							   Http2Codec& codec,
							   Http2Budget budget,
							   Config config,
							   Callbacks callbacks)
		: executor_(std::move(executor))
		, codec_(codec)
		, budget_(std::move(budget))
		, config_(std::move(config))
		, callbacks_(std::move(callbacks))
	{
		config_.options.validate();
		if (config_.role != Http2Codec::Role::hServer && config_.role != Http2Codec::Role::hClient)
		{
			throw std::invalid_argument("HTTP/2 接收角色无效");
		}
	}

	Http2Receive::~Http2Receive()
	{
		fail({ErrorCode::hOperationAborted, "HTTP/2 接收适配器已停止"});
	}

	NetworkError Http2Receive::addClientStream(std::shared_ptr<Http2Stream> stream, ResponsePolicy policy)
	{
		if (connectionError_)
		{
			return connectionError_;
		}
		if (config_.role != Http2Codec::Role::hClient || !stream || stream->id() <= 0 || stream->id() % 2 == 0
			|| !stream->protocolOpen() || stream->initialHeaders().sealed() || streams_.contains(stream->id()))
		{
			return {ErrorCode::hInvalidConfiguration, "HTTP/2 客户端接收流无效或已登记"};
		}
		Entry entry;
		entry.stream = std::move(stream);
		entry.headResponse = policy.headResponse;
		auto id = entry.stream->id();
		streams_.emplace(id, std::move(entry));
		return {};
	}

	std::shared_ptr<Http2Stream> Http2Receive::find(int32_t streamId) const
	{
		auto found = streams_.find(streamId);
		return found == streams_.end() ? nullptr : found->second.stream;
	}

	size_t Http2Receive::size() const noexcept
	{
		return streams_.size();
	}

	Http2Code Http2Receive::onBeginHeaders(int32_t streamId)
	{
		if (connectionError_)
		{
			return Http2Code::hInternalError;
		}
		auto found = streams_.find(streamId);
		if (found == streams_.end())
		{
			if (config_.role != Http2Codec::Role::hServer || streamId <= lastServerStreamId_ || streamId % 2 == 0)
			{
				return Http2Code::hStreamClosed;
			}
			lastServerStreamId_ = streamId;
			Http2Stream::Config streamConfig;
			streamConfig.streamId = streamId;
			streamConfig.incomingKind = Http2Codec::HeaderKind::hRequest;
			streamConfig.options = config_.options;
			auto stream = Http2Stream::create(executor_, streamConfig, budget_);
			if (!stream)
			{
				return Http2Code::hRefusedStream;
			}
			Entry entry;
			entry.stream = std::move(stream);
			found = streams_.emplace(streamId, std::move(entry)).first;
		}
		auto& entry = found->second;
		if (!entry.stream->protocolOpen())
		{
			return reject(streamId, entry.stream->error());
		}
		if (entry.pendingHeaders || entry.stream->bodyComplete() || entry.stream->trailers().sealed())
		{
			return reject(streamId, streamError(streamId, Http2Code::hProtocolError, "HTTP/2 头部块顺序错误"));
		}
		entry.pendingKind = entry.stream->initialHeaders().sealed()     ? Http2Codec::HeaderKind::hTrailers
							: config_.role == Http2Codec::Role::hServer ? Http2Codec::HeaderKind::hRequest
																		: Http2Codec::HeaderKind::hResponse;
		entry.pendingHeaders = std::make_unique<Http2Headers>(entry.pendingKind, streamId, config_.options);
		return Http2Code::hNoError;
	}

	Http2Code Http2Receive::onHeader(int32_t streamId, Http2Codec::Header header)
	{
		auto found = streams_.find(streamId);
		if (found == streams_.end())
		{
			return Http2Code::hStreamClosed;
		}
		auto& entry = found->second;
		if (!entry.pendingHeaders)
		{
			return reject(streamId, streamError(streamId, Http2Code::hProtocolError, "HTTP/2 头字段没有对应头块"));
		}
		if (auto error = entry.pendingHeaders->append(header.name, header.value))
		{
			return reject(streamId, std::move(error));
		}
		return Http2Code::hNoError;
	}

	Http2Code Http2Receive::finishInitial(Entry& entry, const Http2Codec::HeaderBlock& block)
	{
		auto stream = entry.stream;
		auto length = entry.pendingHeaders->contentLength();
		const bool response = config_.role == Http2Codec::Role::hClient;
		entry.bodyForbidden = response && (entry.headResponse || block.statusCode == 204 || block.statusCode == 304);
		if (response && block.statusCode == 204 && length)
		{
			return reject(block.streamId,
						  streamError(block.streamId, Http2Code::hProtocolError, "HTTP/2 204 响应禁止 Content-Length"));
		}
		if (!entry.bodyForbidden)
		{
			const auto limit = response ? config_.options.maxResponseBytes : config_.options.maxBodyBytes;
			if (length && *length > limit)
			{
				return reject(
					block.streamId,
					streamError(block.streamId, Http2Code::hEnhanceYourCalm, "HTTP/2 声明的消息长度超过单流接收预算"));
			}
			entry.expectedLength = length;
		}
		stream->initialHeaders() = std::move(*entry.pendingHeaders);
		entry.pendingHeaders.reset();
		if (callbacks_.onHeadersReady)
		{
			callbacks_.onHeadersReady(stream);
		}
		// 用户通知可同步取消或终结连接，不能在通知后继续访问原 map 节点。
		return stream->error() ? rejectionCode(stream->error()) : Http2Code::hNoError;
	}

	Http2Code Http2Receive::onEndHeaders(const Http2Codec::HeaderBlock& block)
	{
		auto found = streams_.find(block.streamId);
		if (found == streams_.end())
		{
			return Http2Code::hStreamClosed;
		}
		auto& entry = found->second;
		if (!entry.pendingHeaders || entry.pendingKind != block.kind)
		{
			return reject(block.streamId,
						  streamError(block.streamId, Http2Code::hProtocolError, "HTTP/2 头部块角色与流状态不一致"));
		}
		if (auto error = entry.pendingHeaders->seal())
		{
			return reject(block.streamId, std::move(error));
		}
		if (block.kind == Http2Codec::HeaderKind::hTrailers)
		{
			if (!block.endStream)
			{
				return reject(block.streamId,
							  streamError(block.streamId, Http2Code::hProtocolError, "HTTP/2 尾头必须结束入站消息"));
			}
			entry.stream->trailers() = std::move(*entry.pendingHeaders);
			entry.pendingHeaders.reset();
			return Http2Code::hNoError;
		}
		if (block.kind == Http2Codec::HeaderKind::hResponse)
		{
			if (entry.pendingHeaders->status() != block.statusCode)
			{
				return reject(
					block.streamId,
					streamError(block.streamId, Http2Code::hProtocolError, "HTTP/2 响应状态与头块元数据不一致"));
			}
			if (block.statusCode < 200)
			{
				if (block.endStream)
				{
					return reject(block.streamId,
								  streamError(block.streamId, Http2Code::hProtocolError, "HTTP/2 临时响应不能结束流"));
				}
				auto stream = entry.stream;
				auto headers = std::move(entry.pendingHeaders);
				if (callbacks_.onInformationalHeaders)
				{
					callbacks_.onInformationalHeaders(stream, *headers);
				}
				return stream->error() ? rejectionCode(stream->error()) : Http2Code::hNoError;
			}
		}
		return finishInitial(entry, block);
	}

	Http2Code Http2Receive::onData(int32_t streamId, std::string_view bytes)
	{
		auto found = streams_.find(streamId);
		if (found == streams_.end())
		{
			return Http2Code::hStreamClosed;
		}
		auto& entry = found->second;
		auto stream = entry.stream;
		if (!stream->protocolOpen())
		{
			return reject(streamId, stream->error());
		}
		if (!stream->initialHeaders().sealed() || entry.pendingHeaders || stream->trailers().sealed()
			|| stream->bodyComplete() || (entry.bodyForbidden && !bytes.empty()))
		{
			return reject(streamId,
						  streamError(streamId, Http2Code::hProtocolError, "HTTP/2 DATA 与消息状态或无正文语义不一致"));
		}
		if (entry.expectedLength
			&& (stream->receivedBytes() > *entry.expectedLength
				|| bytes.size() > *entry.expectedLength - stream->receivedBytes()))
		{
			return reject(streamId,
						  streamError(streamId, Http2Code::hProtocolError, "HTTP/2 DATA 超过 Content-Length"));
		}
		if (auto error = stream->appendBody(bytes))
		{
			return reject(streamId, std::move(error));
		}
		// 数据已存入流缓冲并占用预算。补窗失败应终止连接，不能再按拒流路径重复归还额度。
		checkOperation(codec_.consume(streamId, bytes.size()));
		return Http2Code::hNoError;
	}

	void Http2Receive::onEndStream(int32_t streamId)
	{
		auto found = streams_.find(streamId);
		if (found == streams_.end())
		{
			return;
		}
		auto& entry = found->second;
		auto stream = entry.stream;
		NetworkError error;
		if (!stream->protocolOpen())
		{
			error = stream->error();
		}
		else if (!stream->initialHeaders().sealed() || entry.pendingHeaders
				 || (entry.expectedLength && stream->receivedBytes() != *entry.expectedLength))
		{
			error = streamError(streamId, Http2Code::hProtocolError, "HTTP/2 消息结束时头部或 Content-Length 不完整");
		}
		else
		{
			error = stream->completeBody();
		}
		if (error)
		{
			auto code = reject(streamId, std::move(error));
			checkOperation(codec_.reset(streamId, code));
		}
	}

	void Http2Receive::onStreamClose(int32_t streamId, Http2Code code)
	{
		auto found = streams_.find(streamId);
		if (found == streams_.end())
		{
			return;
		}
		auto stream = std::move(found->second.stream);
		streams_.erase(found);
		if (code == Http2Code::hNoError)
		{
			stream->closeProtocol();
		}
		else
		{
			stream->reset(code);
		}
	}

	void Http2Receive::onError(const NetworkError& error)
	{
		if (!error)
		{
			return;
		}
		if (error.http2 && error.http2->streamId > 0)
		{
			reject(error.http2->streamId, error);
		}
		else
		{
			fail(error);
		}
	}

	void Http2Receive::cancel(int32_t streamId, Http2Code code)
	{
		auto stream = find(streamId);
		if (!stream)
		{
			return;
		}
		streams_.erase(streamId);
		stream->reset(code);
		checkOperation(codec_.reset(streamId, code));
	}

	void Http2Receive::fail(NetworkError error)
	{
		if (connectionError_)
		{
			return;
		}
		if (!error)
		{
			error = {ErrorCode::hOperationAborted, "HTTP/2 连接接收已取消"};
		}
		connectionError_ = std::move(error);
		// 先整体移出表，唤醒后的任务即使同步访问连接也不会使当前遍历失效。
		decltype(streams_) snapshot;
		snapshot.swap(streams_);
		for (auto& [id, entry] : snapshot)
		{
			entry.stream->fail(connectionError_);
		}
	}

	Http2Code Http2Receive::reject(int32_t streamId, NetworkError error)
	{
		auto code = rejectionCode(error);
		auto found = streams_.find(streamId);
		if (found != streams_.end())
		{
			auto stream = std::move(found->second.stream);
			streams_.erase(found);
			stream->fail(std::move(error));
		}
		return code;
	}

	void Http2Receive::checkOperation(NetworkError error)
	{
		if (!error)
		{
			return;
		}
		error.code = ErrorCode::hHttp2ConnectionError;
		error.http2 = Http2ErrorInfo {static_cast<uint32_t>(Http2Code::hInternalError), 0};
		fail(error);
		throw std::runtime_error(error.message);
	}
} // namespace hical::detail

#endif
