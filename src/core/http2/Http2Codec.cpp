/**
 * @file Http2Codec.cpp
 * @brief nghttp2 会话的 RAII、逐字段回调和有界内存收发。
 */

#include "Http2Codec.h"

#ifdef HICAL_HAS_HTTP2

	#include <nghttp2/nghttp2.h>
	#include <algorithm>
	#include <array>
	#include <limits>
	#include <stdexcept>
	#include <unordered_map>
	#include <unordered_set>
	#include <utility>
	#include <vector>

namespace hical::detail
{
	namespace
	{
		constexpr size_t kMaxSettings = 32;
		constexpr size_t kFrameHeaderBytes = 9;
		constexpr uint8_t kEndHeaders = 0x04;

		Http2Code wireError(int libraryError) noexcept
		{
			switch (libraryError)
			{
				case NGHTTP2_ERR_STREAM_CLOSED:
					return Http2Code::hStreamClosed;
				case NGHTTP2_ERR_HEADER_COMP:
					return Http2Code::hCompressionError;
				case NGHTTP2_ERR_FRAME_SIZE_ERROR:
					return Http2Code::hFrameSizeError;
				case NGHTTP2_ERR_FLOW_CONTROL:
					return Http2Code::hFlowControlError;
				case NGHTTP2_ERR_REFUSED_STREAM:
					return Http2Code::hRefusedStream;
				case NGHTTP2_ERR_FLOODED:
				case NGHTTP2_ERR_TOO_MANY_CONTINUATIONS:
					return Http2Code::hEnhanceYourCalm;
				case NGHTTP2_ERR_NOMEM:
				case NGHTTP2_ERR_CALLBACK_FAILURE:
					return Http2Code::hInternalError;
				default:
					return Http2Code::hProtocolError;
			}
		}

		NetworkError protocolError(int32_t streamId, Http2Code code)
		{
			return {streamId == 0 ? ErrorCode::hHttp2ConnectionError : ErrorCode::hHttp2StreamError,
					{},
					Http2ErrorInfo {static_cast<uint32_t>(code), streamId}};
		}

		NetworkError operationError(int result, int32_t streamId = 0)
		{
			if (result >= 0)
			{
				return {};
			}
			if (result == NGHTTP2_ERR_INVALID_ARGUMENT || result == NGHTTP2_ERR_INVALID_STATE
				|| result == NGHTTP2_ERR_PROTO)
			{
				return {ErrorCode::hInvalidConfiguration, "HTTP/2 操作参数或当前状态无效"};
			}
			auto error = protocolError(streamId, wireError(result));
			if (result == NGHTTP2_ERR_NOMEM)
			{
				error.code = ErrorCode::hResourceLimitExceeded;
			}
			error.message = "HTTP/2 协议操作失败，库错误码 " + std::to_string(result);
			return error;
		}

		struct PumpGuard
		{
			bool& active;

			explicit PumpGuard(bool& flag) : active(flag)
			{
				active = true;
			}

			~PumpGuard()
			{
				active = false;
			}
		};
	} // namespace

	struct Http2Codec::Impl
	{
		Role role_;
		Http2Options options_;
		Listener* listener_;
		std::unique_ptr<nghttp2_session_callbacks, decltype(&nghttp2_session_callbacks_del)> callbacks_ {
			nullptr,
			&nghttp2_session_callbacks_del};
		std::unique_ptr<nghttp2_option, decltype(&nghttp2_option_del)> libraryOptions_ {nullptr, &nghttp2_option_del};
		std::unique_ptr<nghttp2_session, decltype(&nghttp2_session_del)> session_ {nullptr, &nghttp2_session_del};
		std::string pendingOutput_;
		std::unordered_map<int32_t, std::shared_ptr<DataSource>> dataSources_;
		std::unordered_set<int32_t> localRequests_;
		std::shared_ptr<DataSource> pendingSource_;
		HeaderBlock currentHeaders_ {0, HeaderKind::hRequest, 0, false};
		size_t headerFields_ {0};
		size_t headerBytes_ {0};
		bool pumpActive_ {false};
		char rejectedMarker_ {0};
		NetworkError terminalError_;
		std::array<uint8_t, kFrameHeaderBytes> inboundHeader_ {};
		size_t inboundHeaderBytes_ {0};
		size_t inboundPayloadLeft_ {0};
		size_t inboundPrefaceLeft_ {0};
		Frame inboundFrame_ {FrameType::hData, 0, 0, 0};
		bool inboundStopped_ {false};

		Impl(Role role, const Http2Options& options, Listener* listener)
			: role_(role), options_(options), listener_(listener)
		{
			options.validate();
			inboundPrefaceLeft_ = role == Role::hServer ? NGHTTP2_CLIENT_MAGIC_LEN : 0;
			if (role != Role::hClient && role != Role::hServer)
			{
				throw std::invalid_argument("HTTP/2 会话角色无效");
			}
			nghttp2_session_callbacks* callbacks = nullptr;
			checkCreation(nghttp2_session_callbacks_new(&callbacks));
			callbacks_.reset(callbacks);
			nghttp2_option* libraryOptions = nullptr;
			checkCreation(nghttp2_option_new(&libraryOptions));
			libraryOptions_.reset(libraryOptions);
			registerCallbacks();
			nghttp2_option_set_no_auto_window_update(libraryOptions, 1);
			nghttp2_option_set_peer_max_concurrent_streams(libraryOptions, options.maxConcurrentStreams);
			nghttp2_option_set_max_send_header_block_length(libraryOptions, options.maxConnectionSendBytes);
			nghttp2_option_set_max_deflate_dynamic_table_size(libraryOptions, options.headerTableBytes);
			nghttp2_session* session = nullptr;
			checkCreation(role == Role::hClient
							  ? nghttp2_session_client_new2(&session, callbacks, this, libraryOptions)
							  : nghttp2_session_server_new2(&session, callbacks, this, libraryOptions));
			session_.reset(session);
			const auto headerLimit =
				static_cast<uint32_t>(std::min<size_t>(options.maxHeaderBytes, std::numeric_limits<uint32_t>::max()));
			std::array<nghttp2_settings_entry, 6> settings {
				{{NGHTTP2_SETTINGS_HEADER_TABLE_SIZE, options.headerTableBytes},
				 {NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, role == Role::hClient ? 0U : options.maxConcurrentStreams},
				 {NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE, options.initialStreamWindow},
				 {NGHTTP2_SETTINGS_MAX_FRAME_SIZE, options.maxFrameBytes},
				 {NGHTTP2_SETTINGS_MAX_HEADER_LIST_SIZE, headerLimit},
				 {NGHTTP2_SETTINGS_ENABLE_PUSH, 0}}};
			const size_t count = role == Role::hClient ? settings.size() : settings.size() - 1;
			checkCreation(nghttp2_submit_settings(session, NGHTTP2_FLAG_NONE, settings.data(), count));
			checkCreation(nghttp2_session_set_local_window_size(session,
																NGHTTP2_FLAG_NONE,
																0,
																static_cast<int32_t>(options.initialConnectionWindow)));
		}

		~Impl()
		{
			// session 释放前仍保留所有 source，避免底层销毁路径借用已销毁的数据源。
			session_.reset();
		}

		static void checkCreation(int result)
		{
			if (result == NGHTTP2_ERR_NOMEM)
			{
				throw std::bad_alloc();
			}
			if (result != 0)
			{
				throw std::runtime_error("HTTP/2 会话初始化失败，库错误码 " + std::to_string(result));
			}
		}

		template <class Callback>
		int invoke(Callback&& callback) noexcept
		{
			try
			{
				return callback();
			}
			catch (...)
			{
				// 监听器抛出的异常不能越过 C 回调边界。
				return NGHTTP2_ERR_CALLBACK_FAILURE;
			}
		}

		bool rejected(int32_t streamId) const noexcept
		{
			return nghttp2_session_get_stream_user_data(session_.get(), streamId) == &rejectedMarker_;
		}

		int reject(int32_t streamId, Http2Code code) noexcept
		{
			const int result =
				nghttp2_submit_rst_stream(session_.get(), NGHTTP2_FLAG_NONE, streamId, static_cast<uint32_t>(code));
			if (result == 0)
			{
				nghttp2_session_set_stream_user_data(session_.get(), streamId, &rejectedMarker_);
			}
			return result;
		}

		void notifyError(int32_t streamId, Http2Code code)
		{
			if (listener_ && code != Http2Code::hNoError)
			{
				listener_->onError(protocolError(streamId, code));
			}
		}

		static Frame describe(const nghttp2_frame& frame) noexcept
		{
			return {static_cast<FrameType>(frame.hd.type), frame.hd.flags, frame.hd.stream_id, frame.hd.length};
		}

		bool passInbound(std::string_view bytes)
		{
			const auto result =
				nghttp2_session_mem_recv2(session_.get(), reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
			if (result < 0 || static_cast<size_t>(result) != bytes.size())
			{
				// 本适配层不提供 PAUSE 回调；不完整推进时不能继续使用外层帧边界。
				terminalError_ = operationError(result < 0 ? static_cast<int>(result) : NGHTTP2_ERR_CALLBACK_FAILURE);
				return false;
			}
			return !terminalError_;
		}

		bool headerBlockEvent(bool complete)
		{
			if (listener_)
			{
				const int result = invoke(
					[&]
					{
						listener_->onHeaderBlock(inboundFrame_.streamId, complete);
						return 0;
					});
				if (result != 0)
				{
					terminalError_ = operationError(result);
				}
			}
			return !terminalError_;
		}

		bool finishInboundFrame()
		{
			return (inboundFrame_.type != FrameType::hHeaders && inboundFrame_.type != FrameType::hContinuation)
				   || (inboundFrame_.flags & kEndHeaders) == 0 || headerBlockEvent(true);
		}

		bool stopInbound(Http2Code code)
		{
			const int result = nghttp2_session_terminate_session(session_.get(), static_cast<uint32_t>(code));
			if (result != 0)
			{
				terminalError_ = operationError(result);
				return false;
			}
			// 保留 memSend，让唯一 writer 把 GOAWAY 写完后再关闭连接。
			inboundStopped_ = true;
			return true;
		}

		bool beginInboundFrame()
		{
			inboundFrame_ = {
				static_cast<FrameType>(inboundHeader_[3]),
				inboundHeader_[4],
				static_cast<int32_t>(((uint32_t {inboundHeader_[5]} << 24) | (uint32_t {inboundHeader_[6]} << 16)
									  | (uint32_t {inboundHeader_[7]} << 8) | inboundHeader_[8])
									 & 0x7fffffffU),
				(size_t {inboundHeader_[0]} << 16) | (size_t {inboundHeader_[1]} << 8) | inboundHeader_[2]};
			inboundHeaderBytes_ = 0;
			// nghttp2 1.70 的忽略 CONTINUATION 路径没有 on_begin_frame，也不检查 MAX_FRAME_SIZE。
			// 按当前已 ACK 生效的本端设置检查所有帧，且仅保留固定九字节头部。
			if (inboundFrame_.length
				> nghttp2_session_get_local_settings(session_.get(), NGHTTP2_SETTINGS_MAX_FRAME_SIZE))
			{
				return stopInbound(Http2Code::hFrameSizeError);
			}
			if (inboundFrame_.type == FrameType::hPriority && inboundFrame_.streamId == 0)
			{
				// nghttp2 1.70 忽略 PRIORITY 载荷；RFC 9113 6.3 仍要求校验流 ID 非零。
				return stopInbound(Http2Code::hProtocolError);
			}
			if (inboundFrame_.type == FrameType::hHeaders && !headerBlockEvent(false))
			{
				return false;
			}
			inboundPayloadLeft_ = inboundFrame_.length;
			return passInbound({reinterpret_cast<const char*>(inboundHeader_.data()), inboundHeader_.size()})
				   && (inboundPayloadLeft_ != 0 || finishInboundFrame());
		}

		ReceiveResult receive(std::string_view bytes)
		{
			const auto size = bytes.size();
			while (!bytes.empty() && !inboundStopped_)
			{
				if (inboundPrefaceLeft_ != 0 || inboundPayloadLeft_ != 0)
				{
					const bool preface = inboundPrefaceLeft_ != 0;
					auto& remaining = preface ? inboundPrefaceLeft_ : inboundPayloadLeft_;
					const auto count = std::min(remaining, bytes.size());
					if (!passInbound(bytes.substr(0, count)))
					{
						return {0, terminalError_};
					}
					remaining -= count;
					bytes.remove_prefix(count);
					if (!preface && remaining == 0 && !finishInboundFrame())
					{
						return {0, terminalError_};
					}
					continue;
				}
				const auto count = std::min(inboundHeader_.size() - inboundHeaderBytes_, bytes.size());
				std::copy_n(bytes.data(), count, inboundHeader_.data() + inboundHeaderBytes_);
				inboundHeaderBytes_ += count;
				bytes.remove_prefix(count);
				if (inboundHeaderBytes_ == inboundHeader_.size() && !beginInboundFrame())
				{
					return {0, terminalError_};
				}
			}
			return {size, {}};
		}

		static int beginHeaders(nghttp2_session*, const nghttp2_frame* frame, void* data) noexcept
		{
			auto& self = *static_cast<Impl*>(data);
			return self.invoke(
				[&]() -> int
				{
					self.currentHeaders_ = {frame->hd.stream_id,
											frame->headers.cat == NGHTTP2_HCAT_REQUEST ? HeaderKind::hRequest
																					   : HeaderKind::hTrailers,
											0,
											(frame->hd.flags & NGHTTP2_FLAG_END_STREAM) != 0};
					self.headerFields_ = 0;
					self.headerBytes_ = 0;
					const auto code =
						self.listener_ ? self.listener_->onBeginHeaders(frame->hd.stream_id) : Http2Code::hNoError;
					if (code == Http2Code::hNoError)
					{
						return 0;
					}
					return self.reject(frame->hd.stream_id, code) == 0 ? NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE
																	   : NGHTTP2_ERR_CALLBACK_FAILURE;
				});
		}

		static int header(nghttp2_session*,
						  const nghttp2_frame* frame,
						  const uint8_t* name,
						  size_t nameLength,
						  const uint8_t* value,
						  size_t valueLength,
						  uint8_t,
						  void* data) noexcept
		{
			auto& self = *static_cast<Impl*>(data);
			return self.invoke(
				[&]() -> int
				{
					Http2Code code = Http2Code::hEnhanceYourCalm;
					if (self.headerFields_ < self.options_.maxHeaderFields
						&& Http2Options::kHeaderFieldOverheadBytes <= self.options_.maxHeaderBytes - self.headerBytes_
						&& nameLength <= self.options_.maxHeaderBytes - self.headerBytes_
											 - Http2Options::kHeaderFieldOverheadBytes
						&& valueLength <= self.options_.maxHeaderBytes - self.headerBytes_
											  - Http2Options::kHeaderFieldOverheadBytes - nameLength)
					{
						++self.headerFields_;
						self.headerBytes_ += Http2Options::kHeaderFieldOverheadBytes + nameLength + valueLength;
						Header field {{reinterpret_cast<const char*>(name), nameLength},
									  {reinterpret_cast<const char*>(value), valueLength}};
						if (field.name == ":status" && valueLength == 3)
						{
							self.currentHeaders_.kind = HeaderKind::hResponse;
							self.currentHeaders_.statusCode =
								static_cast<uint16_t>((value[0] - '0') * 100 + (value[1] - '0') * 10 + value[2] - '0');
						}
						code =
							self.listener_ ? self.listener_->onHeader(frame->hd.stream_id, field) : Http2Code::hNoError;
					}
					if (code == Http2Code::hNoError)
					{
						return 0;
					}
					return self.reject(frame->hd.stream_id, code) == 0 ? NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE
																	   : NGHTTP2_ERR_CALLBACK_FAILURE;
				});
		}

		static int invalidHeader(nghttp2_session*,
								 const nghttp2_frame* frame,
								 const uint8_t*,
								 size_t,
								 const uint8_t*,
								 size_t,
								 uint8_t,
								 void* data) noexcept
		{
			auto& self = *static_cast<Impl*>(data);
			return self.invoke(
				[&]() -> int
				{
					// 库默认忽略部分非法普通字段；严格拒流，避免绕过原始头字段预算。
					return self.reject(frame->hd.stream_id, Http2Code::hProtocolError) == 0
							   ? NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE
							   : NGHTTP2_ERR_CALLBACK_FAILURE;
				});
		}

		static int dataChunk(nghttp2_session*,
							 uint8_t,
							 int32_t streamId,
							 const uint8_t* bytes,
							 size_t length,
							 void* data) noexcept
		{
			auto& self = *static_cast<Impl*>(data);
			return self.invoke(
				[&]() -> int
				{
					if (self.rejected(streamId))
					{
						return nghttp2_session_consume_connection(self.session_.get(), length);
					}
					const auto code =
						self.listener_
							? self.listener_->onData(streamId, {reinterpret_cast<const char*>(bytes), length})
							: Http2Code::hNoError;
					if (code == Http2Code::hNoError)
					{
						return 0;
					}
					if (self.reject(streamId, code) != 0)
					{
						return NGHTTP2_ERR_CALLBACK_FAILURE;
					}
					// 此回调拒绝的字节没有交给应用；后续忽略的 DATA 由协议库自行归还连接额度。
					return nghttp2_session_consume_connection(self.session_.get(), length);
				});
		}

		static int frameReceived(nghttp2_session*, const nghttp2_frame* frame, void* data) noexcept
		{
			auto& self = *static_cast<Impl*>(data);
			return self.invoke(
				[&]() -> int
				{
					if (!self.listener_)
					{
						return 0;
					}
					if (frame->hd.type == NGHTTP2_HEADERS && !self.rejected(frame->hd.stream_id))
					{
						const auto code = self.listener_->onEndHeaders(self.currentHeaders_);
						if (code != Http2Code::hNoError && self.reject(frame->hd.stream_id, code) != 0)
						{
							return NGHTTP2_ERR_CALLBACK_FAILURE;
						}
					}
					if ((frame->hd.type == NGHTTP2_HEADERS || frame->hd.type == NGHTTP2_DATA)
						&& (frame->hd.flags & NGHTTP2_FLAG_END_STREAM) && !self.rejected(frame->hd.stream_id))
					{
						self.listener_->onEndStream(frame->hd.stream_id);
					}
					self.listener_->onFrameReceived(describe(*frame));
					if (frame->hd.type == NGHTTP2_SETTINGS)
					{
						self.listener_->onSettings((frame->hd.flags & NGHTTP2_FLAG_ACK) != 0);
					}
					else if (frame->hd.type == NGHTTP2_GOAWAY)
					{
						const auto code = static_cast<Http2Code>(frame->goaway.error_code);
						const auto debug =
							frame->goaway.opaque_data_len == 0
								? std::string_view {}
								: std::string_view {reinterpret_cast<const char*>(frame->goaway.opaque_data),
													frame->goaway.opaque_data_len};
						self.listener_->onGoaway(frame->goaway.last_stream_id, code, debug);
						self.notifyError(0, code);
					}
					return 0;
				});
		}

		static int frameSent(nghttp2_session*, const nghttp2_frame* frame, void* data) noexcept
		{
			auto& self = *static_cast<Impl*>(data);
			return self.invoke(
				[&]() -> int
				{
					if (self.listener_)
					{
						self.listener_->onFrameSent(describe(*frame));
					}
					if (frame->hd.type == NGHTTP2_GOAWAY)
					{
						self.notifyError(0, static_cast<Http2Code>(frame->goaway.error_code));
					}
					return 0;
				});
		}

		static int streamClose(nghttp2_session*, int32_t streamId, uint32_t errorCode, void* data) noexcept
		{
			auto& self = *static_cast<Impl*>(data);
			return self.invoke(
				[&]() -> int
				{
					const auto code = static_cast<Http2Code>(errorCode);
					self.localRequests_.erase(streamId);
					if (const auto found = self.dataSources_.find(streamId); found != self.dataSources_.end())
					{
						auto source = std::move(found->second);
						self.dataSources_.erase(found);
						source->onClose(code);
					}
					self.notifyError(streamId, code);
					if (self.listener_)
					{
						self.listener_->onStreamClose(streamId, code);
					}
					return 0;
				});
		}

		static nghttp2_ssize readData(nghttp2_session* session,
									  int32_t streamId,
									  uint8_t* output,
									  size_t length,
									  uint32_t* flags,
									  nghttp2_data_source*,
									  void* data) noexcept
		{
			auto& self = *static_cast<Impl*>(data);
			try
			{
				const auto found = self.dataSources_.find(streamId);
				if (found == self.dataSources_.end())
				{
					return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
				}
				auto source = found->second;
				auto result = source->read({reinterpret_cast<char*>(output), length});
				if (result.bytes > length || (result.deferred && (result.bytes != 0 || result.eof))
					|| (result.bytes == 0 && !result.eof && !result.deferred))
				{
					result.error = protocolError(streamId, Http2Code::hInternalError);
				}
				if (result.error)
				{
					const auto code = result.error.http2 ? static_cast<Http2Code>(result.error.http2->code)
														 : Http2Code::hInternalError;
					if (self.listener_)
					{
						self.listener_->onError(result.error);
					}
					return self.reject(streamId, code) == 0 ? NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE
															: NGHTTP2_ERR_CALLBACK_FAILURE;
				}
				if (result.deferred)
				{
					return NGHTTP2_ERR_DEFERRED;
				}
				if (result.eof)
				{
					*flags |= NGHTTP2_DATA_FLAG_EOF;
					const auto trailers = source->trailers();
					if (!trailers.empty())
					{
						std::vector<nghttp2_nv> fields;
						if (self.makeHeaders(trailers, fields)
							|| nghttp2_submit_trailer(session, streamId, fields.data(), fields.size()) != 0)
						{
							return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
						}
						*flags |= NGHTTP2_DATA_FLAG_NO_END_STREAM;
					}
				}
				return static_cast<nghttp2_ssize>(result.bytes);
			}
			catch (...)
			{
				return NGHTTP2_ERR_CALLBACK_FAILURE;
			}
		}

		void registerCallbacks()
		{
			auto* callbacks = callbacks_.get();
			nghttp2_session_callbacks_set_on_begin_headers_callback(callbacks, beginHeaders);
			nghttp2_session_callbacks_set_on_header_callback(callbacks, header);
			nghttp2_session_callbacks_set_on_invalid_header_callback(callbacks, invalidHeader);
			nghttp2_session_callbacks_set_on_data_chunk_recv_callback(callbacks, dataChunk);
			nghttp2_session_callbacks_set_on_frame_recv_callback(callbacks, frameReceived);
			nghttp2_session_callbacks_set_on_frame_send_callback(callbacks, frameSent);
			nghttp2_session_callbacks_set_on_stream_close_callback(callbacks, streamClose);
		}

		NetworkError checkPump() const
		{
			if (pumpActive_)
			{
				return {ErrorCode::hOperationInProgress, "HTTP/2 收发泵不能从回调重入"};
			}
			return terminalError_;
		}

		NetworkError makeHeaders(std::span<const Header> headers, std::vector<nghttp2_nv>& fields) const
		{
			size_t bytes = 0;
			if (headers.size() > options_.maxHeaderFields)
			{
				return {ErrorCode::hResourceLimitExceeded, "HTTP/2 头字段数量超过预算"};
			}
			for (const auto& field : headers)
			{
				if (Http2Options::kHeaderFieldOverheadBytes > options_.maxHeaderBytes - bytes
					|| field.name.size() > options_.maxHeaderBytes - bytes - Http2Options::kHeaderFieldOverheadBytes
					|| field.value.size() > options_.maxHeaderBytes - bytes - Http2Options::kHeaderFieldOverheadBytes
												- field.name.size())
				{
					return {ErrorCode::hResourceLimitExceeded, "HTTP/2 头字段字节数超过预算"};
				}
				bytes += Http2Options::kHeaderFieldOverheadBytes + field.name.size() + field.value.size();
			}
			fields.reserve(headers.size());
			for (const auto& field : headers)
			{
				fields.push_back({reinterpret_cast<uint8_t*>(const_cast<char*>(field.name.data())),
								  reinterpret_cast<uint8_t*>(const_cast<char*>(field.value.data())),
								  field.name.size(),
								  field.value.size(),
								  NGHTTP2_NV_FLAG_NONE});
			}
			return {};
		}

		SubmitResult submitHeaders(int32_t streamId, std::span<const Header> headers, bool endStream)
		try
		{
			if (terminalError_)
			{
				return {0, terminalError_};
			}
			std::vector<nghttp2_nv> fields;
			if (auto error = makeHeaders(headers, fields))
			{
				return {0, std::move(error)};
			}
			const int32_t result = nghttp2_submit_headers(session_.get(),
														  endStream ? NGHTTP2_FLAG_END_STREAM : NGHTTP2_FLAG_NONE,
														  streamId,
														  nullptr,
														  fields.data(),
														  fields.size(),
														  nullptr);
			if (result >= 0 && streamId == -1)
			{
				localRequests_.emplace(result);
			}
			return {result >= 0 ? (streamId == -1 ? result : streamId) : 0,
					operationError(result, streamId > 0 ? streamId : 0)};
		}
		catch (const std::bad_alloc&)
		{
			terminalError_ = {ErrorCode::hResourceLimitExceeded, {}};
			return {0, terminalError_};
		}

		SubmitResult submitSource(int32_t streamId, std::span<const Header> headers, std::shared_ptr<DataSource> source)
		try
		{
			if (terminalError_)
			{
				return {0, terminalError_};
			}
			if (!source || (streamId > 0 && dataSources_.contains(streamId)))
			{
				return {0, {ErrorCode::hInvalidConfiguration, "HTTP/2 DATA 源为空或重复"}};
			}
			if (dataSources_.size() >= options_.maxConcurrentStreams)
			{
				return {0, {ErrorCode::hResourceLimitExceeded, "HTTP/2 DATA 源超过本地流容量"}};
			}
			std::vector<nghttp2_nv> fields;
			if (auto error = makeHeaders(headers, fields))
			{
				return {0, std::move(error)};
			}
			// 登记失败时仍由 pendingSource_ 持有库正在使用的数据源，随后终止连接处理。
			pendingSource_ = std::move(source);
			nghttp2_data_provider2 provider {{}, readData};
			provider.source.ptr = pendingSource_.get();
			const int32_t result =
				streamId == -1
					? nghttp2_submit_request2(session_.get(), nullptr, fields.data(), fields.size(), &provider, nullptr)
					: nghttp2_submit_response2(session_.get(), streamId, fields.data(), fields.size(), &provider);
			if (result < 0)
			{
				pendingSource_.reset();
				return {0, operationError(result, streamId > 0 ? streamId : 0)};
			}
			const int32_t id = streamId == -1 ? result : streamId;
			dataSources_.emplace(id, pendingSource_);
			if (streamId == -1)
			{
				localRequests_.emplace(id);
			}
			pendingSource_.reset();
			return {id, {}};
		}
		catch (const std::bad_alloc&)
		{
			terminalError_ = {ErrorCode::hResourceLimitExceeded, {}};
			return {0, terminalError_};
		}
	};

	std::span<const Http2Codec::Header> Http2Codec::DataSource::trailers() const noexcept
	{
		return {};
	}

	void Http2Codec::DataSource::onClose(Http2Code)
	{
	}

	Http2Code Http2Codec::Listener::onBeginHeaders(int32_t)
	{
		return Http2Code::hNoError;
	}

	Http2Code Http2Codec::Listener::onHeader(int32_t, Header)
	{
		return Http2Code::hNoError;
	}

	Http2Code Http2Codec::Listener::onEndHeaders(const HeaderBlock&)
	{
		return Http2Code::hNoError;
	}

	Http2Code Http2Codec::Listener::onData(int32_t, std::string_view)
	{
		return Http2Code::hNoError;
	}

	void Http2Codec::Listener::onEndStream(int32_t)
	{
	}

	void Http2Codec::Listener::onStreamClose(int32_t, Http2Code)
	{
	}

	void Http2Codec::Listener::onHeaderBlock(int32_t, bool)
	{
	}

	void Http2Codec::Listener::onFrameReceived(const Frame&)
	{
	}

	void Http2Codec::Listener::onFrameSent(const Frame&)
	{
	}

	void Http2Codec::Listener::onSettings(bool)
	{
	}

	void Http2Codec::Listener::onGoaway(int32_t, Http2Code, std::string_view)
	{
	}

	void Http2Codec::Listener::onError(const NetworkError&)
	{
	}

	Http2Codec::Http2Codec(Role role, const Http2Options& options, Listener* listener)
		: impl_(std::make_unique<Impl>(role, options, listener))
	{
	}

	Http2Codec::~Http2Codec() = default;

	NetworkError Http2Codec::setListener(Listener* listener)
	{
		if (auto error = impl_->checkPump())
		{
			return error;
		}
		impl_->listener_ = listener;
		return {};
	}

	Http2Codec::ReceiveResult Http2Codec::memRecv(std::string_view bytes)
	{
		if (auto error = impl_->checkPump())
		{
			return {0, std::move(error)};
		}
		PumpGuard guard(impl_->pumpActive_);
		return impl_->receive(bytes);
	}

	Http2Codec::SendResult Http2Codec::memSend(size_t maxBytes, size_t storageBudget)
	try
	{
		if (auto error = impl_->checkPump())
		{
			return {{}, std::move(error)};
		}
		const size_t limit = std::min(maxBytes, impl_->options_.maxOutputBatchBytes);
		if (limit == 0)
		{
			return {};
		}
		PumpGuard guard(impl_->pumpActive_);
		auto& pending = impl_->pendingOutput_;
		const size_t storageLimit = std::min(storageBudget, impl_->options_.maxConnectionSendBytes);
		if (pending.size() > storageLimit)
		{
			impl_->terminalError_ = {ErrorCode::hResourceLimitExceeded, "HTTP/2 余留输出超过连接待发预算"};
			return {{}, impl_->terminalError_};
		}
		if (!pending.empty())
		{
			if (pending.size() <= limit)
			{
				return {std::exchange(pending, {}), {}};
			}
			auto output = pending.substr(0, limit);
			pending.erase(0, limit);
			return {std::move(output), {}};
		}
		const uint8_t* bytes = nullptr;
		const auto count = nghttp2_session_mem_send2(impl_->session_.get(), &bytes);
		if (count < 0)
		{
			impl_->terminalError_ = operationError(static_cast<int>(count));
			return {{}, impl_->terminalError_};
		}
		if (count == 0)
		{
			return {};
		}
		if (static_cast<size_t>(count) > storageLimit)
		{
			impl_->terminalError_ = {ErrorCode::hResourceLimitExceeded, "HTTP/2 协议输出超过连接待发预算"};
			return {{}, impl_->terminalError_};
		}
		const auto length = static_cast<size_t>(count);
		const auto outputLength = std::min(length, limit);
		const auto* text = reinterpret_cast<const char*>(bytes);
		std::string output(text, outputLength);
		pending.assign(text + outputLength, length - outputLength);
		return {std::move(output), {}};
	}
	catch (const std::bad_alloc&)
	{
		// 协议状态可能已经推进，复制失败后必须结束连接，不能跳过字节继续发送。
		impl_->terminalError_ = {ErrorCode::hResourceLimitExceeded,
								 {},
								 Http2ErrorInfo {static_cast<uint32_t>(Http2Code::hInternalError), 0}};
		return {{}, impl_->terminalError_};
	}

	size_t Http2Codec::bufferedOutputBytes() const noexcept
	{
		return impl_->pendingOutput_.size();
	}

	Http2Codec::SubmitResult Http2Codec::submitRequest(std::span<const Header> headers, bool endStream)
	{
		if (impl_->role_ != Role::hClient)
		{
			return {0, {ErrorCode::hInvalidConfiguration, "只有 HTTP/2 客户端可以提交请求"}};
		}
		return impl_->submitHeaders(-1, headers, endStream);
	}

	NetworkError Http2Codec::submitResponse(int32_t streamId, std::span<const Header> headers, bool endStream)
	{
		if (impl_->role_ != Role::hServer || streamId <= 0)
		{
			return {ErrorCode::hInvalidConfiguration, "HTTP/2 响应需要服务端角色及有效流 ID"};
		}
		return impl_->submitHeaders(streamId, headers, endStream).error;
	}

	Http2Codec::SubmitResult Http2Codec::submitRequest(std::span<const Header> headers,
													   std::shared_ptr<DataSource> source)
	{
		if (impl_->role_ != Role::hClient)
		{
			return {0, {ErrorCode::hInvalidConfiguration, "只有 HTTP/2 客户端可以提交请求"}};
		}
		return impl_->submitSource(-1, headers, std::move(source));
	}

	NetworkError Http2Codec::submitResponse(int32_t streamId,
											std::span<const Header> headers,
											std::shared_ptr<DataSource> source)
	{
		if (impl_->role_ != Role::hServer || streamId <= 0)
		{
			return {ErrorCode::hInvalidConfiguration, "HTTP/2 响应需要服务端角色及有效流 ID"};
		}
		return impl_->submitSource(streamId, headers, std::move(source)).error;
	}

	NetworkError Http2Codec::submitTrailers(int32_t streamId, std::span<const Header> headers)
	{
		if (streamId <= 0)
		{
			return {ErrorCode::hInvalidConfiguration, "HTTP/2 尾头需要有效流 ID"};
		}
		return impl_->submitHeaders(streamId, headers, true).error;
	}

	NetworkError Http2Codec::submitSettings(std::span<const Setting> settings)
	{
		if (impl_->terminalError_)
		{
			return impl_->terminalError_;
		}
		if (settings.size() > kMaxSettings)
		{
			return {ErrorCode::hResourceLimitExceeded, "HTTP/2 SETTINGS 条目超过上限"};
		}
		std::vector<nghttp2_settings_entry> entries;
		entries.reserve(settings.size());
		for (const auto& setting : settings)
		{
			if (setting.id == Http2Setting::hEnablePush && (impl_->role_ != Role::hClient || setting.value != 0))
			{
				return {ErrorCode::hInvalidConfiguration, "HTTP/2 PUSH 必须保持关闭"};
			}
			entries.push_back({static_cast<int32_t>(setting.id), setting.value});
		}
		return operationError(
			nghttp2_submit_settings(impl_->session_.get(), NGHTTP2_FLAG_NONE, entries.data(), entries.size()));
	}

	NetworkError Http2Codec::reset(int32_t streamId, Http2Code code)
	{
		if (impl_->terminalError_)
		{
			return impl_->terminalError_;
		}
		return operationError(impl_->reject(streamId, code), streamId);
	}

	NetworkError Http2Codec::goaway(int32_t lastStreamId, Http2Code code)
	{
		if (impl_->terminalError_)
		{
			return impl_->terminalError_;
		}
		return operationError(nghttp2_submit_goaway(impl_->session_.get(),
													NGHTTP2_FLAG_NONE,
													lastStreamId,
													static_cast<uint32_t>(code),
													nullptr,
													0));
	}

	NetworkError Http2Codec::consume(int32_t streamId, size_t bytes)
	{
		if (impl_->terminalError_)
		{
			return impl_->terminalError_;
		}
		return operationError(streamId == 0 ? nghttp2_session_consume_connection(impl_->session_.get(), bytes)
											: nghttp2_session_consume(impl_->session_.get(), streamId, bytes),
							  streamId);
	}

	NetworkError Http2Codec::resume(int32_t streamId)
	{
		if (impl_->terminalError_)
		{
			return impl_->terminalError_;
		}
		return operationError(nghttp2_session_resume_data(impl_->session_.get(), streamId), streamId);
	}

	void Http2Codec::abortOutput()
	{
		if (!impl_->terminalError_)
		{
			impl_->terminalError_ = {ErrorCode::hOperationAborted, "HTTP/2 输出已停止"};
		}
		std::string {}.swap(impl_->pendingOutput_);
	}

	Http2Codec::Role Http2Codec::role() const noexcept
	{
		return impl_->role_;
	}

	bool Http2Codec::wantRead() const noexcept
	{
		return !impl_->terminalError_ && nghttp2_session_want_read(impl_->session_.get()) != 0;
	}

	bool Http2Codec::wantWrite() const noexcept
	{
		return !impl_->terminalError_
			   && (!impl_->pendingOutput_.empty() || nghttp2_session_want_write(impl_->session_.get()) != 0);
	}

	uint32_t Http2Codec::peerSetting(Http2Setting setting) const noexcept
	{
		return nghttp2_session_get_remote_settings(impl_->session_.get(), static_cast<nghttp2_settings_id>(setting));
	}

	uint32_t Http2Codec::peerMaxConcurrentStreams() const noexcept
	{
		return peerSetting(Http2Setting::hMaxConcurrentStreams);
	}

	size_t Http2Codec::activeLocalStreamCount() const noexcept
	{
		size_t count = 0;
		for (const auto id : impl_->localRequests_)
		{
			if (nghttp2_session_get_stream_local_close(impl_->session_.get(), id) >= 0)
			{
				++count;
			}
		}
		return count;
	}

	int32_t Http2Codec::peerWindow(int32_t streamId) const noexcept
	{
		return streamId == 0 ? nghttp2_session_get_remote_window_size(impl_->session_.get())
							 : nghttp2_session_get_stream_remote_window_size(impl_->session_.get(), streamId);
	}

	bool Http2Codec::requestAllowed() const noexcept
	{
		return !impl_->terminalError_ && impl_->role_ == Role::hClient
			   && nghttp2_session_check_request_allowed(impl_->session_.get()) != 0;
	}
} // namespace hical::detail

#endif
