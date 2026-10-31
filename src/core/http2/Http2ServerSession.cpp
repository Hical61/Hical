/**
 * @file Http2ServerSession.cpp
 * @brief HTTP/2 服务端会话、共享预算和连接排空
 */

#include "Http2ServerSession.h"

#ifdef HICAL_HAS_HTTP2

	#include "core/Middleware.h"
	#include "core/RequestDispatch.h"
	#include "core/Router.h"
	#include "Http2Response.h"
	#include <boost/asio/co_spawn.hpp>
	#include <boost/asio/post.hpp>
	#include <boost/asio/redirect_error.hpp>
	#include <algorithm>
	#include <array>
	#include <stdexcept>
	#include <utility>

namespace hical::detail
{
	namespace
	{
		HttpResponse bodyFailure(const NetworkError& error)
		{
			auto response = HttpResponse::badRequest(error.message);
			if (error.code == ErrorCode::hResourceLimitExceeded)
			{
				response.setStatus(HttpStatusCode::hPayloadTooLarge);
			}
			else if (error.code == ErrorCode::hTimedOut)
			{
				static constexpr auto kRequestTimeout = static_cast<HttpStatusCode>(408);
				response.setStatus(kRequestTimeout);
			}
			return response;
		}

		HttpResponse notImplemented()
		{
			auto response = HttpResponse::ok("HTTP/2 暂不支持此请求类型");
			response.setStatus(HttpStatusCode::hNotImplemented);
			return response;
		}
	} // namespace

	struct Http2ServerSession::Job
	{
		Job(std::shared_ptr<Http2Stream> owner, boost::asio::any_io_executor executor)
			: stream(std::move(owner)), completion(std::move(executor), Http2Stream::TimePoint::max())
		{
		}

		std::shared_ptr<Http2Stream> stream;
		std::shared_ptr<Http2Output::Producer> producer;
		std::shared_ptr<Http2FileBody> file;
		boost::asio::steady_timer completion;
		Http2Stream::TimePoint responseDeadline {Http2Stream::TimePoint::max()};
		bool responseSubmitted {false};
		bool responseWritten {false};
		bool remoteEnded {false};
		bool peerReset {false};
		bool discardIncoming {false};
		bool collectingDiscardedTrailers {false};
	};

	Http2ServerSession::Http2ServerSession(Context context,
										   boost::asio::any_io_executor executor,
										   const Http2Options& options)
		: context_(std::move(context)), options_(options), jobsJoin_(std::move(executor))
	{
	}

	Http2ServerSession::~Http2ServerSession() = default;

	std::shared_ptr<Http2ServerSession> Http2ServerSession::create(Http2Connection::TlsSocket& socket,
																   const Http2Options& options,
																   Context context)
	{
		struct SharedSession final : Http2ServerSession
		{
			SharedSession(Context context, boost::asio::any_io_executor executor, const Http2Options& options)
				: Http2ServerSession(std::move(context), std::move(executor), options)
			{
			}
		};

		auto self = std::make_shared<SharedSession>(std::move(context), socket.get_executor(), options);
		Http2Budget budget(Http2Budget::Role::hServer, options, self->context_.serviceBudget.get());
		auto weak = std::weak_ptr<Http2ServerSession>(self);
		Http2Connection::Callbacks callbacks;
		callbacks.onActivity = [weak]
		{
			if (auto owner = weak.lock(); owner && owner->context_.onActivity)
			{
				owner->context_.onActivity();
			}
		};
		callbacks.onFailure = [weak](const NetworkError& error)
		{
			if (auto owner = weak.lock())
			{
				owner->transportFailed(error);
			}
		};
		callbacks.onWritten = [weak](int32_t streamId)
		{
			if (auto owner = weak.lock())
			{
				owner->responseWritten(streamId);
			}
		};
		self->connection_ =
			Http2Connection::create(socket, Http2Codec::Role::hServer, options, budget, std::move(callbacks));
		Http2Receive::Callbacks receiveCallbacks;
		receiveCallbacks.onHeadersReady = [weak](std::shared_ptr<Http2Stream> stream)
		{
			if (auto owner = weak.lock())
			{
				owner->headersReady(std::move(stream));
			}
		};
		self->receive_ = std::make_unique<Http2Receive>(socket.get_executor(),
														self->connection_->codec(),
														budget,
														Http2Receive::Config {Http2Codec::Role::hServer, options},
														std::move(receiveCallbacks));
		if (auto error = self->connection_->setListener(self.get()))
		{
			throw std::logic_error(error.message);
		}
		return self;
	}

	Awaitable<NetworkError> Http2ServerSession::run()
	{
		return runOwned(shared_from_this());
	}

	Awaitable<NetworkError> Http2ServerSession::runOwned(std::shared_ptr<Http2ServerSession> self)
	{
		auto error = co_await self->connection_->run();
		self->transportFailed(error);
		// 用户协程自然收尾；取消外层 run 也不能跳过 join 后析构借用的 socket。
		co_await boost::asio::this_coro::throw_if_cancelled(false);
		co_await boost::asio::this_coro::reset_cancellation_state(boost::asio::disable_cancellation());
		while (!self->jobs_.empty())
		{
			self->jobsJoin_.expires_at(Http2Stream::TimePoint::max());
			boost::system::error_code ignored;
			co_await self->jobsJoin_.async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, ignored));
		}
		co_return error;
	}

	void Http2ServerSession::drain()
	{
		if (draining_)
		{
			return;
		}
		draining_ = true;
		if (auto error = connection_->codec().goaway(lastAccepted_))
		{
			connection_->close(std::move(error));
			return;
		}
		connection_->wakeWriter();
		if (jobs_.empty())
		{
			connection_->finish();
		}
	}

	void Http2ServerSession::headersReady(std::shared_ptr<Http2Stream> stream)
	{
		if (draining_)
		{
			// 初始头尚未收齐的流还未接纳，不能越过 GOAWAY 声明的边界执行业务。
			resetStream(stream->id(), Http2Code::hRefusedStream);
			return;
		}
		if (auto error = mapRequest(*stream, context_.peerAddr))
		{
			stream->fail(std::move(error));
			receive_->cancel(stream->id(), Http2Code::hInternalError);
			connection_->wakeWriter();
			return;
		}
		const auto now = Http2Stream::Clock::now();
		const auto deadline = now + std::min(options_.bodyTimeout, Http2Stream::TimePoint::max() - now);
		// 普通 dispatch 入口已同步取得唯一应用 guard，排队期间的 RST 也不能提前归还任务额度。
		auto response = dispatch(stream, deadline);
		auto job = std::make_shared<Job>(stream, jobsJoin_.get_executor());
		const auto id = stream->id();
		jobs_.emplace(id, job);
		lastAccepted_ = std::max(lastAccepted_, id);
		auto self = shared_from_this();
		try
		{
			boost::asio::co_spawn(jobsJoin_.get_executor(),
								  runJob(self, std::move(job), std::move(response)),
								  [self, id](std::exception_ptr exception)
								  {
									  self->jobFinished(id, exception);
								  });
		}
		catch (...)
		{
			jobs_.erase(id);
			throw;
		}
	}

	Awaitable<void> Http2ServerSession::runJob(std::shared_ptr<Http2ServerSession> self,
											   std::shared_ptr<Job> job,
											   Awaitable<HttpResponse> response)
	{
		// co_spawn 可以内联启动；显式 post 确保退出 memRecv 的协议回调后才执行应用与输出。
		co_await boost::asio::post(boost::asio::use_awaitable);
		if (job->peerReset || !self->connection_->running())
		{
			co_return;
		}
		auto result = co_await std::move(response);
		const auto now = Http2Stream::Clock::now();
		// 应用返回时固定；文件生产、等额度和物理 END 都沿用这一期限，控制帧不能延长。
		job->responseDeadline = now + std::min(self->options_.writeTimeout, Http2Stream::TimePoint::max() - now);
		job->completion.expires_at(job->responseDeadline);
		job->discardIncoming = !job->remoteEnded;
		const auto id = job->stream->id();
		if (job->peerReset || !self->connection_->running() || self->receive_->find(id) != job->stream)
		{
			co_return;
		}
		auto submitted = Http2Response::submit(self->connection_->output(),
											   id,
											   job->stream->request().method(),
											   std::move(result),
											   self->options_);
		if (submitted.error)
		{
			self->resetStream(id,
							  submitted.error.code == ErrorCode::hResourceLimitExceeded ? Http2Code::hEnhanceYourCalm
																						: Http2Code::hInternalError);
			co_return;
		}
		job->producer = std::move(submitted.producer);
		job->file = std::move(submitted.file);
		job->responseSubmitted = true;
		self->connection_->wakeWriter();
		if (job->file)
		{
			if (auto error = co_await job->file->run(job->responseDeadline))
			{
				if (!job->peerReset && self->connection_->running())
				{
					job->stream->fail(error);
					self->resetStream(id,
									  error.code == ErrorCode::hTimedOut ? Http2Code::hCancel
																		 : Http2Code::hInternalError);
				}
				co_return;
			}
		}
		while (!job->responseWritten && !job->peerReset && self->connection_->running())
		{
			boost::system::error_code ignored;
			co_await job->completion.async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, ignored));
			if (!ignored && !job->responseWritten && !job->peerReset && self->connection_->running())
			{
				// 单流窗口或生产等待超时只取消本流；TLS 批次无法写出的连接级期限另由 driver 管理。
				job->stream->fail({ErrorCode::hTimedOut, "HTTP/2 响应未在绝对截止点前完成物理发送"});
				self->resetStream(id, Http2Code::hCancel);
				co_return;
			}
		}
	}

	void Http2ServerSession::jobFinished(int32_t streamId, std::exception_ptr exception)
	{
		jobs_.erase(streamId);
		if (exception)
		{
			connection_->close({ErrorCode::hUnknown, "HTTP/2 应用响应任务异常退出"});
		}
		jobsJoin_.cancel();
		if (draining_ && jobs_.empty())
		{
			connection_->finish();
		}
	}

	void Http2ServerSession::responseWritten(int32_t streamId)
	{
		auto found = jobs_.find(streamId);
		if (found == jobs_.end())
		{
			return;
		}
		auto job = found->second;
		job->responseWritten = true;
		job->completion.cancel();
		if (!job->remoteEnded && receive_->find(streamId))
		{
			// 早拒响应 END 已物理写完，才终止尚未发完的请求；不能让 RST 抢先取消响应。
			resetStream(streamId, Http2Code::hNoError);
		}
	}

	void Http2ServerSession::transportFailed(const NetworkError& error)
	{
		if (receive_)
		{
			receive_->fail(error);
		}
		for (auto& [id, job] : jobs_)
		{
			static_cast<void>(id);
			job->completion.cancel();
		}
	}

	void Http2ServerSession::resetStream(int32_t streamId, Http2Code code)
	{
		connection_->output().cancelStream(streamId);
		if (receive_->find(streamId))
		{
			receive_->cancel(streamId, code);
		}
		else if (auto error = connection_->codec().reset(streamId, code))
		{
			connection_->close(std::move(error));
		}
		connection_->wakeWriter();
	}

	NetworkError Http2ServerSession::mapRequest(Http2Stream& stream, const InetAddress& peerAddr)
	{
		if (!stream.protocolOpen())
		{
			return stream.error()
					   ? stream.error()
					   : NetworkError {ErrorCode::hHttp2StreamError,
									   "HTTP/2 已关闭流不能映射请求",
									   Http2ErrorInfo {static_cast<uint32_t>(Http2Code::hStreamClosed), stream.id()}};
		}
		const auto& headers = stream.initialHeaders();
		if (!headers.sealed() || headers.rawMethod().empty() || headers.target().empty())
		{
			return {ErrorCode::hInvalidConfiguration, "HTTP/2 请求映射需要封存的初始请求头"};
		}
		if (!stream.request().target().empty() || !stream.request().body().empty())
		{
			return {ErrorCode::hOperationInProgress, "HTTP/2 请求已经映射或发布 body"};
		}
		static constexpr std::array<std::pair<std::string_view, HttpMethod>, 7> kMethods {
			{{"GET", HttpMethod::hGet},
			 {"POST", HttpMethod::hPost},
			 {"PUT", HttpMethod::hPut},
			 {"DELETE", HttpMethod::hDelete},
			 {"PATCH", HttpMethod::hPatch},
			 {"HEAD", HttpMethod::hHead},
			 {"OPTIONS", HttpMethod::hOptions}}};
		NativeRequest native;
		for (const auto& [raw, method] : kMethods)
		{
			if (headers.rawMethod() == raw)
			{
				native.method = method;
				break;
			}
		}
		native.protocol = HttpProtocol::hHttp2;
		native.target = headers.target();
		native.upgradePreScanned = false;
		native.expectContinue = HeaderMap::iequals(headers.find("expect"), "100-continue");
		static_assert(Http2Options::kMaxHeaderFields <= RequestHeaders::kMaxHeaders);
		for (const auto& field : headers.fields())
		{
			native.headers.add(field.name, field.value);
		}
		auto request = HttpRequest::fromParsed(std::move(native));
		request.setPeerAddr(peerAddr);
		stream.request() = std::move(request);
		return {};
	}

	Awaitable<HttpResponse> Http2ServerSession::dispatch(std::shared_ptr<Http2Stream> stream,
														 Http2Stream::TimePoint bodyDeadline)
	{
		if (!stream)
		{
			throw std::invalid_argument("HTTP/2 分发需要有效请求流");
		}
		// 返回 awaitable 前就持有会话、流和任务额度，延后启动也不会悬空。
		auto application = stream->startApplication();
		return dispatchOwned(shared_from_this(), std::move(stream), bodyDeadline, std::move(application));
	}

	Awaitable<HttpResponse> Http2ServerSession::dispatchOwned(std::shared_ptr<Http2ServerSession> self,
															  std::shared_ptr<Http2Stream> stream,
															  Http2Stream::TimePoint bodyDeadline,
															  Http2Stream::ApplicationGuard application)
	{
		if (stream->error())
		{
			co_return bodyFailure(stream->error());
		}
		if (!application)
		{
			co_return HttpResponse::serverError();
		}
		auto& request = stream->request();
		try
		{
			if (stream->initialHeaders().isConnect() || request.method() == HttpMethod::hUnknown)
			{
				co_return notImplemented();
			}
			auto resolved = self->context_.router.resolveRoute(request);
			if (!resolved.isMatch())
			{
				if (!resolved.pathTooDeep && !resolved.isMethodNotAllowed() && request.method() == HttpMethod::hGet
					&& self->context_.router.findSseRoute(request.path()).route)
				{
					co_return notImplemented();
				}
				co_return co_await self->context_.router.dispatchResolved(request, resolved);
			}
			bool bodyRead = false;
			bool continueSent = false;
			auto readBody = [&]() -> Awaitable<std::optional<HttpResponse>>
			{
				if (!bodyRead)
				{
					if (!continueSent && request.native().expectContinue && !stream->bodyComplete()
						&& bodyDeadline > Http2Stream::Clock::now() && self->connection_->running())
					{
						static constexpr std::array<Http2Codec::Header, 1> kContinue {{{":status", "100"}}};
						if (auto error = self->connection_->codec().submitResponse(stream->id(), kContinue, false))
						{
							co_return bodyFailure(stream->fail(std::move(error)));
						}
						continueSent = true;
						self->connection_->wakeWriter();
					}
					if (auto error = co_await stream->waitBody(bodyDeadline))
					{
						co_return bodyFailure(error);
					}
					if (auto error = stream->publishBody())
					{
						co_return bodyFailure(error);
					}
					bodyRead = true;
				}
				co_return std::nullopt;
			};
			auto tail = [&](HttpRequest& req) -> Awaitable<HttpResponse>
			{
				if (!bodyRead)
				{
					if (auto error = co_await readBody())
					{
						co_return std::move(*error);
					}
				}
				if (auto response = self->context_.router.dispatchSyncResolved(req, resolved))
				{
					co_return std::move(*response);
				}
				co_return co_await self->context_.router.dispatchResolved(req, resolved);
			};
			RequestDispatchContext context;
			context.obj = &tail;
			context.invoke = &tailInvoke<decltype(tail)>;
			context.bodyObj = &readBody;
			context.readBody = &bodyInvoke<decltype(readBody)>;

			struct Attachment
			{
				HttpRequest& request;

				~Attachment()
				{
					RequestDispatchContext::attach(request, nullptr);
				}
			} attachment {request};

			RequestDispatchContext::attach(request, &context);
			if (self->context_.middleware.size() == 0)
			{
				co_return co_await tail(request);
			}
			co_return co_await self->context_.middleware.execute(request);
		}
		catch (const std::exception& error)
		{
			if (self->context_.errorHandler)
			{
				try
				{
					co_return self->context_.errorHandler(error, request);
				}
				catch (...)
				{
					co_return HttpResponse::serverError();
				}
			}
		}
		catch (...)
		{
		}
		// application 在响应产出后归还；响应发送的 stream 保活由调用方另行负责。
		co_return HttpResponse::serverError();
	}

	Http2Code Http2ServerSession::onBeginHeaders(int32_t streamId)
	{
		if (auto* job = discardedRequest(streamId))
		{
			if (job->collectingDiscardedTrailers || job->stream->trailers().sealed())
			{
				return Http2Code::hProtocolError;
			}
			// 固定整个块的收集路径。已在 Receive 开始的块即使跨超时也继续原 pendingHeaders。
			job->collectingDiscardedTrailers = true;
			return Http2Code::hNoError;
		}
		// GOAWAY 只停止接纳新流，已有流仍须接收合法尾头和 END_STREAM 才能自然完成。
		return draining_ && !receive_->find(streamId) ? Http2Code::hRefusedStream : receive_->onBeginHeaders(streamId);
	}

	Http2Code Http2ServerSession::onHeader(int32_t streamId, Http2Codec::Header header)
	{
		if (auto* job = discardedRequest(streamId); job && job->collectingDiscardedTrailers)
		{
			if (auto error = job->stream->trailers().append(header.name, header.value))
			{
				return error.code == ErrorCode::hResourceLimitExceeded ? Http2Code::hEnhanceYourCalm
																	   : Http2Code::hProtocolError;
			}
			return Http2Code::hNoError;
		}
		return receive_->onHeader(streamId, header);
	}

	Http2Code Http2ServerSession::onEndHeaders(const Http2Codec::HeaderBlock& block)
	{
		if (auto* job = discardedRequest(block.streamId); job && job->collectingDiscardedTrailers)
		{
			job->collectingDiscardedTrailers = false;
			if (block.kind != Http2Codec::HeaderKind::hTrailers || !block.endStream)
			{
				return Http2Code::hProtocolError;
			}
			if (auto error = job->stream->trailers().seal())
			{
				return error.code == ErrorCode::hResourceLimitExceeded ? Http2Code::hEnhanceYourCalm
																	   : Http2Code::hProtocolError;
			}
			return Http2Code::hNoError;
		}
		return receive_->onEndHeaders(block);
	}

	Http2ServerSession::Job* Http2ServerSession::discardedRequest(int32_t streamId) const
	{
		auto found = jobs_.find(streamId);
		if (found == jobs_.end())
		{
			return nullptr;
		}
		auto* job = found->second.get();
		return job->discardIncoming && !job->peerReset && !job->remoteEnded && receive_->find(streamId) == job->stream
				   ? job
				   : nullptr;
	}

	Http2Code Http2ServerSession::onData(int32_t streamId, std::string_view bytes)
	{
		if (discardedRequest(streamId))
		{
			// 已决定早拒的请求不再积累 payload；响应写完前仍接收在途帧并归还窗口。
			if (auto error = connection_->codec().consume(streamId, bytes.size()))
			{
				throw std::runtime_error(error.message);
			}
			return Http2Code::hNoError;
		}
		return receive_->onData(streamId, bytes);
	}

	void Http2ServerSession::onEndStream(int32_t streamId)
	{
		if (auto found = jobs_.find(streamId); found != jobs_.end())
		{
			found->second->remoteEnded = true;
			if (found->second->discardIncoming)
			{
				static_cast<void>(found->second->stream->completeBody());
				return;
			}
		}
		receive_->onEndStream(streamId);
	}

	void Http2ServerSession::onStreamClose(int32_t streamId, Http2Code code)
	{
		receive_->onStreamClose(streamId, code);
		if (auto found = jobs_.find(streamId);
			found != jobs_.end() && (code != Http2Code::hNoError || !found->second->responseSubmitted))
		{
			found->second->peerReset = true;
			found->second->completion.cancel();
		}
	}

	void Http2ServerSession::onFrameReceived(const Http2Codec::Frame& frame)
	{
		if (frame.type == Http2Codec::FrameType::hRstStream)
		{
			connection_->output().cancelStream(frame.streamId);
			if (auto found = jobs_.find(frame.streamId); found != jobs_.end())
			{
				found->second->peerReset = true;
				found->second->completion.cancel();
			}
		}
	}

	void Http2ServerSession::onError(const NetworkError& error)
	{
		receive_->onError(error);
	}
} // namespace hical::detail

#endif
