/**
 * @file LogFormatter.cpp
 * @brief 日志格式化器实现
 */

#include "LogFormatter.h"

#include "Log.h"

#include <boost/json/object.hpp>
#include <boost/json/serialize.hpp>
#include <charconv>
#include <cstdio>
#include <ctime>

namespace hical
{

	// ============ 共用：thread_local 时间戳缓存 ============

	namespace
	{
		const char* extractFilename(const char* file)
		{
			if (file == nullptr)
			{
				return "";
			}
			const char* fileName = file;
			for (const char* p = file; *p != '\0'; ++p)
			{
				if (*p == '/' || *p == '\\')
				{
					fileName = p + 1;
				}
			}
			return fileName;
		}

		/// 转义控制字符，防止日志注入
		std::string sanitizeForText(std::string_view sv)
		{
			std::string out;
			out.reserve(sv.size());
			for (unsigned char c : sv)
			{
				if (c == '\n')
				{
					out += "\\n";
				}
				else if (c == '\r')
				{
					out += "\\r";
				}
				else if (c == '\033') // ESC — ANSI 转义序列入口
				{
					out += "\\e";
				}
				else if (c < 0x20 && c != '\t') // 其余控制字符（保留 tab）
				{
					out += '?';
				}
				else
				{
					out += static_cast<char>(c);
				}
			}
			return out;
		}

		struct TsCache
		{
			time_t cachedSec {0};
			struct tm cachedTm {};
		};

		// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)
		thread_local TsCache tsFmtCache;

		// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

		struct tm cachedLocaltime(time_t nowSec)
		{
			auto& cache = tsFmtCache;
			if (nowSec != cache.cachedSec)
			{
				cache.cachedSec = nowSec;
#if defined(_WIN32)
				localtime_s(&cache.cachedTm, &nowSec);
#else
				localtime_r(&nowSec, &cache.cachedTm);
#endif
			}
			return cache.cachedTm;
		}
	} // namespace

	// ============ TextFormatter ============

	std::string TextFormatter::format(const LogRecord& record)
	{
		auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(record.timestamp.time_since_epoch()).count();
		auto nowSec = static_cast<time_t>(nowMs / 1000);
		int ms = static_cast<int>(nowMs % 1000);

		// 使用 thread_local 缓存的 localtime
		struct tm tmInfo = cachedLocaltime(nowSec);

		char timeBuf[64];
		snprintf(timeBuf,
				 sizeof(timeBuf),
				 "%04d-%02d-%02d %02d:%02d:%02d.%03d",
				 tmInfo.tm_year + 1900,
				 tmInfo.tm_mon + 1,
				 tmInfo.tm_mday,
				 tmInfo.tm_hour,
				 tmInfo.tm_min,
				 tmInfo.tm_sec,
				 ms);

		const char* fileName = extractFilename(record.file);

		// 格式化
		std::string result;
		result.reserve(256);
		result += '[';
		result += timeBuf;
		result += "] [";
		result += logLevelToString(record.level);
		result += "] [";
		{
			char buf[24]; // NOLINT(cppcoreguidelines-avoid-magic-numbers)
			auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), record.threadId);
			result.append(buf, static_cast<size_t>(ptr - buf));
		}
		result += "] ";

		// traceId（可选）
		if (!record.traceId.empty())
		{
			result += '[';
			result += sanitizeForText(record.traceId);
			result += "] ";
		}

		result += '[';
		result += fileName;
		result += ':';
		{
			char buf[12]; // NOLINT(cppcoreguidelines-avoid-magic-numbers)
			auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), record.line);
			result.append(buf, static_cast<size_t>(ptr - buf));
		}
		result += "] ";
		result += sanitizeForText(record.message);
		result += '\n';

		return result;
	}

	// ============ JsonFormatter ============

	std::string JsonFormatter::format(const LogRecord& record)
	{
		auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(record.timestamp.time_since_epoch()).count();
		auto nowSec = static_cast<time_t>(nowMs / 1000);
		int ms = static_cast<int>(nowMs % 1000);

		// JSON 时间戳使用 UTC（gmtime），与 "Z" 后缀语义一致
		struct tm tmInfo {};
#if defined(_WIN32)
		gmtime_s(&tmInfo, &nowSec);
#else
		gmtime_r(&nowSec, &tmInfo);
#endif
		char timeBuf[64];
		snprintf(timeBuf,
				 sizeof(timeBuf),
				 "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
				 tmInfo.tm_year + 1900,
				 tmInfo.tm_mon + 1,
				 tmInfo.tm_mday,
				 tmInfo.tm_hour,
				 tmInfo.tm_min,
				 tmInfo.tm_sec,
				 ms);

		boost::json::object obj;
		obj["timestamp"] = timeBuf;
		obj["level"] = logLevelToString(record.level);
		obj["thread_id"] = record.threadId;

		if (record.file != nullptr)
		{
			obj["file"] = extractFilename(record.file);
			obj["line"] = record.line;
		}

		obj["message"] = record.message;

		if (!record.traceId.empty())
		{
			obj["trace_id"] = record.traceId;
		}

		// 合并结构化字段
		for (const auto& [key, val] : record.fields)
		{
			obj[key] = val;
		}

		auto s = boost::json::serialize(obj);
		s.push_back('\n');
		return s;
	}

} // namespace hical
