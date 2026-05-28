/**
 * @file LogRecord.h
 * @brief 结构化日志条目
 */

#pragma once

#include <boost/json/object.hpp>
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

namespace hical
{

	// 前向声明 LogLevel（定义在 Log.h 中）
	enum class LogLevel : uint8_t;

	/**
	 * @brief 结构化日志条目
	 * 值类型，在栈上构建后传递给 Formatter。
	 * fields 和 traceId 为可选字段，仅在结构化日志或中间件场景下使用。
	 */
	struct LogRecord
	{
		LogLevel level {};
		std::chrono::system_clock::time_point timestamp;
		uint64_t threadId {0};
		const char* file {nullptr};
		int line {0};
		std::string message;

		// 结构化字段（可选）
		boost::json::object fields;

		// 请求追踪 ID（可选，由 LogMiddleware 注入）
		std::string traceId;
	};

} // namespace hical
