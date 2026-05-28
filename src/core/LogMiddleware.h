/**
 * @file LogMiddleware.h
 * @brief 洋葱模型访问日志中间件
 */

#pragma once

#include "Middleware.h"

#include <string>

namespace hical
{

	class HttpRequest;

	/**
	 * @brief 日志中间件配置
	 */
	struct LogMiddlewareOptions
	{
		std::string accessLogChannel = "access";
		bool autoTraceId = true;
		std::string traceIdHeader = "X-Trace-Id";
	};

	/// 请求属性键名
	inline constexpr const char* hTraceIdKey = "hical.trace_id";

	/**
	 * @brief 创建日志中间件
	 * 洋葱模型：
	 * - before next()：生成 trace-id（或从请求头提取），注入到 req 属性
	 * - after next()：计算耗时，发射结构化访问日志到 access 通道
	 * @param opts 配置选项
	 * @return MiddlewareHandler
	 */
	MiddlewareHandler makeLogMiddleware(LogMiddlewareOptions opts = {});

	/**
	 * @brief 从请求中获取 trace-id
	 * @param req HTTP 请求
	 * @return trace-id 字符串，不存在则返回空
	 */
	std::string getTraceId(const HttpRequest& req);

	/**
	 * @brief 生成 32 字符十六进制随机 ID（128 位）
	 * 使用 thread_local std::mt19937_64（仅需唯一性，非密码学安全）。
	 * @return 32 字符十六进制字符串
	 */
	std::string generateTraceId();

} // namespace hical
