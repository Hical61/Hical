/**
 * @file Http2ClientSession.cpp
 * @brief HTTP/2 客户端请求、响应及收发任务的生命周期
 */

#include "Http2ClientSession.h"

#ifdef HICAL_HAS_HTTP2

	#include "core/http2/Http2Receive.h"
	#include <boost/asio/cancel_at.hpp>
	#include <boost/asio/redirect_error.hpp>
	#include <boost/asio/this_coro.hpp>
	#include <boost/asio/use_awaitable.hpp>
	#include <algorithm>
	#include <limits>
	#include <stdexcept>
	#include <unordered_map>
	#include <utility>
	#include <vector>

namespace hical::detail
{
	namespace
	{
		HttpClientResult unsent(NetworkError error)
		{
			HttpClientResult result;
			result.protocol = HttpProtocol::hHttp2;
			result.requestDisposition = HttpRequestDisposition::hNotSent;
			result.error = std::move(error);
			return result;
		}

		std::string lowerName(std::string_view name)
		{
			std::string result(name);
			for (auto& ch : result)
			{
				if (ch >= 'A' && ch <= 'Z')
				{
					ch = static_cast<char>(ch + ('a' - 'A'));
				}
			}
			return result;
		}

		bool namedByConnection(const HeaderMap& headers, std::string_view name)
		{
			for (const auto& [key, value] : headers)
			{
				if (!HeaderMap::iequals(key, "connection"))
				{
					continue;
				}
				std::string_view remaining(value);
				while (!remaining.empty())
				{
					const auto separator = remaining.find(',');
					auto token = remaining.substr(0, separator);
					const auto first = token.find_first_not_of(" \t");
					if (first != std::string_view::npos)
					{
						token = token.substr(first, token.find_last_not_of(" \t") - first + 1);
						if (HeaderMap::iequals(name, token))
						{
							return true;
						}
					}
					if (separator == std::string_view::npos)
					{
						break;
					}
					remaining.remove_prefix(separator + 1);
				}
			}
			return false;
		}

		NetworkError prepareHeaders(const Http2ClientSession::Request& request, Http2Headers& owned)
		{
			NetworkError error;
			auto append = [&](std::string_view name, std::string_view value)
			{
				if (!error)
				{
					error = owned.append(name, value);
				}
			};
			append(":method", httpMethodToString(request.method));
			append(":scheme", "https");
			append(":authority", request.authority);
			append(":path", request.target);
			for (const auto& [name, value] : request.headers)
			{
				auto normalized = lowerName(name);
				if (normalized == "host" || normalized == "content-length" || normalized == "connection"
					|| normalized == "keep-alive" || normalized == "proxy-connection" || normalized == "upgrade"
					|| normalized == "transfer-encoding" || namedByConnection(request.headers, normalized))
				{
					continue;
				}
				append(normalized, value);
			}
			if (!request.body.empty() || request.method == HttpMethod::hPost || request.method == HttpMethod::hPut
				|| request.method == HttpMethod::hPatch)
			{
				append("content-length", std::to_string(request.body.size()));
			}
			if (!error)
			{
				error = owned.seal();
			}
			if (error)
			{
				// 预检还未建立协议流，临时校验器的 ID 不能作为请求已发送的证据。
				error.http2.reset();
				if (error.code != ErrorCode::hResourceLimitExceeded)
				{
					error.code = ErrorCode::hInvalidConfiguration;
				}
			}
			return error;
		}

		void copyFields(const Http2Headers& source, HeaderMap& destination)
		{
			for (const auto& field : source.fields())
			{
				destination.insert(field.name, field.value);
			}
		}
	} // namespace

	struct Http2ClientSession::Impl final : Http2Codec::Listener
	{
		using Clock = std::chrono::steady_clock;

		struct Context
		{
			std::shared_ptr<Http2Stream> stream;
			std::shared_ptr<Http2Output::Producer> producer;
			HttpClientResult result;
			TimePoint deadline {TimePoint::max()};
			TimePoint completedAt {};
			bool responseReady {false};
			bool resetReceived {false};
		};

		Impl(TlsSocket& socket, const Http2Options& options)
			: options_(options)
			, budget_(Http2Budget::Role::hClient, options)
			, connection_(Http2Connection::create(socket,
												  Http2Codec::Role::hClient,
												  options,
												  budget_,
												  {.onActivity = {},
												   .onWritten = {},
												   .onFailure =
													   [this](const NetworkError& error)
												   {
													   fail(error);
												   }}))
			, receive_(socket.get_executor(), connection_->codec(), budget_, {Http2Codec::Role::hClient, options})
			, settingsWake_(socket.get_executor(), TimePoint::max())
		{
			if (auto error = connection_->setListener(this))
			{
				throw std::invalid_argument(error.message);
			}
		}

		Capacity capacity() const noexcept
		{
			const auto peer = connection_->codec().peerMaxConcurrentStreams();
			const auto used = budget_.usage();
			const auto limit = std::min(options_.maxConcurrentStreams, peer);
			const bool accepting = settingsSeen_ && !error_ && !goaway_ && connection_->codec().requestAllowed();
			const auto protocolFree = used.protocolStreams < limit ? limit - used.protocolStreams : 0;
			const auto applicationFree = used.applicationTasks < options_.maxApplicationTasks
											 ? options_.maxApplicationTasks - used.applicationTasks
											 : 0;
			return {settingsSeen_,
					accepting,
					options_.maxConcurrentStreams,
					peer,
					used.protocolStreams,
					accepting ? std::min(protocolFree, applicationFree) : 0};
		}

		void notify()
		{
			settingsWake_.cancel();
			if (capacityChanged_)
			{
				capacityChanged_();
			}
		}

		Awaitable<NetworkError> waitAdmission(TimePoint deadline)
		{
			auto cancellation = co_await boost::asio::this_coro::cancellation_state;
			auto stateError = [&]() -> NetworkError
			{
				if (cancellation.cancelled() != boost::asio::cancellation_type::none)
				{
					return {ErrorCode::hOperationAborted, "HTTP/2 请求准入等待已取消"};
				}
				if (Clock::now() >= deadline)
				{
					return {ErrorCode::hTimedOut, "HTTP/2 请求准入超过原绝对期限"};
				}
				if (error_)
				{
					return error_;
				}
				if (goaway_ || !connection_->codec().requestAllowed())
				{
					return goawayError_ ? goawayError_
										: NetworkError {ErrorCode::hHttp2ConnectionError, "HTTP/2 连接不再接纳请求"};
				}
				return {};
			};
			if (auto error = stateError())
			{
				co_return error;
			}
			if (capacity().available != 0)
			{
				co_return NetworkError {};
			}
			if (admissionWaiters_ >= options_.maxQueuedRequestsPerOrigin)
			{
				co_return NetworkError {ErrorCode::hResourceLimitExceeded, "HTTP/2 等待流槽的有限队列已满"};
			}
			++admissionWaiters_;

			struct Registration
			{
				Impl& impl;

				~Registration()
				{
					--impl.admissionWaiters_;
				}
			} registration {*this};

			const auto now = Clock::now();
			const auto stageDeadline =
				options_.queueTimeout >= TimePoint::max() - now ? TimePoint::max() : now + options_.queueTimeout;
			const auto waitDeadline = std::min(deadline, stageDeadline);
			for (;;)
			{
				boost::system::error_code error;
				co_await settingsWake_.async_wait(
					boost::asio::cancel_at(waitDeadline,
										   boost::asio::redirect_error(boost::asio::use_awaitable, error)));
				if (auto failure = stateError())
				{
					co_return failure;
				}
				if (Clock::now() >= waitDeadline)
				{
					co_return NetworkError {ErrorCode::hTimedOut, "HTTP/2 流槽等待超时"};
				}
				if (error && error != boost::asio::error::operation_aborted)
				{
					co_return toNetworkError(error);
				}
				if (capacity().available != 0)
				{
					co_return NetworkError {};
				}
			}
		}

		void captureError(const std::shared_ptr<Context>& context)
		{
			if (!context->responseReady && !context->result.error && context->stream->error())
			{
				context->result.error = context->stream->error();
			}
		}

		void fail(NetworkError error)
		{
			if (error_)
			{
				return;
			}
			if (!error)
			{
				error = {ErrorCode::hOperationAborted, "HTTP/2 客户端连接已停止"};
			}
			error_ = std::move(error);
			receive_.fail(error_);
			for (const auto& [id, context] : contexts_)
			{
				captureError(context);
			}
			contexts_.clear();
			settingsWake_.cancel();
			notify();
		}

		std::shared_ptr<Context> submit(Request& request, NetworkError& error)
		{
			Http2Headers headers(Http2Codec::HeaderKind::hRequest, 1, options_);
			if ((error = prepareHeaders(request, headers)))
			{
				return {};
			}
			std::vector<Http2Codec::Header> fields {{":method", headers.rawMethod()},
													{":scheme", headers.scheme()},
													{":authority", headers.authority()},
													{":path", headers.target()}};
			for (const auto& field : headers.fields())
			{
				if (!HeaderMap::iequals(field.name, "host"))
				{
					fields.push_back(field);
				}
			}
			if (Clock::now() >= request.deadline)
			{
				error = {ErrorCode::hTimedOut, "HTTP/2 请求在提交协议头前超过原绝对期限"};
				return {};
			}
			auto opened = connection_->output().submitRequest(fields);
			if (opened.error)
			{
				error = std::move(opened.error);
				return {};
			}
			auto context = std::make_shared<Context>();
			context->producer = std::move(opened.producer);
			context->deadline = request.deadline;
			context->result.protocol = HttpProtocol::hHttp2;
			Http2Stream::Config config;
			config.streamId = opened.streamId;
			config.incomingKind = Http2Codec::HeaderKind::hResponse;
			config.peerLimit = connection_->codec().peerMaxConcurrentStreams();
			config.options = options_;
			context->stream = Http2Stream::create(settingsWake_.get_executor(), config, budget_);
			if (!context->stream)
			{
				error = {ErrorCode::hResourceLimitExceeded, "HTTP/2 客户端流或应用配额已满"};
			}
			else
			{
				error = receive_.addClientStream(context->stream, {request.method == HttpMethod::hHead});
			}
			if (!error)
			{
				error = context->producer->appendOwned(std::move(request.body));
			}
			if (!error)
			{
				error = context->producer->finish();
			}
			if (error)
			{
				// 此段不挂起且输出仅排队，writer 尚无执行机会，失败可确定为未发送。
				connection_->output().cancelStream(opened.streamId);
				if (receive_.find(opened.streamId))
				{
					receive_.cancel(opened.streamId);
				}
				else
				{
					(void)connection_->codec().reset(opened.streamId);
				}
				connection_->wakeWriter();
				return {};
			}
			contexts_.emplace(opened.streamId, context);
			notify();
			return context;
		}

		void cancel(const std::shared_ptr<Context>& context, Http2Code code = Http2Code::hCancel)
		{
			const auto id = context->stream->id();
			connection_->output().cancelStream(id);
			if (receive_.find(id))
			{
				receive_.cancel(id, code);
			}
			else if (context->stream->protocolOpen())
			{
				context->stream->reset(code);
				(void)connection_->codec().reset(id, code);
			}
			contexts_.erase(id);
			connection_->wakeWriter();
			notify();
		}

		void retireUpload(const std::shared_ptr<Context>& context)
		{
			if (context->responseReady && context->stream->protocolOpen())
			{
				// 响应已收齐，但上传可能仍在等待窗口。取消剩余上传，保留已收到的响应。
				// 两端都已结束的流不走此分支，仍由 Output 确认尚未写完的 END。
				cancel(context, Http2Code::hNoError);
			}
		}

		Http2Code onBeginHeaders(int32_t id) override
		{
			auto code = receive_.onBeginHeaders(id);
			capture(id);
			return code;
		}

		Http2Code onHeader(int32_t id, Http2Codec::Header header) override
		{
			auto code = receive_.onHeader(id, header);
			capture(id);
			return code;
		}

		Http2Code onEndHeaders(const Http2Codec::HeaderBlock& block) override
		{
			auto code = receive_.onEndHeaders(block);
			auto found = contexts_.find(block.streamId);
			if (found != contexts_.end())
			{
				auto& context = found->second;
				if (context->stream->initialHeaders().sealed() && context->result.status == 0)
				{
					context->result.status = context->stream->initialHeaders().status();
					copyFields(context->stream->initialHeaders(), context->result.headers);
				}
				captureError(context);
			}
			return code;
		}

		Http2Code onData(int32_t id, std::string_view bytes) override
		{
			auto code = receive_.onData(id, bytes);
			capture(id);
			return code;
		}

		void onEndStream(int32_t id) override
		{
			receive_.onEndStream(id);
			auto found = contexts_.find(id);
			if (found == contexts_.end())
			{
				return;
			}
			auto& context = found->second;
			captureError(context);
			if (!context->responseReady && !context->result.error && context->stream->bodyComplete())
			{
				if (auto error = context->stream->publishBody())
				{
					context->result.error = std::move(error);
					return;
				}
				context->result.body = std::move(context->stream->response().native().body);
				copyFields(context->stream->trailers(), context->result.trailers);
				context->completedAt = std::chrono::steady_clock::now();
				// 先保存完整响应，同次 memRecv 后到的 RST 或连接关闭不能覆盖结果。
				context->responseReady = true;
			}
		}

		void onStreamClose(int32_t id, Http2Code code) override
		{
			auto found = contexts_.find(id);
			if (found != contexts_.end() && found->second->resetReceived && code == Http2Code::hNoError
				&& !found->second->responseReady)
			{
				// NO_ERROR 是有效的 RST 原始错误码，不能由普通关闭路径合成为 STREAM_CLOSED。
				NetworkError error {ErrorCode::hHttp2StreamError,
									"HTTP/2 对端已复位未完成的响应",
									Http2ErrorInfo {static_cast<uint32_t>(code), id}};
				if (!found->second->result.error)
				{
					found->second->result.error = error;
				}
				receive_.onError(error);
			}
			receive_.onStreamClose(id, code);
			capture(id);
			contexts_.erase(id);
			notify();
		}

		void onFrameReceived(const Http2Codec::Frame& frame) override
		{
			if (frame.type != Http2Codec::FrameType::hRstStream)
			{
				return;
			}
			// nghttp2 先通知 RST 帧，再通过 streamClose 给出错误码；这里先标记收到 RST。
			// RST(NO_ERROR) 也要取消尚未写完的本地输出。
			connection_->output().cancelStream(frame.streamId);
			auto found = contexts_.find(frame.streamId);
			if (found != contexts_.end())
			{
				found->second->resetReceived = true;
			}
		}

		void onSettings(bool ack) override
		{
			if (!ack)
			{
				settingsSeen_ = true;
				settingsWake_.cancel();
				notify();
			}
		}

		void onGoaway(int32_t lastStreamId, Http2Code code, std::string_view) override
		{
			goaway_ = true;
			lastStreamId_ = std::min(lastStreamId_, lastStreamId);
			NetworkError error {ErrorCode::hHttp2ConnectionError,
								"HTTP/2 对端已发送 GOAWAY",
								Http2ErrorInfo {static_cast<uint32_t>(code), 0}};
			goawayError_ = error;
			std::vector<std::shared_ptr<Context>> refused;
			for (const auto& [id, context] : contexts_)
			{
				if (id > lastStreamId_ && !context->responseReady)
				{
					context->result.requestDisposition = HttpRequestDisposition::hNotProcessed;
					context->result.error = error;
					context->stream->fail(error);
					refused.push_back(context);
				}
			}
			for (const auto& context : refused)
			{
				cancel(context);
			}
			settingsWake_.cancel();
			notify();
		}

		void onError(const NetworkError& error) override
		{
			if (error.http2 && error.http2->streamId > 0)
			{
				auto found = contexts_.find(error.http2->streamId);
				if (found != contexts_.end() && !found->second->responseReady)
				{
					if (error.http2->code == static_cast<uint32_t>(Http2Code::hRefusedStream))
					{
						found->second->result.requestDisposition = HttpRequestDisposition::hNotProcessed;
					}
					if (!found->second->result.error)
					{
						found->second->result.error = error;
					}
				}
				receive_.onError(error);
			}
			else
			{
				fail(error);
			}
		}

		void capture(int32_t id)
		{
			auto found = contexts_.find(id);
			if (found != contexts_.end())
			{
				captureError(found->second);
			}
		}

		Http2Options options_;
		Http2Budget budget_;
		std::shared_ptr<Http2Connection> connection_;
		Http2Receive receive_;
		boost::asio::steady_timer settingsWake_;
		std::unordered_map<int32_t, std::shared_ptr<Context>> contexts_;
		std::function<void()> capacityChanged_;
		NetworkError error_;
		NetworkError goawayError_;
		size_t admissionWaiters_ {0};
		int32_t lastStreamId_ {std::numeric_limits<int32_t>::max()};
		bool settingsSeen_ {false};
		bool goaway_ {false};
	};

	std::shared_ptr<Http2ClientSession> Http2ClientSession::create(std::shared_ptr<TlsSocket> socket,
																   const Http2Options& options)
	{
		if (!socket)
		{
			throw std::invalid_argument("HTTP/2 客户端会话缺少 TLS 传输");
		}
		return std::shared_ptr<Http2ClientSession>(new Http2ClientSession(std::move(socket), options));
	}

	Http2ClientSession::Http2ClientSession(std::shared_ptr<TlsSocket> socket, const Http2Options& options)
		: socket_(std::move(socket)), impl_(std::make_unique<Impl>(*socket_, options))
	{
	}

	Http2ClientSession::~Http2ClientSession() = default;

	Awaitable<NetworkError> Http2ClientSession::run()
	{
		return runOwned(shared_from_this());
	}

	Awaitable<NetworkError> Http2ClientSession::runOwned(std::shared_ptr<Http2ClientSession> self)
	{
		auto error = co_await self->impl_->connection_->run();
		self->impl_->fail(error);
		co_return error;
	}

	Awaitable<HttpClientResult> Http2ClientSession::request(Request request)
	{
		return requestOwned(shared_from_this(), std::move(request));
	}

	Awaitable<HttpClientResult> Http2ClientSession::requestOwned(std::shared_ptr<Http2ClientSession> self,
																 Request request)
	{
		auto& impl = *self->impl_;
		std::shared_ptr<Impl::Context> context;
		try
		{
			auto error = co_await impl.waitAdmission(request.deadline);
			if (error)
			{
				co_return unsent(std::move(error));
			}
			context = impl.submit(request, error);
			if (!context)
			{
				co_return unsent(std::move(error));
			}
			if (Impl::Clock::now() >= request.deadline)
			{
				impl.cancel(context);
				co_return unsent({ErrorCode::hTimedOut, "HTTP/2 请求在首次发送前超过原绝对期限"});
			}

			struct CapacityNotice
			{
				Impl& impl;

				~CapacityNotice()
				{
					impl.notify();
				}
			} notice {impl};

			auto application = context->stream->startApplication();
			if (!application)
			{
				context->result.error = {ErrorCode::hOperationAborted, "HTTP/2 客户端流已结束"};
				impl.cancel(context);
				co_return std::move(context->result);
			}
			error = co_await context->stream->waitBody(request.deadline);
			if (context->responseReady && error)
			{
				// 按首次收齐响应的时刻判断超时，不能因 END 的回调先于定时器执行就算成功。
				context->responseReady = false;
				context->result.error = std::move(error);
				context->result.body.clear();
				context->result.trailers.clear();
			}
			if (!context->responseReady)
			{
				impl.captureError(context);
				if (!context->result.error)
				{
					context->result.error =
						error ? std::move(error) : NetworkError {ErrorCode::hHttp2StreamError, "HTTP/2 响应未完整结束"};
				}
				impl.cancel(context);
			}
			impl.retireUpload(context);
			co_return std::move(context->result);
		}
		catch (const boost::system::system_error& exception)
		{
			if (!context)
			{
				co_return unsent(toNetworkError(exception.code()));
			}
			if (!context->responseReady)
			{
				if (!context->result.error)
				{
					context->result.error = toNetworkError(exception.code());
				}
				impl.cancel(context);
			}
			impl.retireUpload(context);
			co_return std::move(context->result);
		}
	}

	Http2ClientSession::Capacity Http2ClientSession::capacity() const noexcept
	{
		return impl_->capacity();
	}

	void Http2ClientSession::setCapacityChanged(std::function<void()> callback)
	{
		impl_->capacityChanged_ = std::move(callback);
	}

	void Http2ClientSession::close(NetworkError error)
	{
		impl_->connection_->close(std::move(error));
	}

	Http2Connection& Http2ClientSession::connection() noexcept
	{
		return *impl_->connection_;
	}
} // namespace hical::detail

#endif
