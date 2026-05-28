/**
 * @file LogFormatter.h
 * @brief 日志格式化器接口与实现（Text/JSON）
 */

#pragma once

#include "LogRecord.h"

#include <memory>
#include <string>

namespace hical
{

	/**
	 * @brief 日志格式化器抽象接口
	 * 将 LogRecord 转换为可写入 Sink 的字符串行。
	 */
	class LogFormatter
	{
	public:
		virtual ~LogFormatter() = default;

		/**
		 * @brief 将 LogRecord 格式化为字符串（含换行符）
		 * @param record 结构化日志条目
		 * @return 已格式化的完整日志行
		 */
		virtual std::string format(const LogRecord& record) = 0;
	};

	/**
	 * @brief 文本格式化器
	 * 输出格式与 Phase 1/2 一致：
	 * [2026-05-01 14:25:05.123] [INFO] [12345] [file.cpp:42] message
	 * 当 traceId 非空时：
	 * [2026-05-01 14:25:05.123] [INFO] [12345] [abc123] [file.cpp:42] message
	 */
	class TextFormatter : public LogFormatter
	{
	public:
		std::string format(const LogRecord& record) override;
	};

	/**
	 * @brief JSON 格式化器
	 * 输出 JSON Lines 格式（每行一个 JSON 对象），便于 ELK/Loki/Splunk 采集。
	 * 自动合并 record.fields 到输出对象中。
	 */
	class JsonFormatter : public LogFormatter
	{
	public:
		std::string format(const LogRecord& record) override;
	};

} // namespace hical
