/**
 * @file HttpProtocol.h
 * @brief HTTP 协议身份、协商策略和请求处理证据
 */

#pragma once

#include <cstdint>

namespace hical
{

	/**
	 * @brief 已选定的 HTTP 传输协议，与 HTTP/1 minor version 分开记录
	 */
	enum class HttpProtocol : uint8_t
	{
		hHttp1,
		hHttp2
	};

	/**
	 * @brief 客户端 TLS 协议协商策略；HTTP/2 模块关闭时仅支持 hHttp1Only
	 */
	enum class HttpProtocolMode : uint8_t
	{
		hHttp1Only,
		hNegotiateHttp2,
		hHttp2Only
	};

	/**
	 * @brief 传输层能证明的请求处理状态，不代表已满足重试条件
	 * 即使明确未处理，自动重试仍需检查方法幂等性、body 可重放、次数和原截止时间。
	 */
	enum class HttpRequestDisposition : uint8_t
	{
		hNotSent,          // 尚未开始发送请求
		hNotProcessed,     // REFUSED_STREAM 或 GOAWAY last-stream-id 能证明未处理
		hPossiblyProcessed // 已发送或没有足够证据，不能推断业务是否执行
	};

} // namespace hical
