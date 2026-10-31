/**
 * @file Http2Output.cpp
 * @brief 拥有型 DATA 队列、连接 wire 额度及实际发送帧边界
 */

#include "Http2Output.h"

#ifdef HICAL_HAS_HTTP2

	#include "Http2Headers.h"
	#include <boost/asio/redirect_error.hpp>
	#include <boost/asio/steady_timer.hpp>
	#include <algorithm>
	#include <array>
	#include <cstring>
	#include <deque>
	#include <limits>
	#include <optional>
	#include <stdexcept>
	#include <unordered_map>
	#include <unordered_set>
	#include <utility>

namespace hical::detail
{
	namespace
	{
		constexpr size_t kFrameHeaderBytes = 9;
		constexpr size_t kClientPrefaceBytes = 24;
		constexpr size_t kMinimumWireCapacity = 128;
		constexpr size_t kHeaderEncodingSlack = 32;
		constexpr size_t kMaxProducerChunks = 64;
		constexpr uint8_t kEndHeaders = 0x04;

		NetworkError cancelledError(int32_t streamId = 0)
		{
			return {ErrorCode::hOperationAborted,
					"HTTP/2 输出已停止",
					Http2ErrorInfo {static_cast<uint32_t>(Http2Code::hCancel), streamId}};
		}

		NetworkError resourceError(int32_t streamId = 0)
		{
			return {ErrorCode::hResourceLimitExceeded,
					"HTTP/2 发送队列额度不足",
					Http2ErrorInfo {static_cast<uint32_t>(Http2Code::hEnhanceYourCalm), streamId}};
		}

		NetworkError allocationError(int32_t streamId = 0) noexcept
		{
			return {ErrorCode::hResourceLimitExceeded,
					{},
					Http2ErrorInfo {static_cast<uint32_t>(Http2Code::hInternalError), streamId}};
		}
	} // namespace

	struct Http2Output::State
	{
		State(Http2Codec& engine, Http2Budget connectionBudget, const Http2Options& configuration)
			: codec(&engine), budget(std::move(connectionBudget)), options(configuration)
		{
			options.validate();
			// 头部预算每项预留了 32 字节，可覆盖字面量和整数编码；另预留动态表更新和帧头空间。
			const auto headerFrames = options.maxHeaderBytes / Http2Options::kMinFrameBytes + 1;
			const auto headerOverhead = kHeaderEncodingSlack + headerFrames * kFrameHeaderBytes;
			if (options.maxHeaderBytes > options.maxConnectionSendBytes
				|| headerOverhead >= options.maxConnectionSendBytes - options.maxHeaderBytes)
			{
				throw std::invalid_argument("HTTP/2 连接待发预算不足以容纳有界头部和 payload 推进空间");
			}
			wireCapacity =
				std::max({kMinimumWireCapacity, options.maxOutputBatchBytes, options.maxHeaderBytes + headerOverhead});
			if (wireCapacity >= options.maxConnectionSendBytes)
			{
				throw std::invalid_argument("HTTP/2 wire 预留必须小于连接待发预算，以保证 payload 可推进");
			}
		}

		void wake() const
		{
			// 回调可替换自身；本次调用独立保活函数对象。
			auto callback = wakeup;
			if (callback)
			{
				(*callback)();
			}
		}

		void notifyReaders() noexcept
		{
			for (auto it = readers.begin(); it != readers.end();)
			{
				if (auto timer = it->lock())
				{
					try
					{
						timer->cancel();
					}
					catch (const boost::system::system_error&)
					{
						// 保持 C 回调和额度归还无异常；绝对期限仍会结束等待。
					}
					++it;
				}
				else
				{
					it = readers.erase(it);
				}
			}
		}

		Http2Codec* codec;
		Http2Budget budget;
		Http2Options options;
		size_t wireCapacity {0};
		size_t queuedBytes {0};
		bool closed {false};
		bool pumping {false};
		bool readCredit {false};
		int32_t selectedStream {0};
		int32_t lastReadStream {0};
		std::shared_ptr<const std::function<void()>> wakeup;
		std::unordered_map<int32_t, std::weak_ptr<Producer>> producers;
		std::unordered_set<int32_t> pendingEnds;
		std::vector<std::weak_ptr<boost::asio::steady_timer>> readers;
	};

	struct Http2Output::Producer::Impl
	{
		explicit Impl(std::shared_ptr<State> connection)
			: state(std::move(connection)), lease(state->budget.tryAcquireSend())
		{
		}

		NetworkError canProduce() const
		{
			if (state->closed || cancelled)
			{
				return cancelledError(streamId);
			}
			if (finished)
			{
				return {ErrorCode::hInvalidConfiguration, "HTTP/2 DATA 生产者已经结束"};
			}
			return {};
		}

		bool resize(size_t bytes) noexcept
		{
			const auto previous = lease.amount();
			if (!lease.tryResize(bytes))
			{
				return false;
			}
			state->queuedBytes -= previous;
			state->queuedBytes += bytes;
			return true;
		}

		NetworkError ready()
		{
			if (deferred && state->codec && !state->closed)
			{
				if (auto error = state->codec->resume(streamId))
				{
					return error;
				}
				deferred = false;
			}
			state->wake();
			return {};
		}

		std::shared_ptr<State> state;
		Http2Budget::Lease lease;
		std::deque<std::string> chunks;
		std::optional<Http2Headers> trailerBlock;
		size_t frontOffset {0};
		int32_t streamId {0};
		bool finished {false};
		bool cancelled {false};
		bool deferred {false};
		bool eofRead {false};
		bool readStarted {false};
		std::shared_ptr<const std::function<void()>> cancelHandler;
	};

	Http2Output::Producer::ReadPermit::ReadPermit(std::shared_ptr<State> state,
												  Http2Budget::Lease lease,
												  int32_t streamId)
		: state_(std::move(state)), lease_(std::move(lease)), streamId_(streamId)
	{
	}

	Http2Output::Producer::ReadPermit::~ReadPermit()
	{
		release();
	}

	Http2Output::Producer::ReadPermit::ReadPermit(ReadPermit&& other) noexcept = default;

	Http2Output::Producer::ReadPermit& Http2Output::Producer::ReadPermit::operator=(ReadPermit&& other) noexcept
	{
		if (this != &other)
		{
			release();
			state_ = std::move(other.state_);
			lease_ = std::move(other.lease_);
			streamId_ = other.streamId_;
		}
		return *this;
	}

	void Http2Output::Producer::ReadPermit::release() noexcept
	{
		lease_.reset();
		if (state_)
		{
			state_->notifyReaders();
			state_.reset();
		}
	}

	size_t Http2Output::Producer::ReadPermit::size() const noexcept
	{
		return lease_.amount();
	}

	Http2Output::Producer::Producer(std::shared_ptr<State> state) : impl_(std::make_unique<Impl>(std::move(state)))
	{
	}

	Http2Output::Producer::~Producer()
	{
		impl_->state->queuedBytes -= impl_->lease.amount();
	}

	Awaitable<Http2Output::Producer::ReadResult> Http2Output::Producer::reserveRead(
		size_t maximum,
		std::chrono::steady_clock::time_point deadline)
	{
		return reserveOwned(shared_from_this(), maximum, deadline);
	}

	Awaitable<Http2Output::Producer::ReadResult> Http2Output::Producer::reserveOwned(
		std::shared_ptr<Producer> owner,
		size_t maximum,
		std::chrono::steady_clock::time_point deadline)
	{
		auto& producer = *owner->impl_;
		auto& state = *producer.state;
		if (maximum == 0 || maximum > state.options.maxFilePrefetchBytes)
		{
			co_return ReadResult {{}, {ErrorCode::hInvalidConfiguration, "HTTP/2 文件预取数量无效"}};
		}
		auto executor = co_await boost::asio::this_coro::executor;
		auto timer = std::make_shared<boost::asio::steady_timer>(executor);
		state.readers.push_back(timer);
		for (;;)
		{
			if (auto error = producer.canProduce())
			{
				co_return ReadResult {{}, std::move(error)};
			}
			if (std::chrono::steady_clock::now() >= deadline)
			{
				co_return ReadResult {{}, {ErrorCode::hTimedOut, "HTTP/2 文件等待超过响应绝对截止点"}};
			}
			const auto connectionWindow = state.codec->peerWindow();
			const auto streamWindow = state.codec->peerWindow(producer.streamId);
			const auto available = state.options.maxConnectionSendBytes - state.budget.usage().sendBytes;
			if (producer.chunks.empty() && connectionWindow > 0 && streamWindow > 0 && available > 0)
			{
				const auto count = std::min(
					{maximum, available, static_cast<size_t>(connectionWindow), static_cast<size_t>(streamWindow)});
				auto lease = state.budget.tryAcquireSend(count);
				if (lease)
				{
					co_return ReadResult {ReadPermit(producer.state, std::move(lease), producer.streamId), {}};
				}
			}
			// 查询、注册及挂起间不交出 loop；通知只取消此 timer，不主动唤醒 writer，避免空转。
			timer->expires_at(deadline);
			boost::system::error_code ignored;
			co_await timer->async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, ignored));
		}
	}

	NetworkError Http2Output::Producer::appendReserved(std::string&& bytes, ReadPermit permit)
	{
		if (permit.state_ != impl_->state || permit.streamId_ != impl_->streamId || bytes.size() > permit.size())
		{
			return {ErrorCode::hInvalidConfiguration, "HTTP/2 预取额度不属于当前生产者或字节数超额"};
		}
		// 两次计费操作间没有挂起或用户回调；释放后同一 loop 立即转为队列拥有。
		permit.release();
		return appendOwned(std::move(bytes));
	}

	Http2Budget::Lease Http2Output::Producer::acquireApplicationTask()
	{
		return impl_->canProduce() ? Http2Budget::Lease {} : impl_->state->budget.tryAcquireApplicationTask();
	}

	void Http2Output::Producer::setCancelHandler(std::function<void()> callback)
	{
		impl_->cancelHandler = callback ? std::make_shared<const std::function<void()>>(std::move(callback)) : nullptr;
		if (impl_->cancelled && impl_->cancelHandler)
		{
			auto handler = impl_->cancelHandler;
			(*handler)();
		}
	}

	NetworkError Http2Output::Producer::append(std::string_view bytes)
	{
		auto& producer = *impl_;
		if (auto error = producer.canProduce())
		{
			return error;
		}
		if (bytes.empty())
		{
			return {};
		}
		const auto previous = producer.lease.amount();
		if (producer.chunks.size() >= kMaxProducerChunks || bytes.size() > std::numeric_limits<size_t>::max() - previous
			|| !producer.resize(previous + bytes.size()))
		{
			return resourceError(producer.streamId);
		}
		try
		{
			producer.chunks.emplace_back(bytes);
		}
		catch (const std::bad_alloc&)
		{
			static_cast<void>(producer.resize(previous));
			return allocationError(producer.streamId);
		}
		auto error = producer.ready();
		if (error)
		{
			producer.chunks.pop_back();
			static_cast<void>(producer.resize(previous));
		}
		return error;
	}

	NetworkError Http2Output::Producer::appendOwned(std::string&& bytes)
	{
		auto& producer = *impl_;
		if (auto error = producer.canProduce())
		{
			return error;
		}
		if (bytes.empty())
		{
			return {};
		}
		const auto previous = producer.lease.amount();
		if (producer.chunks.size() >= kMaxProducerChunks || bytes.size() > std::numeric_limits<size_t>::max() - previous
			|| !producer.resize(previous + bytes.size()))
		{
			return resourceError(producer.streamId);
		}
		try
		{
			producer.chunks.emplace_back(std::move(bytes));
		}
		catch (const std::bad_alloc&)
		{
			static_cast<void>(producer.resize(previous));
			return allocationError(producer.streamId);
		}
		auto error = producer.ready();
		if (error)
		{
			bytes = std::move(producer.chunks.back());
			producer.chunks.pop_back();
			static_cast<void>(producer.resize(previous));
		}
		return error;
	}

	NetworkError Http2Output::Producer::appendChunks(std::span<const std::string> chunks)
	{
		auto& producer = *impl_;
		if (auto error = producer.canProduce())
		{
			return error;
		}
		size_t total = 0;
		for (const auto& chunk : chunks)
		{
			if (chunk.size() > std::numeric_limits<size_t>::max() - total)
			{
				return resourceError(producer.streamId);
			}
			total += chunk.size();
		}
		if (total == 0)
		{
			return {};
		}
		const auto previous = producer.lease.amount();
		if (producer.chunks.size() >= kMaxProducerChunks || total > std::numeric_limits<size_t>::max() - previous
			|| !producer.resize(previous + total))
		{
			return resourceError(producer.streamId);
		}
		try
		{
			std::string combined;
			combined.reserve(total);
			for (const auto& chunk : chunks)
			{
				combined.append(chunk);
			}
			producer.chunks.push_back(std::move(combined));
		}
		catch (const std::bad_alloc&)
		{
			static_cast<void>(producer.resize(previous));
			return allocationError(producer.streamId);
		}
		auto error = producer.ready();
		if (error)
		{
			producer.chunks.pop_back();
			static_cast<void>(producer.resize(previous));
		}
		return error;
	}

	NetworkError Http2Output::Producer::finish(std::span<const Http2Codec::Header> trailers)
	{
		auto& producer = *impl_;
		if (auto error = producer.canProduce())
		{
			return error;
		}
		size_t trailerBytes = 0;
		for (const auto& field : trailers)
		{
			const auto limit = producer.state->options.maxHeaderBytes;
			if (field.name.size() > limit || field.value.size() > limit - field.name.size()
				|| Http2Options::kHeaderFieldOverheadBytes > limit - field.name.size() - field.value.size())
			{
				return resourceError(producer.streamId);
			}
			const auto size = field.name.size() + field.value.size() + Http2Options::kHeaderFieldOverheadBytes;
			if (size > limit - trailerBytes)
			{
				return resourceError(producer.streamId);
			}
			trailerBytes += size;
		}
		const auto previous = producer.lease.amount();
		if (trailerBytes > std::numeric_limits<size_t>::max() - previous || !producer.resize(previous + trailerBytes))
		{
			return resourceError(producer.streamId);
		}
		try
		{
			if (!trailers.empty())
			{
				Http2Headers block(Http2Codec::HeaderKind::hTrailers, producer.streamId, producer.state->options);
				for (const auto& field : trailers)
				{
					if (auto error = block.append(field.name, field.value))
					{
						static_cast<void>(producer.resize(previous));
						return error;
					}
				}
				if (auto error = block.seal())
				{
					static_cast<void>(producer.resize(previous));
					return error;
				}
				producer.trailerBlock.emplace(std::move(block));
			}
		}
		catch (const std::bad_alloc&)
		{
			static_cast<void>(producer.resize(previous));
			return allocationError(producer.streamId);
		}
		producer.finished = true;
		auto error = producer.ready();
		if (error)
		{
			producer.finished = false;
			producer.trailerBlock.reset();
			static_cast<void>(producer.resize(previous));
		}
		return error;
	}

	void Http2Output::Producer::cancel()
	{
		stop(true);
	}

	void Http2Output::Producer::stop(bool cancelPending)
	{
		auto& producer = *impl_;
		if (cancelPending)
		{
			producer.state->pendingEnds.erase(producer.streamId);
		}
		if (producer.cancelled)
		{
			return;
		}
		producer.cancelled = true;
		if (producer.state->selectedStream == producer.streamId)
		{
			producer.state->selectedStream = 0;
			producer.state->readCredit = false;
		}
		std::deque<std::string>().swap(producer.chunks);
		producer.trailerBlock.reset();
		producer.frontOffset = 0;
		static_cast<void>(producer.resize(0));
		producer.state->notifyReaders();
		auto handler = producer.cancelHandler;
		if (handler)
		{
			(*handler)();
		}
		producer.state->wake();
	}

	size_t Http2Output::Producer::queuedBytes() const noexcept
	{
		return impl_->lease.amount();
	}

	Http2Codec::DataRead Http2Output::Producer::read(std::span<char> output)
	{
		auto& producer = *impl_;
		if (producer.state->closed || producer.cancelled)
		{
			return {0, false, false, cancelledError(producer.streamId)};
		}
		if (producer.chunks.empty())
		{
			if (!producer.finished)
			{
				producer.deferred = true;
				return {0, false, true, {}};
			}
		}
		// 每批只授权一个流读取一次，避免协议库默认的非 incremental 调度耗尽一整条流。
		auto& state = *producer.state;
		if (state.selectedStream != producer.streamId || !state.readCredit)
		{
			producer.deferred = true;
			return {0, false, true, {}};
		}
		state.readCredit = false;
		state.lastReadStream = producer.streamId;
		producer.readStarted = true;
		if (producer.chunks.empty())
		{
			producer.eofRead = true;
			return {0, true, false, {}};
		}
		const auto capacity = producer.state->wireCapacity;
		if (capacity <= kFrameHeaderBytes || output.empty())
		{
			return {0, false, false, resourceError(producer.streamId)};
		}
		const auto& chunk = producer.chunks.front();
		const auto batchLimit = producer.state->options.maxOutputBatchBytes;
		const auto quantum = batchLimit > kFrameHeaderBytes ? batchLimit - kFrameHeaderBytes : size_t {1};
		const auto count =
			std::min({output.size(), capacity - kFrameHeaderBytes, quantum, chunk.size() - producer.frontOffset});
		std::memcpy(output.data(), chunk.data() + producer.frontOffset, count);
		producer.frontOffset += count;
		if (producer.frontOffset == chunk.size())
		{
			const auto bytes = chunk.size();
			producer.chunks.pop_front();
			producer.frontOffset = 0;
			static_cast<void>(producer.resize(producer.lease.amount() - bytes));
			producer.state->notifyReaders();
		}
		const auto eof = producer.finished && producer.chunks.empty();
		producer.eofRead = eof;
		return {count, eof, false, {}};
	}

	std::span<const Http2Codec::Header> Http2Output::Producer::trailers() const noexcept
	{
		return impl_->trailerBlock ? impl_->trailerBlock->fields() : std::span<const Http2Codec::Header> {};
	}

	void Http2Output::Producer::onClose(Http2Code code)
	{
		// 协议库关闭流时，TLS 写回调可能还没完成；END_STREAM 跟踪要保留到实际写出。
		stop(code != Http2Code::hNoError);
		impl_->state->producers.erase(impl_->streamId);
	}

	struct Http2Output::Impl
	{
		Impl(Http2Codec& codec, Http2Budget budget, const Http2Options& options)
			: state(std::make_shared<State>(codec, std::move(budget), options))
			, wireLease(state->budget.tryAcquireSend(state->wireCapacity))
			, prefaceRemaining(codec.role() == Http2Codec::Role::hClient ? kClientPrefaceBytes : 0)
		{
			if (!wireLease)
			{
				throw std::invalid_argument("HTTP/2 连接预算不足以预留 wire 批次");
			}
		}

		bool ready(const Producer::Impl& producer, bool mayOpenStream) const
		{
			if (producer.cancelled || producer.eofRead || (producer.chunks.empty() && !producer.finished))
			{
				return false;
			}
			// 新请求的未知窗口为 -1，仍需给它一个轮次，使旧流先 DEFERRED、让新请求头前进。
			const auto window = state->codec->peerWindow(producer.streamId);
			const auto unknown = window == -1 && !producer.readStarted;
			if (unknown && !mayOpenStream)
			{
				return false;
			}
			return producer.chunks.empty() || (state->codec->peerWindow() > 0 && (window > 0 || unknown));
		}

		NetworkError prepareFinalHeaders()
		{
			for (const auto& [id, weak] : state->producers)
			{
				auto source = weak.lock();
				if (!source)
				{
					continue;
				}
				auto& producer = *source->impl_;
				if (producer.cancelled || !producer.finished || !producer.chunks.empty() || producer.eofRead)
				{
					continue;
				}
				if (state->codec->role() == Http2Codec::Role::hClient && state->codec->peerWindow(id) == -1
					&& !producer.readStarted)
				{
					continue;
				}
				// 零窗口时库不会调用 DATA reader；已排空的消息改用最终 HEADERS 结束，且不越过任何 payload。
				if (auto error = state->codec->submitTrailers(id, source->trailers()))
				{
					return error;
				}
				producer.eofRead = true;
				if (state->selectedStream == id)
				{
					state->readCredit = false;
					state->selectedStream = 0;
				}
			}
			return {};
		}

		NetworkError selectProducer()
		{
			const auto mayOpenStream =
				state->codec->role() != Http2Codec::Role::hClient
				|| state->codec->activeLocalStreamCount() < state->codec->peerMaxConcurrentStreams();
			if (state->readCredit)
			{
				auto found = state->producers.find(state->selectedStream);
				if (found != state->producers.end())
				{
					if (auto producer = found->second.lock(); producer && ready(*producer->impl_, mayOpenStream))
					{
						return {};
					}
				}
			}
			state->readCredit = false;
			state->selectedStream = 0;
			std::shared_ptr<Producer> next;
			std::shared_ptr<Producer> first;
			for (const auto& [id, weak] : state->producers)
			{
				auto producer = weak.lock();
				if (!producer || !ready(*producer->impl_, mayOpenStream))
				{
					continue;
				}
				if (!first || id < first->impl_->streamId)
				{
					first = producer;
				}
				if (id > state->lastReadStream && (!next || id < next->impl_->streamId))
				{
					next = std::move(producer);
				}
			}
			if (!next)
			{
				next = std::move(first);
			}
			if (!next)
			{
				return {};
			}
			auto& producer = *next->impl_;
			if (producer.deferred)
			{
				if (auto error = state->codec->resume(producer.streamId))
				{
					return error;
				}
				producer.deferred = false;
			}
			state->selectedStream = producer.streamId;
			state->readCredit = true;
			return {};
		}

		void frameComplete(std::vector<int32_t>& ended)
		{
			const auto type = static_cast<Http2Codec::FrameType>(frameHeader[3]);
			const auto flags = frameHeader[4];
			const auto id = static_cast<int32_t>((static_cast<uint32_t>(frameHeader[5] & 0x7f) << 24)
												 | (static_cast<uint32_t>(frameHeader[6]) << 16)
												 | (static_cast<uint32_t>(frameHeader[7]) << 8) | frameHeader[8]);
			if (type == Http2Codec::FrameType::hHeaders && (flags & Http2Codec::kEndStream))
			{
				if (flags & kEndHeaders)
				{
					if (state->pendingEnds.contains(id))
					{
						ended.push_back(id);
					}
				}
				else
				{
					continuedEndStream = id;
				}
			}
			else if (type == Http2Codec::FrameType::hContinuation && (flags & kEndHeaders) && continuedEndStream == id)
			{
				if (state->pendingEnds.contains(id))
				{
					ended.push_back(id);
				}
				continuedEndStream = 0;
			}
			else if (type == Http2Codec::FrameType::hData && (flags & Http2Codec::kEndStream))
			{
				if (state->pendingEnds.contains(id))
				{
					ended.push_back(id);
				}
			}
			headerUsed = 0;
		}

		void parseAcknowledged(std::string_view bytes, std::vector<int32_t>& ended)
		{
			while (!bytes.empty())
			{
				if (prefaceRemaining != 0)
				{
					const auto count = std::min(prefaceRemaining, bytes.size());
					prefaceRemaining -= count;
					bytes.remove_prefix(count);
					continue;
				}
				if (headerUsed < kFrameHeaderBytes)
				{
					const auto count = std::min(kFrameHeaderBytes - headerUsed, bytes.size());
					std::memcpy(frameHeader.data() + headerUsed, bytes.data(), count);
					headerUsed += count;
					bytes.remove_prefix(count);
					if (headerUsed != kFrameHeaderBytes)
					{
						continue;
					}
					payloadRemaining = (static_cast<size_t>(frameHeader[0]) << 16)
									   | (static_cast<size_t>(frameHeader[1]) << 8) | frameHeader[2];
					if (payloadRemaining == 0)
					{
						frameComplete(ended);
					}
					continue;
				}
				const auto count = std::min(payloadRemaining, bytes.size());
				payloadRemaining -= count;
				bytes.remove_prefix(count);
				if (payloadRemaining == 0)
				{
					frameComplete(ended);
				}
			}
		}

		std::shared_ptr<State> state;
		Http2Budget::Lease wireLease;
		std::string batch;
		size_t offset {0};
		size_t prefaceRemaining {0};
		std::array<uint8_t, kFrameHeaderBytes> frameHeader {};
		size_t headerUsed {0};
		size_t payloadRemaining {0};
		int32_t continuedEndStream {0};
	};

	Http2Output::Http2Output(Http2Codec& codec, Http2Budget budget, const Http2Options& options)
		: impl_(std::make_unique<Impl>(codec, std::move(budget), options))
	{
	}

	Http2Output::~Http2Output()
	{
		close();
	}

	void Http2Output::setWakeup(std::function<void()> callback)
	{
		if (!impl_->state->closed)
		{
			impl_->state->wakeup =
				callback ? std::make_shared<const std::function<void()>>(std::move(callback)) : nullptr;
		}
	}

	Http2Output::OpenResult Http2Output::submitRequest(std::span<const Http2Codec::Header> headers)
	{
		auto state = impl_->state;
		if (state->closed)
		{
			return {0, {}, cancelledError()};
		}
		std::shared_ptr<Producer> producer;
		int32_t streamId = 0;
		try
		{
			producer = std::shared_ptr<Producer>(new Producer(state));
			auto submitted = state->codec->submitRequest(headers, producer);
			if (submitted.error)
			{
				return {0, {}, std::move(submitted.error)};
			}
			streamId = submitted.streamId;
			producer->impl_->streamId = streamId;
			state->producers.emplace(streamId, producer);
			state->pendingEnds.emplace(streamId);
		}
		catch (const std::bad_alloc&)
		{
			if (streamId != 0)
			{
				static_cast<void>(state->codec->reset(streamId, Http2Code::hInternalError));
				producer->cancel();
			}
			return {0, {}, allocationError(streamId)};
		}
		state->wake();
		return {streamId, std::move(producer), {}};
	}

	Http2Output::OpenResult Http2Output::submitResponse(int32_t streamId, std::span<const Http2Codec::Header> headers)
	{
		auto state = impl_->state;
		if (state->closed)
		{
			return {0, {}, cancelledError(streamId)};
		}
		std::shared_ptr<Producer> producer;
		bool submitted = false;
		try
		{
			producer = std::shared_ptr<Producer>(new Producer(state));
			producer->impl_->streamId = streamId;
			if (auto error = state->codec->submitResponse(streamId, headers, producer))
			{
				return {0, {}, std::move(error)};
			}
			submitted = true;
			state->producers.emplace(streamId, producer);
			state->pendingEnds.emplace(streamId);
		}
		catch (const std::bad_alloc&)
		{
			if (submitted)
			{
				static_cast<void>(state->codec->reset(streamId, Http2Code::hInternalError));
				producer->cancel();
			}
			return {0, {}, allocationError(streamId)};
		}
		state->wake();
		return {streamId, std::move(producer), {}};
	}

	Http2Output::OpenResult Http2Output::submitResponseHeaders(int32_t streamId,
															   std::span<const Http2Codec::Header> headers)
	{
		auto state = impl_->state;
		if (state->closed)
		{
			return {0, {}, cancelledError(streamId)};
		}
		try
		{
			const auto [position, inserted] = state->pendingEnds.emplace(streamId);
			static_cast<void>(position);
			if (!inserted)
			{
				return {0, {}, {ErrorCode::hOperationInProgress, "HTTP/2 响应已经提交"}};
			}
			if (auto error = state->codec->submitResponse(streamId, headers, true))
			{
				state->pendingEnds.erase(streamId);
				return {0, {}, std::move(error)};
			}
		}
		catch (const std::bad_alloc&)
		{
			state->pendingEnds.erase(streamId);
			return {0, {}, allocationError(streamId)};
		}
		state->wake();
		return {streamId, {}, {}};
	}

	void Http2Output::cancelStream(int32_t streamId)
	{
		auto state = impl_->state;
		state->pendingEnds.erase(streamId);
		const auto found = state->producers.find(streamId);
		if (found != state->producers.end())
		{
			auto producer = found->second.lock();
			state->producers.erase(found);
			if (producer)
			{
				producer->cancel();
			}
		}
		state->wake();
	}

	Http2Output::BatchResult Http2Output::nextBatch()
	try
	{
		auto& output = *impl_;
		if (output.state->closed)
		{
			return {{}, cancelledError()};
		}
		if (output.offset < output.batch.size())
		{
			return {std::string_view(output.batch).substr(output.offset), {}};
		}
		output.state->notifyReaders();
		if (auto error = output.prepareFinalHeaders())
		{
			close();
			return {{}, std::move(error)};
		}
		if (auto error = output.selectProducer())
		{
			close();
			return {{}, std::move(error)};
		}
		output.state->pumping = true;
		auto result =
			output.state->codec->memSend(output.state->options.maxOutputBatchBytes, output.state->wireCapacity);
		output.state->pumping = false;
		if (result.error)
		{
			close();
			return {{}, std::move(result.error)};
		}
		output.batch = std::move(result.bytes);
		output.offset = 0;
		return {output.batch, {}};
	}
	catch (const std::bad_alloc&)
	{
		impl_->state->pumping = false;
		close();
		return {{}, allocationError()};
	}

	Http2Output::AckResult Http2Output::acknowledge(size_t bytesWritten)
	{
		auto& output = *impl_;
		if (bytesWritten > output.batch.size() - output.offset)
		{
			return {{}, {ErrorCode::hInvalidConfiguration, "HTTP/2 写确认超过当前批次"}};
		}
		AckResult result;
		try
		{
			if (!output.state->closed)
			{
				output.parseAcknowledged(std::string_view(output.batch).substr(output.offset, bytesWritten),
										 result.endedStreams);
			}
		}
		catch (const std::bad_alloc&)
		{
			close();
			result.endedStreams.clear();
			result.error = allocationError();
		}
		for (const auto id : result.endedStreams)
		{
			output.state->pendingEnds.erase(id);
		}
		output.offset += bytesWritten;
		output.state->notifyReaders();
		if (output.offset == output.batch.size())
		{
			std::string().swap(output.batch);
			output.offset = 0;
		}
		return result;
	}

	void Http2Output::onStreamClose(int32_t streamId, Http2Code code)
	{
		if (code != Http2Code::hNoError)
		{
			cancelStream(streamId);
			return;
		}
		const auto& state = impl_->state;
		auto found = state->producers.find(streamId);
		if (found == state->producers.end())
		{
			return;
		}
		auto producer = found->second.lock();
		state->producers.erase(found);
		if (producer)
		{
			producer->onClose(code);
		}
	}

	void Http2Output::close()
	{
		auto state = impl_->state;
		if (state->closed)
		{
			return;
		}
		state->closed = true;
		state->pendingEnds.clear();
		// cancel 会唤醒等待者；先摘下整个弱引用表，允许回调同步注销。
		auto producers = std::move(state->producers);
		for (const auto& [id, weak] : producers)
		{
			static_cast<void>(id);
			if (auto producer = weak.lock())
			{
				producer->cancel();
			}
		}
		state->wake();
		state->wakeup.reset();
		state->codec->abortOutput();
		state->codec = nullptr;
	}

	size_t Http2Output::queuedBytes() const noexcept
	{
		return impl_->state->queuedBytes;
	}

	size_t Http2Output::bufferedWireBytes() const noexcept
	{
		return impl_->batch.size() + (impl_->state->codec ? impl_->state->codec->bufferedOutputBytes() : 0);
	}

	bool Http2Output::hasPendingOutput() const noexcept
	{
		const auto& output = *impl_;
		const auto& state = *output.state;
		return !state.closed
			   && (state.pumping || !state.pendingEnds.empty() || output.offset < output.batch.size()
				   || (state.codec && state.codec->wantWrite()));
	}
} // namespace hical::detail

#endif
