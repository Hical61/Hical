/**
 * @file Http2Options.h
 * @brief HTTP/2 流、内存和时限的有限资源配置
 */

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

namespace hical
{

	/**
	 * @brief HTTP/2 服务端和客户端的资源预算；构造配置本身不启用 HTTP/2
	 * body/响应上限是单流口径，连接预算同时计算所有流；复位后仍在运行的 handler
	 * 继续占应用任务配额。服务端还需与已有 maxBodySize/maxHeaderSize 取较小值。
	 */
	struct Http2Options
	{
		static constexpr uint32_t kDefaultMaxConcurrentStreams = 64;
		static constexpr size_t kDefaultMaxServerApplicationTasks = 4096;
		static constexpr size_t kDefaultMaxHeaderBytes = 8 * 1024;
		static constexpr size_t kMaxHeaderFields = 64;
		static constexpr size_t kHeaderFieldOverheadBytes = 32;
		static constexpr size_t kDefaultMaxBodyBytes = 1024 * 1024;
		static constexpr size_t kDefaultConnectionBufferBytes = 8 * 1024 * 1024;
		static constexpr size_t kDefaultOutputBatchBytes = 64 * 1024;
		static constexpr size_t kDefaultMaxResponseBytes = 16 * 1024 * 1024;
		static constexpr size_t kDefaultClientConnectionReceiveBytes = 32 * 1024 * 1024;
		static constexpr size_t kDefaultMaxConnectionsPerOrigin = 2;
		static constexpr size_t kDefaultMaxQueuedRequestsPerOrigin = 256;
		static constexpr uint32_t kDefaultWindowSize = 65535;
		static constexpr uint32_t kMaxWindowSize = 0x7fffffff;
		static constexpr uint32_t kMinFrameBytes = 16384;
		static constexpr uint32_t kMaxFrameBytes = 0xffffff;
		static constexpr uint32_t kDefaultHeaderTableBytes = 4096;
		static constexpr auto kDefaultNegotiationTimeout = std::chrono::seconds {10};
		static constexpr auto kDefaultProgressTimeout = std::chrono::seconds {30};

		/** 入站协议流或客户端出站流的本地上限 */
		uint32_t maxConcurrentStreams {kDefaultMaxConcurrentStreams};
		/** 每连接仍运行的应用任务上限，独立于活跃协议流计数 */
		uint32_t maxApplicationTasks {kDefaultMaxConcurrentStreams};
		/** 服务级应用任务总上限，不能只依靠每连接上限 */
		size_t maxServerApplicationTasks {kDefaultMaxServerApplicationTasks};
		/** 初始头与尾头分别计费；每个原始字段按 name + value + 32 字节计算，含伪头 */
		size_t maxHeaderBytes {kDefaultMaxHeaderBytes};
		/** 每个头部块的字段上限，不允许超过 RequestHeaders 容量 */
		size_t maxHeaderFields {kMaxHeaderFields};
		/** 服务端每流完整请求体上限 */
		size_t maxBodyBytes {kDefaultMaxBodyBytes};
		/** 每连接已接收的应用数据总预算 */
		size_t maxConnectionReceiveBytes {kDefaultConnectionBufferBytes};
		/** 每连接待发应用数据预算，含响应和文件预取 */
		size_t maxConnectionSendBytes {kDefaultConnectionBufferBytes};
		/** 每批输出及每流文件预取均不得超过连接待发预算 */
		size_t maxOutputBatchBytes {kDefaultOutputBatchBytes};
		size_t maxFilePrefetchBytes {kDefaultOutputBatchBytes};
		/** 客户端单流及连接级响应聚合上限 */
		size_t maxResponseBytes {kDefaultMaxResponseBytes};
		size_t maxClientConnectionReceiveBytes {kDefaultClientConnectionReceiveBytes};
		/** 客户端每个 origin 的连接与待调度请求上限 */
		size_t maxConnectionsPerOrigin {kDefaultMaxConnectionsPerOrigin};
		size_t maxQueuedRequestsPerOrigin {kDefaultMaxQueuedRequestsPerOrigin};
		/** 初始接收窗口；流窗口允许为零，连接窗口不得小于协议初值 */
		uint32_t initialStreamWindow {kDefaultWindowSize};
		uint32_t initialConnectionWindow {kDefaultWindowSize};
		/** 本端接收的帧和 HPACK 动态表上限；动态表允许为零 */
		uint32_t maxFrameBytes {kMinFrameBytes};
		uint32_t headerTableBytes {kDefaultHeaderTableBytes};
		/** 独立阶段的最长等待时间，必须为正；PING 不延长 bodyTimeout */
		std::chrono::steady_clock::duration negotiationTimeout {kDefaultNegotiationTimeout};
		std::chrono::steady_clock::duration headerTimeout {kDefaultNegotiationTimeout};
		std::chrono::steady_clock::duration bodyTimeout {kDefaultProgressTimeout};
		std::chrono::steady_clock::duration writeTimeout {kDefaultProgressTimeout};
		std::chrono::steady_clock::duration queueTimeout {kDefaultProgressTimeout};

		/**
		 * @brief 检查配置边界，不分配内存，也不改变配置
		 * @return 首个配置错误；空字符串表示有效
		 */
		[[nodiscard]] std::string_view validationError() const noexcept
		{
			if (maxConcurrentStreams == 0 || maxConcurrentStreams > kMaxWindowSize || maxApplicationTasks == 0
				|| maxServerApplicationTasks == 0)
			{
				return "HTTP/2 流和应用任务上限必须为正，流上限不得超过 2^31-1";
			}
			if (maxHeaderFields == 0 || maxHeaderFields > kMaxHeaderFields || maxHeaderBytes == 0)
			{
				return "HTTP/2 头部预算必须为正，字段数不得超过 64";
			}
			if (maxBodyBytes == 0 || maxConnectionReceiveBytes < maxBodyBytes || maxConnectionSendBytes == 0)
			{
				return "HTTP/2 body 和连接预算必须为正，单流 body 不得超过连接接收预算";
			}
			if (maxOutputBatchBytes == 0 || maxOutputBatchBytes > maxConnectionSendBytes || maxFilePrefetchBytes == 0
				|| maxFilePrefetchBytes > maxConnectionSendBytes)
			{
				return "HTTP/2 输出批和文件预取必须为正，且不得超过连接待发预算";
			}
			if (maxResponseBytes == 0 || maxClientConnectionReceiveBytes < maxResponseBytes
				|| maxConnectionsPerOrigin == 0 || maxQueuedRequestsPerOrigin == 0)
			{
				return "HTTP/2 客户端预算必须为正，单流响应不得超过连接接收预算";
			}
			if (initialStreamWindow > kMaxWindowSize || initialConnectionWindow < kDefaultWindowSize
				|| initialConnectionWindow > kMaxWindowSize || maxFrameBytes < kMinFrameBytes
				|| maxFrameBytes > kMaxFrameBytes)
			{
				return "HTTP/2 窗口或帧大小超出协议范围";
			}
			if (negotiationTimeout <= std::chrono::steady_clock::duration::zero()
				|| headerTimeout <= std::chrono::steady_clock::duration::zero()
				|| bodyTimeout <= std::chrono::steady_clock::duration::zero()
				|| writeTimeout <= std::chrono::steady_clock::duration::zero()
				|| queueTimeout <= std::chrono::steady_clock::duration::zero())
			{
				return "HTTP/2 阶段时限必须为正";
			}
			return {};
		}

		/**
		 * @brief 检查配置，非法配置抛出 std::invalid_argument
		 */
		void validate() const
		{
			if (auto error = validationError(); !error.empty())
			{
				throw std::invalid_argument(std::string(error));
			}
		}
	};

} // namespace hical
