/**
 * @file Http2Connection.cpp
 * @brief HTTP/2 TLS 收发、取消和子任务收尾
 */

#include "Http2Connection.h"

#ifdef HICAL_HAS_HTTP2

	#include "core/Log.h"
	#include <boost/asio/co_spawn.hpp>
	#include <boost/asio/redirect_error.hpp>
	#include <boost/asio/this_coro.hpp>
	#include <boost/asio/use_awaitable.hpp>
	#include <array>
	#include <utility>

namespace hical::detail
{
	namespace
	{
		constexpr size_t kReadBufferBytes = 16 * 1024;
		using Clock = std::chrono::steady_clock;

		Clock::time_point deadlineAfter(Clock::duration duration)
		{
			const auto now = Clock::now();
			return duration > Clock::time_point::max() - now ? Clock::time_point::max() : now + duration;
		}

		NetworkError pumpException()
		{
			return {ErrorCode::hHttp2ConnectionError, "HTTP/2 连接收发或完成回调抛出异常"};
		}
	} // namespace

	std::shared_ptr<Http2Connection> Http2Connection::create(TlsSocket& socket,
															 Http2Codec::Role role,
															 const Http2Options& options,
															 Http2Budget budget,
															 Callbacks callbacks)
	{
		auto self = std::shared_ptr<Http2Connection>(
			new Http2Connection(socket, role, options, std::move(budget), std::move(callbacks)));
		self->output_.setWakeup(
			[weak = std::weak_ptr(self)]
			{
				if (auto owner = weak.lock())
				{
					owner->wakeWriter();
				}
			});
		return self;
	}

	Http2Connection::Http2Connection(TlsSocket& socket,
									 Http2Codec::Role role,
									 const Http2Options& options,
									 Http2Budget budget,
									 Callbacks callbacks)
		: socket_(socket)
		, options_(options)
		, callbacks_(std::move(callbacks))
		, codec_(role, options_, this)
		, output_(codec_, std::move(budget), options_)
		, writerWake_(socket.get_executor())
		, writerJoin_(socket.get_executor())
		, negotiationTimer_(socket.get_executor())
		, headerTimer_(socket.get_executor())
		, writeTimer_(socket.get_executor())
	{
	}

	Http2Connection::~Http2Connection() = default;

	NetworkError Http2Connection::setListener(Http2Codec::Listener* listener)
	{
		if (started_)
		{
			return {ErrorCode::hOperationInProgress, "HTTP/2 连接启动后不能替换角色监听器"};
		}
		listener_ = listener;
		return {};
	}

	Awaitable<NetworkError> Http2Connection::run()
	{
		return runOwned(shared_from_this());
	}

	Awaitable<NetworkError> Http2Connection::runOwned(std::shared_ptr<Http2Connection> self)
	{
		if (self->started_)
		{
			co_return NetworkError {ErrorCode::hOperationInProgress, "HTTP/2 连接收发泵只能运行一次"};
		}
		self->started_ = true;
		if (self->stopped_)
		{
			co_return self->error_;
		}
		try
		{
			self->negotiationTimer_.expires_at(deadlineAfter(self->options_.negotiationTimeout));
			self->negotiationTimer_.async_wait(
				[weak = std::weak_ptr(self)](const boost::system::error_code& error)
				{
					if (auto owner = weak.lock(); owner && !error && !owner->peerSettings_)
					{
						owner->close({ErrorCode::hTimedOut, "HTTP/2 等待对端 preface/SETTINGS 超时"});
					}
				});
			self->writerActive_ = true;
			try
			{
				boost::asio::co_spawn(self->socket_.get_executor(),
									  writeOwned(self),
									  [self](std::exception_ptr exception)
									  {
										  self->writerDone(exception);
									  });
			}
			catch (...)
			{
				self->writerActive_ = false;
				throw;
			}
			co_await self->readLoop();
		}
		catch (const boost::system::system_error& exception)
		{
			self->close(toNetworkError(exception.code()));
		}
		catch (...)
		{
			self->close(pumpException());
		}
		if (!self->stopped_)
		{
			self->stopTransport({}, false);
		}

		// 父请求取消只能结束收发，不能跳过 join 后让外层 socket 提前析构。
		co_await boost::asio::this_coro::throw_if_cancelled(false);
		co_await boost::asio::this_coro::reset_cancellation_state(boost::asio::disable_cancellation());
		while (self->writerActive_)
		{
			self->writerJoin_.expires_at(Clock::time_point::max());
			boost::system::error_code ignored;
			co_await self->writerJoin_.async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, ignored));
		}
		co_return self->error_;
	}

	Awaitable<void> Http2Connection::writeOwned(std::shared_ptr<Http2Connection> self)
	{
		co_await self->writeLoop();
	}

	void Http2Connection::writerDone(std::exception_ptr exception)
	{
		if (exception)
		{
			close(pumpException());
		}
		writerActive_ = false;
		writerJoin_.cancel();
	}

	Awaitable<void> Http2Connection::readLoop()
	{
		std::array<char, kReadBufferBytes> buffer;
		while (!stopped_)
		{
			boost::system::error_code error;
			const auto count =
				co_await socket_.async_read_some(boost::asio::buffer(buffer),
												 boost::asio::redirect_error(boost::asio::use_awaitable, error));
			if (error || count == 0)
			{
				if (!stopped_)
				{
					close(error ? toNetworkError(error) : NetworkError {ErrorCode::hEof, "HTTP/2 对端关闭连接"});
				}
				co_return;
			}
			touch();
			std::string_view bytes(buffer.data(), count);
			while (!bytes.empty() && !stopped_)
			{
				const auto received = codec_.memRecv(bytes);
				if (received.error || received.consumed == 0)
				{
					close(received.error ? received.error
										 : NetworkError {ErrorCode::hHttp2ConnectionError, "HTTP/2 入站解析没有推进"});
					co_return;
				}
				bytes.remove_prefix(received.consumed);
			}
			wakeWriter();
		}
	}

	Awaitable<void> Http2Connection::writeLoop()
	{
		while (!stopped_)
		{
			auto batch = output_.nextBatch();
			if (batch.error)
			{
				close(std::move(batch.error));
				co_return;
			}
			if (batch.bytes.empty())
			{
				if ((finishing_ && !output_.hasPendingOutput()) || !codec_.wantRead())
				{
					stopTransport({}, false);
					co_return;
				}
				writerWake_.expires_at(Clock::time_point::max());
				boost::system::error_code ignored;
				co_await writerWake_.async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, ignored));
				continue;
			}
			const auto generation = ++writeGeneration_;
			writeTimer_.expires_at(deadlineAfter(options_.writeTimeout));
			writeTimer_.async_wait(
				[weak = weak_from_this(), generation](const boost::system::error_code& error)
				{
					if (auto owner = weak.lock(); owner && !error && generation == owner->writeGeneration_)
					{
						owner->close({ErrorCode::hTimedOut, "HTTP/2 输出批次写入超时"});
					}
				});
			co_await writeBatch(batch.bytes);
			++writeGeneration_;
			writeTimer_.cancel();
		}
	}

	Awaitable<void> Http2Connection::writeBatch(std::string_view bytes)
	{
		while (!bytes.empty() && !stopped_)
		{
			boost::system::error_code error;
			const auto count =
				co_await socket_.async_write_some(boost::asio::buffer(bytes.data(), bytes.size()),
												  boost::asio::redirect_error(boost::asio::use_awaitable, error));
			if (error || count == 0)
			{
				close(error ? toNetworkError(error) : NetworkError {ErrorCode::hBrokenPipe, "HTTP/2 写入没有推进"});
				co_return;
			}
			const auto remaining = bytes.size() - count;
			auto acknowledged = output_.acknowledge(count);
			if (acknowledged.error)
			{
				close(std::move(acknowledged.error));
				co_return;
			}
			touch();
			for (const auto streamId : acknowledged.endedStreams)
			{
				if (callbacks_.onWritten)
				{
					callbacks_.onWritten(streamId);
				}
			}
			if (remaining == 0 || stopped_)
			{
				co_return;
			}
			bytes = output_.nextBatch().bytes;
		}
	}

	void Http2Connection::stopTransport(NetworkError error, bool notify)
	{
		if (stopped_)
		{
			return;
		}
		stopped_ = true;
		if (protocolError_)
		{
			error = std::move(protocolError_);
			notify = true;
		}
		error_ = std::move(error);
		++writeGeneration_;
		++headerGeneration_;
		headerStreamId_ = 0;
		negotiationTimer_.cancel();
		headerTimer_.cancel();
		writeTimer_.cancel();
		writerWake_.cancel();
		const bool cleanExit = !notify && !error_ && !output_.hasPendingOutput();
		output_.close();
		boost::system::error_code ignored;
		if (cleanExit)
		{
			// 全部 TLS 写完成后沿用外层 SocketGuard 的正常半关闭，避免 close 丢弃末尾响应。
			// 不启动 SSL shutdown；随后 close 取消 reader，run 继续等待两个泵全部退出。
			socket_.next_layer().shutdown(boost::asio::ip::tcp::socket::shutdown_send, ignored);
		}
		socket_.next_layer().close(ignored);
		if (notify && callbacks_.onFailure)
		{
			try
			{
				callbacks_.onFailure(error_);
			}
			catch (...)
			{
				HICAL_LOG_ERROR("HTTP/2 连接故障回调抛出异常");
			}
		}
	}

	void Http2Connection::close(NetworkError error)
	{
		if (!error)
		{
			error = {ErrorCode::hOperationAborted, "HTTP/2 连接主动关闭"};
		}
		stopTransport(std::move(error), true);
	}

	void Http2Connection::finish()
	{
		finishing_ = true;
		wakeWriter();
	}

	void Http2Connection::wakeWriter()
	{
		if (!stopped_)
		{
			writerWake_.cancel();
		}
	}

	void Http2Connection::touch()
	{
		if (callbacks_.onActivity && !stopped_)
		{
			callbacks_.onActivity();
		}
	}

	Http2Codec& Http2Connection::codec() noexcept
	{
		return codec_;
	}

	Http2Output& Http2Connection::output() noexcept
	{
		return output_;
	}

	bool Http2Connection::running() const noexcept
	{
		return started_ && !stopped_;
	}

	const NetworkError& Http2Connection::error() const noexcept
	{
		return error_;
	}

	Http2Code Http2Connection::onBeginHeaders(int32_t streamId)
	{
		return listener_ ? listener_->onBeginHeaders(streamId) : Http2Code::hNoError;
	}

	Http2Code Http2Connection::onHeader(int32_t streamId, Http2Codec::Header header)
	{
		return listener_ ? listener_->onHeader(streamId, header) : Http2Code::hNoError;
	}

	Http2Code Http2Connection::onEndHeaders(const Http2Codec::HeaderBlock& block)
	{
		return listener_ ? listener_->onEndHeaders(block) : Http2Code::hNoError;
	}

	Http2Code Http2Connection::onData(int32_t streamId, std::string_view bytes)
	{
		return listener_ ? listener_->onData(streamId, bytes) : Http2Code::hNoError;
	}

	void Http2Connection::onEndStream(int32_t streamId)
	{
		if (listener_)
		{
			listener_->onEndStream(streamId);
		}
	}

	void Http2Connection::onStreamClose(int32_t streamId, Http2Code code)
	{
		output_.onStreamClose(streamId, code);
		if (listener_)
		{
			listener_->onStreamClose(streamId, code);
		}
	}

	void Http2Connection::onHeaderBlock(int32_t streamId, bool complete)
	{
		if (complete && headerStreamId_ == streamId)
		{
			headerStreamId_ = 0;
			++headerGeneration_;
			headerTimer_.cancel();
		}
		else if (!complete && !stopped_ && headerStreamId_ == 0 && streamId > 0)
		{
			headerStreamId_ = streamId;
			const auto generation = ++headerGeneration_;
			headerTimer_.expires_at(deadlineAfter(options_.headerTimeout));
			headerTimer_.async_wait(
				[weak = weak_from_this(), generation](const boost::system::error_code& error)
				{
					if (auto owner = weak.lock(); owner && !error && !owner->stopped_
												  && owner->headerGeneration_ == generation
												  && owner->headerStreamId_ != 0)
					{
						owner->close({ErrorCode::hTimedOut, "HTTP/2 头部块未在绝对截止点前收齐"});
					}
				});
		}
		if (listener_)
		{
			listener_->onHeaderBlock(streamId, complete);
		}
	}

	void Http2Connection::onFrameReceived(const Http2Codec::Frame& frame)
	{
		if (listener_)
		{
			listener_->onFrameReceived(frame);
		}
	}

	void Http2Connection::onFrameSent(const Http2Codec::Frame& frame)
	{
		if (listener_)
		{
			listener_->onFrameSent(frame);
		}
	}

	void Http2Connection::onSettings(bool ack)
	{
		if (!ack && !peerSettings_)
		{
			peerSettings_ = true;
			negotiationTimer_.cancel();
		}
		if (listener_)
		{
			listener_->onSettings(ack);
		}
	}

	void Http2Connection::onGoaway(int32_t lastStreamId, Http2Code code, std::string_view debugData)
	{
		if (listener_)
		{
			listener_->onGoaway(lastStreamId, code, debugData);
		}
	}

	void Http2Connection::onError(const NetworkError& error)
	{
		if (!stopped_ && !protocolError_ && error.http2 && error.http2->streamId == 0)
		{
			// 先保留 GOAWAY 的协议错误，等 writer 确认实际写出后再结束传输。
			protocolError_ = error;
		}
		if (listener_)
		{
			listener_->onError(error);
		}
	}
} // namespace hical::detail

#endif
