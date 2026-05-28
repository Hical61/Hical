/**
 * @file Log.h
 * @brief 生产级日志系统（Logger 单例 + 宏族）
 */

#pragma once

#include "FixedBuffer.h"

#include <boost/json/object.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <memory>
#include <mutex>
#include <ostream>
#include <string_view>
#include <vector>

namespace hical
{

	class LogSink;
	class LogFormatter;
	class LogChannelRegistry;
	struct LogRecord;

	/**
	 * @brief 日志级别枚举（6 级）
	 * hTrace 最细粒度，在 NDEBUG 下编译期完全消除
	 * hDebug 调试信息
	 * hInfo 正常运行信息（生产环境默认级别）
	 * hWarn 警告，需要关注
	 * hError 错误，但服务仍可继续运行
	 * hFatal 致命错误，自动 flush + abort()
	 */
	enum class LogLevel : uint8_t
	{
		hTrace = 0,
		hDebug = 1,
		hInfo = 2,
		hWarn = 3,
		hError = 4,
		hFatal = 5
	};

	/**
	 * @brief 日志级别转字符串
	 * @param level 日志级别
	 * @return 级别名称字符串（固定 5 字符宽度，便于对齐）
	 */
	inline const char* logLevelToString(LogLevel level)
	{
		switch (level)
		{
			case LogLevel::hTrace:
				return "TRACE";
			case LogLevel::hDebug:
				return "DEBUG";
			case LogLevel::hInfo:
				return "INFO";
			case LogLevel::hWarn:
				return "WARN";
			case LogLevel::hError:
				return "ERROR";
			case LogLevel::hFatal:
				return "FATAL";
		}
		return "UNKNOWN";
	}

	/**
	 * @brief 日志系统（线程安全）
	 * 单例模式，支持日志级别过滤和多种输出目标。
	 * 级别判断使用 atomic load（零开销跳过），mutex 仅在实际写入时持有。
	 * 支持 std::format 风格 API 和流式 API。
	 */
	class Logger
	{
	public:
		/**
		 * @brief 获取全局 Logger 实例（Meyer's singleton）
		 * @return Logger 引用
		 */
		static Logger& instance();

		/**
		 * @brief 设置日志级别
		 * @param level 最低输出级别（低于此级别的日志将被过滤）
		 */
		void setLevel(LogLevel level);

		/**
		 * @brief 获取当前日志级别
		 * @return 当前级别
		 */
		LogLevel level() const;

		/**
		 * @brief 设置 flush 阈值级别（>= 此级别的日志自动 flush）
		 * @param level flush 阈值（默认 hError）
		 */
		void setFlushLevel(LogLevel level);

		/**
		 * @brief 获取当前 flush 阈值级别
		 * @return flush 阈值
		 */
		LogLevel flushLevel() const;

		/**
		 * @brief 设置输出到指定流（如 std::cerr、stringstream 等）
		 * @param os 输出流引用（调用方需保证生命周期）
		 */
		void setOutput(std::ostream& os);

		/**
		 * @brief 设置输出到文件
		 * @param filePath 日志文件路径
		 */
		void setOutput(const std::string& filePath);

		/**
		 * @brief 统一输出入口（logFmt/log/LogStream 析构均调用此方法）
		 * @param lvl 日志级别
		 * @param file 源文件名
		 * @param line 行号
		 * @param msg 已格式化的日志消息
		 */
		void output(LogLevel lvl, const char* file, int line, std::string_view msg);

		/**
		 * @brief std::format 风格日志输出
		 * @param lvl 日志级别
		 * @param file 源文件名
		 * @param line 行号
		 * @param fmt 格式字符串（编译期校验）
		 * @param args 格式化参数
		 */
		template <typename... Args>
		void logFmt(LogLevel lvl, const char* file, int line, std::format_string<Args...> fmt, Args&&... args)
		{
			output(lvl, file, line, std::format(fmt, std::forward<Args>(args)...));
		}

		/**
		 * @brief 输出一条日志（内部转调 output()）
		 * @param lvl 日志级别
		 * @param file 源文件名
		 * @param line 行号
		 * @param msg 日志消息
		 */
		void log(LogLevel lvl, const char* file, int line, std::string_view msg);

		// ============ Sink 管理 API ============

		/**
		 * @brief 添加一个日志输出后端
		 * @param sink Sink 的 shared_ptr（Logger 与调用方共享所有权）
		 */
		void addSink(std::shared_ptr<LogSink> sink);

		/**
		 * @brief 设置唯一的 Sink（清除已有 Sink 后添加）
		 * @param sink Sink 的 shared_ptr
		 */
		void setSink(std::shared_ptr<LogSink> sink);

		/**
		 * @brief 清除所有已注册的 Sink
		 */
		void clearSinks();

		// ============ Formatter 管理 API ============

		/**
		 * @brief 设置默认通道的日志格式化器
		 * @param formatter Formatter 的 shared_ptr（默认 TextFormatter）
		 */
		void setFormatter(std::shared_ptr<LogFormatter> formatter);

		// ============ Channel 管理 API ============

		/**
		 * @brief 获取通道注册表
		 * @return 通道注册表引用
		 */
		LogChannelRegistry& channels();

		/**
		 * @brief 发射结构化日志到指定通道
		 * @param channelName 通道名称
		 * @param record 日志条目
		 */
		void emitTo(const std::string& channelName, const LogRecord& record);

		/**
		 * @brief 发射结构化日志到默认通道
		 * @param record 日志条目
		 */
		void emit(const LogRecord& record);

		/**
		 * @brief 带结构化字段的 std::format 风格日志输出
		 * @param lvl 日志级别
		 * @param file 源文件名
		 * @param line 行号
		 * @param fields 结构化字段
		 * @param fmt 格式字符串
		 * @param args 格式化参数
		 */
		template <typename... Args>
		void logFmtWithFields(LogLevel lvl,
							  const char* file,
							  int line,
							  boost::json::object fields,
							  std::format_string<Args...> fmt,
							  Args&&... args)
		{
			outputWithFields(lvl, file, line, std::move(fields), std::format(fmt, std::forward<Args>(args)...));
		}

		/**
		 * @brief 带结构化字段的统一输出入口
		 */
		void outputWithFields(LogLevel lvl,
							  const char* file,
							  int line,
							  boost::json::object fields,
							  std::string_view msg);

	private:
		Logger();
		Logger(const Logger&) = delete;
		Logger& operator=(const Logger&) = delete;

		std::atomic<LogLevel> level_ {LogLevel::hInfo};
		std::atomic<LogLevel> flushLevel_ {LogLevel::hError};
		std::mutex mutex_;
		std::shared_ptr<const std::vector<std::shared_ptr<LogSink>>> sinks_;
		std::shared_ptr<LogFormatter> formatter_;
		std::unique_ptr<LogChannelRegistry> channels_;
	};

	// ============ 流式日志辅助类 ============

	/**
	 * @brief 流式日志收集器（RAII）
	 * 析构时调用 Logger::output() 输出收集到的日志消息。
	 */
	class LogStream
	{
	public:
		LogStream(LogLevel lvl, const char* file, int line) : lvl_(lvl), file_(file), line_(line)
		{
		}

		~LogStream()
		{
			if (buf_.size() > 0)
			{
				Logger::instance().output(lvl_, file_, line_, buf_.view());
			}
		}

		LogStream(const LogStream&) = delete;
		LogStream& operator=(const LogStream&) = delete;
		LogStream(LogStream&&) noexcept = default;
		LogStream& operator=(LogStream&&) noexcept = default;

		template <typename T>
		LogStream& operator<<(const T& val)
		{
			buf_ << val;
			return *this;
		}

	private:
		LogLevel lvl_;
		const char* file_;
		int line_;
		FixedBuffer<4096> buf_;
	};

	/**
	 * @brief 辅助类，用于在流式宏的 if-else 分支中消耗 LogStream 对象
	 */
	struct LogMessageVoid
	{
		void operator&(const LogStream& /*unused*/) const
		{
		}
	};

} // namespace hical

// ============ 日志宏 ============
// 级别短路：先 atomic load 判断级别，不满足时零开销跳过

// NOLINTBEGIN(cppcoreguidelines-macro-usage)

// 内部实现宏：级别判断 + logFmt 调用
#define HICAL_LOG_IMPL_(lvl, fmt, ...)                                                                     \
	do                                                                                                     \
	{                                                                                                      \
		if (::hical::Logger::instance().level() <= (lvl))                                                  \
		{                                                                                                  \
			::hical::Logger::instance().logFmt((lvl), __FILE__, __LINE__, fmt __VA_OPT__(, ) __VA_ARGS__); \
		}                                                                                                  \
	}                                                                                                      \
	while (0)

// ---- TRACE：NDEBUG 下编译期完全消除 ----
#ifdef NDEBUG
	#define HICAL_LOG_TRACE(fmt, ...) ((void)0)
	#define HICAL_LOG_TRACE_IF(cond, fmt, ...) ((void)0)
	#define HICAL_LOG_TRACE_F(fields, fmt, ...) ((void)0)
	// NOLINTNEXTLINE(bugprone-macro-parentheses)
	#define HICAL_LOG_TRACE_STREAM \
		if (true)                  \
		{                          \
		}                          \
		else                       \
			::hical::LogMessageVoid() & ::hical::LogStream(::hical::LogLevel::hTrace, __FILE__, __LINE__)
#else
	#define HICAL_LOG_TRACE(fmt, ...) HICAL_LOG_IMPL_(::hical::LogLevel::hTrace, fmt __VA_OPT__(, ) __VA_ARGS__)
	#define HICAL_LOG_TRACE_IF(cond, fmt, ...)                   \
		do                                                       \
		{                                                        \
			if (cond)                                            \
			{                                                    \
				HICAL_LOG_TRACE(fmt __VA_OPT__(, ) __VA_ARGS__); \
			}                                                    \
		}                                                        \
		while (0)
	// NOLINTNEXTLINE(bugprone-macro-parentheses)
	#define HICAL_LOG_TRACE_STREAM                                           \
		if (::hical::Logger::instance().level() > ::hical::LogLevel::hTrace) \
		{                                                                    \
		}                                                                    \
		else                                                                 \
			::hical::LogMessageVoid() & ::hical::LogStream(::hical::LogLevel::hTrace, __FILE__, __LINE__)
#endif

// ---- DEBUG ----
#define HICAL_LOG_DEBUG(fmt, ...) HICAL_LOG_IMPL_(::hical::LogLevel::hDebug, fmt __VA_OPT__(, ) __VA_ARGS__)
#define HICAL_LOG_DEBUG_IF(cond, fmt, ...)                   \
	do                                                       \
	{                                                        \
		if (cond)                                            \
		{                                                    \
			HICAL_LOG_DEBUG(fmt __VA_OPT__(, ) __VA_ARGS__); \
		}                                                    \
	}                                                        \
	while (0)
// NOLINTNEXTLINE(bugprone-macro-parentheses)
#define HICAL_LOG_DEBUG_STREAM                                           \
	if (::hical::Logger::instance().level() > ::hical::LogLevel::hDebug) \
	{                                                                    \
	}                                                                    \
	else                                                                 \
		::hical::LogMessageVoid() & ::hical::LogStream(::hical::LogLevel::hDebug, __FILE__, __LINE__)

// ---- INFO ----
#define HICAL_LOG_INFO(fmt, ...) HICAL_LOG_IMPL_(::hical::LogLevel::hInfo, fmt __VA_OPT__(, ) __VA_ARGS__)
#define HICAL_LOG_INFO_IF(cond, fmt, ...)                   \
	do                                                      \
	{                                                       \
		if (cond)                                           \
		{                                                   \
			HICAL_LOG_INFO(fmt __VA_OPT__(, ) __VA_ARGS__); \
		}                                                   \
	}                                                       \
	while (0)
// NOLINTNEXTLINE(bugprone-macro-parentheses)
#define HICAL_LOG_INFO_STREAM                                           \
	if (::hical::Logger::instance().level() > ::hical::LogLevel::hInfo) \
	{                                                                   \
	}                                                                   \
	else                                                                \
		::hical::LogMessageVoid() & ::hical::LogStream(::hical::LogLevel::hInfo, __FILE__, __LINE__)

// ---- WARN ----
#define HICAL_LOG_WARN(fmt, ...) HICAL_LOG_IMPL_(::hical::LogLevel::hWarn, fmt __VA_OPT__(, ) __VA_ARGS__)
#define HICAL_LOG_WARN_IF(cond, fmt, ...)                   \
	do                                                      \
	{                                                       \
		if (cond)                                           \
		{                                                   \
			HICAL_LOG_WARN(fmt __VA_OPT__(, ) __VA_ARGS__); \
		}                                                   \
	}                                                       \
	while (0)
// NOLINTNEXTLINE(bugprone-macro-parentheses)
#define HICAL_LOG_WARN_STREAM                                           \
	if (::hical::Logger::instance().level() > ::hical::LogLevel::hWarn) \
	{                                                                   \
	}                                                                   \
	else                                                                \
		::hical::LogMessageVoid() & ::hical::LogStream(::hical::LogLevel::hWarn, __FILE__, __LINE__)

// ---- ERROR ----
#define HICAL_LOG_ERROR(fmt, ...) HICAL_LOG_IMPL_(::hical::LogLevel::hError, fmt __VA_OPT__(, ) __VA_ARGS__)
#define HICAL_LOG_ERROR_IF(cond, fmt, ...)                   \
	do                                                       \
	{                                                        \
		if (cond)                                            \
		{                                                    \
			HICAL_LOG_ERROR(fmt __VA_OPT__(, ) __VA_ARGS__); \
		}                                                    \
	}                                                        \
	while (0)
// NOLINTNEXTLINE(bugprone-macro-parentheses)
#define HICAL_LOG_ERROR_STREAM                                           \
	if (::hical::Logger::instance().level() > ::hical::LogLevel::hError) \
	{                                                                    \
	}                                                                    \
	else                                                                 \
		::hical::LogMessageVoid() & ::hical::LogStream(::hical::LogLevel::hError, __FILE__, __LINE__)

// ---- FATAL ----
#define HICAL_LOG_FATAL(fmt, ...) HICAL_LOG_IMPL_(::hical::LogLevel::hFatal, fmt __VA_OPT__(, ) __VA_ARGS__)
#define HICAL_LOG_FATAL_IF(cond, fmt, ...)                   \
	do                                                       \
	{                                                        \
		if (cond)                                            \
		{                                                    \
			HICAL_LOG_FATAL(fmt __VA_OPT__(, ) __VA_ARGS__); \
		}                                                    \
	}                                                        \
	while (0)
// NOLINTNEXTLINE(bugprone-macro-parentheses)
#define HICAL_LOG_FATAL_STREAM                                           \
	if (::hical::Logger::instance().level() > ::hical::LogLevel::hFatal) \
	{                                                                    \
	}                                                                    \
	else                                                                 \
		::hical::LogMessageVoid() & ::hical::LogStream(::hical::LogLevel::hFatal, __FILE__, __LINE__)

// ---- 结构化字段日志宏（带 boost::json::object fields）----
// 需要完整的 LogRecord 定义，用户使用前需 include LogRecord.h
// 或者直接 include LogChannel.h（其中包含 LogRecord.h）
#include "LogChannel.h"
#include "LogRecord.h"

#define HICAL_LOG_IMPL_F_(lvl, fields, fmt, ...)                                          \
	do                                                                                    \
	{                                                                                     \
		if (::hical::Logger::instance().level() <= (lvl))                                 \
		{                                                                                 \
			::hical::Logger::instance().logFmtWithFields((lvl),                           \
														 __FILE__,                        \
														 __LINE__,                        \
														 (fields),                        \
														 fmt __VA_OPT__(, ) __VA_ARGS__); \
		}                                                                                 \
	}                                                                                     \
	while (0)

#ifndef NDEBUG
	#define HICAL_LOG_TRACE_F(fields, fmt, ...) \
		HICAL_LOG_IMPL_F_(::hical::LogLevel::hTrace, fields, fmt __VA_OPT__(, ) __VA_ARGS__)
#endif
#define HICAL_LOG_DEBUG_F(fields, fmt, ...) \
	HICAL_LOG_IMPL_F_(::hical::LogLevel::hDebug, fields, fmt __VA_OPT__(, ) __VA_ARGS__)
#define HICAL_LOG_INFO_F(fields, fmt, ...) \
	HICAL_LOG_IMPL_F_(::hical::LogLevel::hInfo, fields, fmt __VA_OPT__(, ) __VA_ARGS__)
#define HICAL_LOG_WARN_F(fields, fmt, ...) \
	HICAL_LOG_IMPL_F_(::hical::LogLevel::hWarn, fields, fmt __VA_OPT__(, ) __VA_ARGS__)
#define HICAL_LOG_ERROR_F(fields, fmt, ...) \
	HICAL_LOG_IMPL_F_(::hical::LogLevel::hError, fields, fmt __VA_OPT__(, ) __VA_ARGS__)
#define HICAL_LOG_FATAL_F(fields, fmt, ...) \
	HICAL_LOG_IMPL_F_(::hical::LogLevel::hFatal, fields, fmt __VA_OPT__(, ) __VA_ARGS__)

// 辅助拼接宏（需要间接层以确保参数先展开再拼接）
#define HICAL_LOG_CONCAT_(prefix, suffix) prefix##suffix
#define HICAL_LOG_LEVEL_(level) HICAL_LOG_CONCAT_(h, level)

// ---- 通道日志宏 ----
// 用法: HICAL_LOG_TO("access", Info, "GET /api status={}", 200)
#define HICAL_LOG_TO(channel, lvlName, fmt, ...)                                   \
	do                                                                             \
	{                                                                              \
		auto hCh_ = ::hical::Logger::instance().channels().get(channel);           \
		if (hCh_ && hCh_->level() <= ::hical::LogLevel::HICAL_LOG_LEVEL_(lvlName)) \
		{                                                                          \
			::hical::LogRecord hRec_;                                              \
			hRec_.level = ::hical::LogLevel::HICAL_LOG_LEVEL_(lvlName);            \
			hRec_.timestamp = std::chrono::system_clock::now();                    \
			hRec_.file = __FILE__;                                                 \
			hRec_.line = __LINE__;                                                 \
			hRec_.message = std::format(fmt __VA_OPT__(, ) __VA_ARGS__);           \
			hCh_->emit(hRec_);                                                     \
		}                                                                          \
	}                                                                              \
	while (0)

// ---- 通道 + 结构化字段日志宏 ----
// 用法: HICAL_LOG_TO_F("access", Info, {{"status", 200}}, "GET /api")
#define HICAL_LOG_TO_F(channel, lvlName, fields, fmt, ...)                         \
	do                                                                             \
	{                                                                              \
		auto hCh_ = ::hical::Logger::instance().channels().get(channel);           \
		if (hCh_ && hCh_->level() <= ::hical::LogLevel::HICAL_LOG_LEVEL_(lvlName)) \
		{                                                                          \
			::hical::LogRecord hRec_;                                              \
			hRec_.level = ::hical::LogLevel::HICAL_LOG_LEVEL_(lvlName);            \
			hRec_.timestamp = std::chrono::system_clock::now();                    \
			hRec_.file = __FILE__;                                                 \
			hRec_.line = __LINE__;                                                 \
			hRec_.message = std::format(fmt __VA_OPT__(, ) __VA_ARGS__);           \
			hRec_.fields = (fields);                                               \
			hCh_->emit(hRec_);                                                     \
		}                                                                          \
	}                                                                              \
	while (0)

// NOLINTEND(cppcoreguidelines-macro-usage)
