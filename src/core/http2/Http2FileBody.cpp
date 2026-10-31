/**
 * @file Http2FileBody.cpp
 * @brief HTTP/2 文件分块读取、取消和预取预算管理。
 */

#include "Http2FileBody.h"

#ifdef HICAL_HAS_HTTP2

	#include "Http2Headers.h"
	#include <boost/asio/bind_executor.hpp>
	#include <boost/asio/co_spawn.hpp>
	#include <boost/asio/random_access_file.hpp>
	#include <boost/asio/redirect_error.hpp>
	#include <boost/asio/steady_timer.hpp>
	#ifndef BOOST_ASIO_HAS_FILE
		#include <boost/asio/thread_pool.hpp>
		#include <fstream>
	#endif
	#include <algorithm>
	#include <limits>
	#include <optional>
	#include <utility>

namespace hical::detail
{
	namespace
	{
		NetworkError fileError(ErrorCode code = ErrorCode::hHttp2StreamError, int32_t streamId = 0)
		{
			// 取消或分配失败时不再分配错误字符串，避免丢失原错误。
			return {code, {}, Http2ErrorInfo {static_cast<uint32_t>(Http2Code::hInternalError), streamId}};
		}

		void cancelTimer(boost::asio::steady_timer& timer) noexcept
		{
			try
			{
				timer.cancel();
			}
			catch (const boost::system::system_error&)
			{
				// 这里可能由协议回调或清理流程调用，定时器异常不能向外抛。
			}
		}

	#ifndef BOOST_ASIO_HAS_FILE
		constexpr size_t kFileWorkerThreads = 2;

		boost::asio::thread_pool& fileWorkers()
		{
			// 整个进程共用文件线程池；排队文件仍占用连接和服务级任务配额。
			static boost::asio::thread_pool workers(kFileWorkerThreads);
			return workers;
		}

		struct ReadResult
		{
			std::string bytes;
			bool failed {false};
		};

		Awaitable<ReadResult> readOnWorker(std::shared_ptr<std::ifstream> file,
										   std::filesystem::path path,
										   int64_t offset,
										   std::string bytes)
		{
			// 路径和缓冲按值传入协程；文件操作期间不挂起，也不访问所属会话。
			if (!file->is_open())
			{
				file->open(path, std::ios::binary);
			}
			if (!*file)
			{
				co_return ReadResult {{}, true};
			}
			file->seekg(offset);
			file->read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
			const auto count = file->gcount();
			const bool failed = file->bad() || count == 0;
			bytes.resize(static_cast<size_t>(count));
			co_return ReadResult {std::move(bytes), failed};
		}

		Awaitable<void> closeOnWorker(std::shared_ptr<std::ifstream> file)
		{
			if (file->is_open())
			{
				file->close();
			}
			co_return;
		}
	#endif
	} // namespace

	struct Http2FileBody::Impl
	{
		Impl(std::shared_ptr<Http2Output::Producer> source,
			 FileBody description,
			 const Http2Options& options,
			 Http2Budget::Lease application)
			: producer(std::move(source))
			, file(std::move(description))
			, task(std::move(application))
			, prefetch(options.maxFilePrefetchBytes)
		{
		}

		void stop(ErrorCode code) noexcept
		{
			if (complete || error)
			{
				return;
			}
			error = fileError(code, streamId);
	#ifdef BOOST_ASIO_HAS_FILE
			if (handle)
			{
				boost::system::error_code ignored;
				handle->cancel(ignored);
				handle->close(ignored);
			}
	#endif
			producer->cancel();
		}

		std::shared_ptr<Http2Output::Producer> producer;
		FileBody file;
		Http2Budget::Lease task;
		size_t prefetch;
		std::optional<Http2Headers> trailers;
		std::unique_ptr<boost::asio::steady_timer> deadline;
		std::unique_ptr<boost::asio::steady_timer> signal;
	#ifdef BOOST_ASIO_HAS_FILE
		std::unique_ptr<boost::asio::random_access_file> handle;
	#else
		std::shared_ptr<std::ifstream> handle;
	#endif
		NetworkError error;
		int32_t streamId {0};
		bool started {false};
		bool cancelRequested {false};
		bool complete {false};
	};

	Http2FileBody::Http2FileBody(std::unique_ptr<Impl> implementation) : impl_(std::move(implementation))
	{
	}

	Http2FileBody::~Http2FileBody() = default;

	std::shared_ptr<Http2FileBody> Http2FileBody::create(int32_t streamId,
														 std::shared_ptr<Http2Output::Producer> producer,
														 FileBody file,
														 const Http2Options& options,
														 std::span<const Http2Codec::Header> trailers)
	{
		if (!producer || streamId <= 0 || file.offset < 0 || file.length < 0
			|| file.length > std::numeric_limits<int64_t>::max() - file.offset)
		{
			return {};
		}
		auto task = producer->acquireApplicationTask();
		if (!task)
		{
			return {};
		}
		auto implementation = std::make_unique<Impl>(std::move(producer), std::move(file), options, std::move(task));
		implementation->streamId = streamId;
		if (!trailers.empty())
		{
			implementation->trailers.emplace(Http2Codec::HeaderKind::hTrailers, streamId, options);
			for (auto field : trailers)
			{
				if (implementation->trailers->append(field.name, field.value))
				{
					return {};
				}
			}
			if (implementation->trailers->seal())
			{
				return {};
			}
		}
		return std::shared_ptr<Http2FileBody>(new Http2FileBody(std::move(implementation)));
	}

	Awaitable<NetworkError> Http2FileBody::run(std::chrono::steady_clock::time_point deadline)
	{
		return runOwned(shared_from_this(), deadline);
	}

	void Http2FileBody::cancel() noexcept
	{
		impl_->cancelRequested = true;
		if (impl_->signal)
		{
			cancelTimer(*impl_->signal);
		}
	}

	Awaitable<NetworkError> Http2FileBody::runOwned(std::shared_ptr<Http2FileBody> self,
													std::chrono::steady_clock::time_point deadline)
	{
		auto& job = *self->impl_;
		if (job.started)
		{
			co_return fileError(ErrorCode::hOperationInProgress, job.streamId);
		}
		job.started = true;
		co_await boost::asio::this_coro::throw_if_cancelled(false);
		co_await boost::asio::this_coro::reset_cancellation_state(boost::asio::disable_cancellation());
		try
		{
			auto executor = co_await boost::asio::this_coro::executor;
			job.deadline = std::make_unique<boost::asio::steady_timer>(executor, deadline);
			job.signal =
				std::make_unique<boost::asio::steady_timer>(executor, std::chrono::steady_clock::time_point::max());
			auto weak = std::weak_ptr(self);
			job.signal->async_wait(
				[weak](const boost::system::error_code&)
				{
					if (auto owner = weak.lock(); owner && !owner->impl_->complete)
					{
						owner->impl_->stop(ErrorCode::hOperationAborted);
					}
				});
			job.deadline->async_wait(
				[weak](const boost::system::error_code& error)
				{
					if (!error)
					{
						if (auto owner = weak.lock())
						{
							owner->impl_->stop(ErrorCode::hTimedOut);
						}
					}
				});
			job.producer->setCancelHandler(
				[weak]
				{
					if (auto owner = weak.lock())
					{
						owner->cancel();
					}
				});
			if (job.cancelRequested || std::chrono::steady_clock::now() >= deadline)
			{
				job.stop(job.cancelRequested ? ErrorCode::hOperationAborted : ErrorCode::hTimedOut);
			}
			if (!job.error)
			{
				job.error = co_await produce(self);
			}
		}
		catch (const std::bad_alloc&)
		{
			job.error = fileError(ErrorCode::hResourceLimitExceeded);
		}
		catch (...)
		{
			job.error = fileError();
		}
		// produce 已等待唯一 read 回调；此后关闭句柄和撤销定时器不会使在途 buffer 悬垂。
		job.complete = true;
		job.producer->setCancelHandler({});
		boost::system::error_code ignored;
		if (job.deadline)
		{
			cancelTimer(*job.deadline);
		}
		if (job.signal)
		{
			cancelTimer(*job.signal);
		}
	#ifdef BOOST_ASIO_HAS_FILE
		if (job.handle)
		{
			job.handle->close(ignored);
		}
	#else
		if (job.handle)
		{
			try
			{
				auto executor = co_await boost::asio::this_coro::executor;
				co_await boost::asio::co_spawn(fileWorkers(),
											   closeOnWorker(job.handle),
											   boost::asio::bind_executor(executor, boost::asio::use_awaitable));
			}
			catch (...)
			{
				if (!job.error)
				{
					job.error = fileError();
				}
			}
		}
	#endif
		if (job.error)
		{
			job.producer->cancel();
			if (job.error.http2)
			{
				job.error.http2->streamId = job.streamId;
			}
			else
			{
				job.error.http2 = Http2ErrorInfo {static_cast<uint32_t>(Http2Code::hInternalError), job.streamId};
			}
		}
		co_return job.error;
	}

	Awaitable<NetworkError> Http2FileBody::produce(std::shared_ptr<Http2FileBody> self)
	{
		auto& job = *self->impl_;
		auto executor = co_await boost::asio::this_coro::executor;
		auto remaining = job.file.length;
		auto offset = job.file.offset;
		while (remaining > 0 && !job.error)
		{
			auto reservation =
				co_await job.producer->reserveRead(std::min(job.prefetch, static_cast<size_t>(remaining)),
												   job.deadline->expiry());
			if (reservation.error)
			{
				co_return job.error ? job.error : reservation.error;
			}
			std::string bytes(reservation.permit.size(), '\0');
	#ifdef BOOST_ASIO_HAS_FILE
			if (!job.handle)
			{
				job.handle = std::make_unique<boost::asio::random_access_file>(executor);
				boost::system::error_code error;
				job.handle->open(job.file.path.string(), boost::asio::random_access_file::read_only, error);
				if (error)
				{
					co_return fileError();
				}
			}
			boost::system::error_code error;
			const auto count =
				co_await job.handle->async_read_some_at(static_cast<uint64_t>(offset),
														boost::asio::buffer(bytes),
														boost::asio::redirect_error(boost::asio::use_awaitable, error));
			if (job.error || error || count == 0)
			{
				co_return job.error ? job.error : fileError();
			}
			bytes.resize(count);
	#else
			if (!job.handle)
			{
				job.handle = std::make_shared<std::ifstream>();
			}
			auto result =
				co_await boost::asio::co_spawn(fileWorkers(),
											   readOnWorker(job.handle, job.file.path, offset, std::move(bytes)),
											   boost::asio::bind_executor(executor, boost::asio::use_awaitable));
			if (job.error || result.failed)
			{
				co_return job.error ? job.error : fileError();
			}
			bytes = std::move(result.bytes);
			const auto count = bytes.size();
	#endif
			offset += static_cast<int64_t>(count);
			remaining -= static_cast<int64_t>(count);
			if (auto error = job.producer->appendReserved(std::move(bytes), std::move(reservation.permit)))
			{
				co_return error;
			}
		}
		if (job.error)
		{
			co_return job.error;
		}
		co_return job.producer->finish(job.trailers ? job.trailers->fields() : std::span<const Http2Codec::Header> {});
	}
} // namespace hical::detail

#endif
