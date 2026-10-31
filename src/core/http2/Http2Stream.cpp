/**
 * @file Http2Stream.cpp
 * @brief HTTP/2 流终结、可取消 body 等待与应用资源收尾
 */

#include "Http2Stream.h"

#ifdef HICAL_HAS_HTTP2

	#include <boost/asio/redirect_error.hpp>
	#include <boost/asio/use_awaitable.hpp>
	#include <boost/system/system_error.hpp>
	#include <utility>

namespace hical::detail
{
	struct Http2Stream::Reservations
	{
		Http2Budget::Lease protocol;
		Http2Budget::Lease application;
		Http2Budget::Lease receive;
		Http2Budget::Lease send;
	};

	std::shared_ptr<Http2Stream> Http2Stream::create(boost::asio::any_io_executor executor,
													 const Config& config,
													 Http2Budget budget)
	{
		config.options.validate();
		if (config.streamId <= 0
			|| (config.incomingKind != Http2Codec::HeaderKind::hRequest
				&& config.incomingKind != Http2Codec::HeaderKind::hResponse))
		{
			throw std::invalid_argument("HTTP/2 流 ID 或接收角色无效");
		}
		Reservations reserved;
		reserved.protocol = budget.tryAcquireProtocolStream(config.peerLimit);
		if (!reserved.protocol)
		{
			return {};
		}
		reserved.application = budget.tryAcquireApplicationTask();
		if (!reserved.application)
		{
			return {};
		}
		reserved.receive = budget.tryAcquireReceive();
		reserved.send = budget.tryAcquireSend();

		struct SharedStream final : Http2Stream
		{
			SharedStream(boost::asio::any_io_executor executor, const Config& config, Reservations reserved)
				: Http2Stream(std::move(executor), config, std::move(reserved))
			{
			}
		};

		return std::make_shared<SharedStream>(std::move(executor), config, std::move(reserved));
	}

	Http2Stream::Http2Stream(boost::asio::any_io_executor executor, const Config& config, Reservations reserved)
		: executor_(std::move(executor))
		, id_(config.streamId)
		, incomingKind_(config.incomingKind)
		, initialHeaders_(config.incomingKind, id_, config.options)
		, trailers_(Http2Codec::HeaderKind::hTrailers, id_, config.options)
		, protocol_(std::move(reserved.protocol))
		, application_(std::move(reserved.application))
		, receive_(std::move(reserved.receive))
		, send_(std::move(reserved.send))
	{
		request_.native().protocol = HttpProtocol::hHttp2;
	}

	Http2Stream::~Http2Stream() = default;

	Http2Stream::ApplicationGuard::ApplicationGuard(std::shared_ptr<Http2Stream> stream) noexcept
		: stream_(std::move(stream))
	{
	}

	Http2Stream::ApplicationGuard::~ApplicationGuard()
	{
		reset();
	}

	Http2Stream::ApplicationGuard::ApplicationGuard(ApplicationGuard&& other) noexcept = default;

	Http2Stream::ApplicationGuard& Http2Stream::ApplicationGuard::operator=(ApplicationGuard&& other) noexcept
	{
		if (this != &other)
		{
			reset();
			stream_ = std::move(other.stream_);
		}
		return *this;
	}

	Http2Stream::ApplicationGuard::operator bool() const noexcept
	{
		return static_cast<bool>(stream_);
	}

	void Http2Stream::ApplicationGuard::reset() noexcept
	{
		if (stream_)
		{
			stream_->finishApplication();
			stream_.reset();
		}
	}

	Http2Stream::ApplicationGuard Http2Stream::startApplication()
	{
		if (!protocol_ || applicationStarted_ || applicationFinished_)
		{
			return {};
		}
		applicationStarted_ = true;
		return ApplicationGuard(shared_from_this());
	}

	void Http2Stream::finishApplication() noexcept
	{
		applicationFinished_ = true;
		application_.reset();
	}

	NetworkError Http2Stream::appendBody(std::string_view bytes)
	{
		if (error_)
		{
			return error_;
		}
		if (bodyComplete_ || !protocol_ || !initialHeaders_.sealed())
		{
			NetworkError error {ErrorCode::hHttp2StreamError,
								"HTTP/2 DATA 不在有效的 body 接收阶段",
								Http2ErrorInfo {static_cast<uint32_t>(Http2Code::hStreamClosed), id_}};
			return protocol_ ? fail(std::move(error)) : error;
		}
		const auto previous = receive_.amount();
		if (bytes.size() > pendingBody_.max_size() - previous || !receive_.tryResize(previous + bytes.size()))
		{
			return fail({ErrorCode::hResourceLimitExceeded,
						 "HTTP/2 接收 body 超过流或连接预算",
						 Http2ErrorInfo {static_cast<uint32_t>(Http2Code::hEnhanceYourCalm), id_}});
		}
		try
		{
			pendingBody_.append(bytes);
		}
		catch (...)
		{
			[[maybe_unused]] auto restored = receive_.tryResize(previous);
			assert(restored);
			throw;
		}
		return {};
	}

	NetworkError Http2Stream::completeBody()
	{
		if (bodyComplete_)
		{
			return bodyFailed_ ? error_ : NetworkError {};
		}
		if (!initialHeaders_.sealed())
		{
			return fail({ErrorCode::hHttp2StreamError,
						 "HTTP/2 初始头未完成便结束 body",
						 Http2ErrorInfo {static_cast<uint32_t>(Http2Code::hProtocolError), id_}});
		}
		completeBodyOnce(false);
		return {};
	}

	NetworkError Http2Stream::publishBody()
	{
		if (error_)
		{
			return error_;
		}
		if (!bodyComplete_)
		{
			return {ErrorCode::hOperationInProgress, "HTTP/2 body 尚未接收完成"};
		}
		if (bodyPublished_)
		{
			return {};
		}
		if (incomingKind_ == Http2Codec::HeaderKind::hRequest)
		{
			request_.acceptBody(std::move(pendingBody_));
		}
		else
		{
			response_.native().body = std::move(pendingBody_);
		}
		bodyPublished_ = true;
		return {};
	}

	void Http2Stream::completeBodyOnce(bool failed)
	{
		if (!bodyComplete_)
		{
			bodyCompletedAt_ = Clock::now();
			bodyFailed_ = failed;
			bodyComplete_ = true;
			for (auto& timer : bodyWaiters_)
			{
				timer->cancel();
			}
		}
	}

	NetworkError Http2Stream::bodyResult(TimePoint deadline)
	{
		if (bodyFailed_)
		{
			return error_;
		}
		if (bodyCompletedAt_ <= deadline)
		{
			return {};
		}
		NetworkError timeout {ErrorCode::hTimedOut, "HTTP/2 body 在绝对截止点之后才接收完成"};
		if (!protocol_)
		{
			// 正常协议关闭的首次结果保持不变，此次等待仍须遵守调用方传入的期限。
			return timeout;
		}
		fail(std::move(timeout));
		// END 已置完成标记，fail 不会再运行 completeBodyOnce；等待结果仍必须记为失败。
		bodyFailed_ = true;
		return error_;
	}

	NetworkError Http2Stream::fail(NetworkError error)
	{
		if (!protocol_)
		{
			return error_;
		}
		if (!error)
		{
			error = {ErrorCode::hOperationAborted, "HTTP/2 流已取消"};
		}
		error_ = std::move(error);
		protocol_.reset();
		if (!applicationStarted_)
		{
			finishApplication();
		}
		completeBodyOnce(true);
		return error_;
	}

	void Http2Stream::reset(Http2Code code)
	{
		fail({ErrorCode::hHttp2StreamError, "HTTP/2 流已复位", Http2ErrorInfo {static_cast<uint32_t>(code), id_}});
	}

	void Http2Stream::closeProtocol()
	{
		if (!bodyComplete_)
		{
			fail({ErrorCode::hHttp2StreamError,
				  "HTTP/2 流在 body 完成前关闭",
				  Http2ErrorInfo {static_cast<uint32_t>(Http2Code::hStreamClosed), id_}});
			return;
		}
		protocol_.reset();
		if (!applicationStarted_)
		{
			finishApplication();
		}
	}

	boost::asio::awaitable<NetworkError> Http2Stream::waitBody(TimePoint deadline)
	{
		// 此函数本身不挂起，避免调用方在惰性协程首次运行前销毁最后一个所有者。
		return waitBodyOwned(shared_from_this(), deadline);
	}

	boost::asio::awaitable<NetworkError> Http2Stream::waitBodyOwned(std::shared_ptr<Http2Stream> stream,
																	TimePoint deadline)
	{
		if (stream->bodyComplete_)
		{
			co_return stream->bodyResult(deadline);
		}
		auto timer = std::make_shared<boost::asio::steady_timer>(stream->executor_, deadline);
		const auto position = stream->bodyWaiters_.insert(stream->bodyWaiters_.end(), timer);

		struct Registration
		{
			Http2Stream& stream;
			decltype(position) entry;

			~Registration()
			{
				stream.bodyWaiters_.erase(entry);
			}
		} registration {*stream, position};

		boost::system::error_code error;
		try
		{
			co_await timer->async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, error));
		}
		catch (const boost::system::system_error& exception)
		{
			error = exception.code();
		}
		if (!stream->bodyComplete_)
		{
			stream->fail(error ? toNetworkError(error) : NetworkError {ErrorCode::hTimedOut, "HTTP/2 body 等待超时"});
		}
		co_return stream->bodyResult(deadline);
	}

	bool Http2Stream::reserveSend(size_t bytes) noexcept
	{
		return (protocol_ || bytes <= send_.amount()) && send_.tryResize(bytes);
	}

	int32_t Http2Stream::id() const noexcept
	{
		return id_;
	}

	Http2Headers& Http2Stream::initialHeaders() noexcept
	{
		return initialHeaders_;
	}

	Http2Headers& Http2Stream::trailers() noexcept
	{
		return trailers_;
	}

	HttpRequest& Http2Stream::request() noexcept
	{
		return request_;
	}

	HttpResponse& Http2Stream::response() noexcept
	{
		return response_;
	}

	bool Http2Stream::protocolOpen() const noexcept
	{
		return static_cast<bool>(protocol_);
	}

	bool Http2Stream::bodyComplete() const noexcept
	{
		return bodyComplete_;
	}

	size_t Http2Stream::receivedBytes() const noexcept
	{
		return receive_.amount();
	}

	const NetworkError& Http2Stream::error() const noexcept
	{
		return error_;
	}

	std::string_view Http2Stream::body() const noexcept
	{
		if (!bodyComplete_ || bodyFailed_)
		{
			return {};
		}
		if (!bodyPublished_)
		{
			return pendingBody_;
		}
		return incomingKind_ == Http2Codec::HeaderKind::hRequest ? request_.body() : response_.body();
	}
} // namespace hical::detail

#endif
