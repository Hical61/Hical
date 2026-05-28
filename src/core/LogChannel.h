/**
 * @file LogChannel.h
 * @brief 命名日志通道与通道注册表
 */

#pragma once

#include "LogFormatter.h"
#include "LogRecord.h"
#include "LogSink.h"

#include <atomic>
#include <memory>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace hical
{

	/**
	 * @brief 命名日志通道
	 * 每个通道有独立的级别过滤、Formatter 和 Sink 列表。
	 * 用于将不同类型的日志（access、audit、perf 等）路由到不同的输出目标。
	 */
	class LogChannel
	{
	public:
		explicit LogChannel(std::string name);

		[[nodiscard]] const std::string& name() const;

		void setLevel(LogLevel lvl);
		[[nodiscard]] LogLevel level() const;

		void setFormatter(std::shared_ptr<LogFormatter> formatter);
		void addSink(std::shared_ptr<LogSink> sink);
		void clearSinks();

		/**
		 * @brief 发射日志到此通道
		 * 按 level 过滤后，用通道 Formatter 格式化，分发给通道 Sinks。
		 */
		void emit(const LogRecord& record);

	private:
		std::string name_;
		std::atomic<LogLevel> level_ {LogLevel::hTrace};
		std::shared_ptr<LogFormatter> formatter_;
		std::shared_ptr<const std::vector<std::shared_ptr<LogSink>>> sinks_;
		std::mutex mutex_;
	};

	/**
	 * @brief 日志通道注册表
	 * 线程安全（shared_mutex，读多写少模式）。
	 * 通道通常在启动时创建，运行时通过名称查找。
	 */
	class LogChannelRegistry
	{
	public:
		/**
		 * @brief 获取或创建通道
		 * @param name 通道名称
		 * @return 通道 shared_ptr
		 */
		std::shared_ptr<LogChannel> getOrCreate(const std::string& name);

		/**
		 * @brief 获取已存在的通道（不存在返回 nullptr）
		 * @param name 通道名称
		 * @return 通道 shared_ptr 或 nullptr
		 */
		[[nodiscard]] std::shared_ptr<LogChannel> get(const std::string& name) const;

		/**
		 * @brief 列出所有通道及其级别
		 * @return name→level 列表
		 */
		[[nodiscard]] std::vector<std::pair<std::string, LogLevel>> listChannels() const;

	private:
		mutable std::shared_mutex mutex_;
		std::unordered_map<std::string, std::shared_ptr<LogChannel>> channels_;
	};

} // namespace hical
