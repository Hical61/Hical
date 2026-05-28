/**
 * @file LogFile.h
 * @brief 日志文件轮转引擎
 */

#pragma once

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>

namespace hical
{

	/**
	 * @brief 日志文件轮转引擎
	 * 按文件大小轮转，保留最大文件数限制。
	 * 当前文件始终为 basePath（如 app.log），轮转时 rename 为带时间戳的归档文件。
	 * **不是线程安全的**：调用方（FileSink/AsyncFileSink）负责同步。
	 */
	class LogFile
	{
	public:
		struct Options
		{
			std::string basePath = "app.log";
			size_t maxFileSize = 100 * 1024 * 1024; // 100MB
			size_t maxFiles = 10;
		};

		explicit LogFile(Options opts);
		~LogFile();

		LogFile(const LogFile&) = delete;
		LogFile& operator=(const LogFile&) = delete;
		LogFile(LogFile&&) noexcept;
		LogFile& operator=(LogFile&&) noexcept;

		/**
		 * @brief 追加数据到当前日志文件
		 * 超过 maxFileSize 时自动触发轮转。
		 */
		void append(const char* data, size_t len);

		/**
		 * @brief 刷新文件缓冲区
		 */
		void flush();

		/**
		 * @brief 获取当前文件已写入字节数
		 */
		[[nodiscard]] size_t writtenBytes() const;

	private:
		void openFile();
		void rotate();
		void cleanOldFiles();
		[[nodiscard]] std::string makeRotatedName() const;

		Options opts_;
		FILE* fp_ {nullptr};
		size_t writtenBytes_ {0};
		uint64_t rotationSeq_ {0};
		// 预解析的路径组件
		std::string dir_;
		std::string stem_;
		std::string ext_;
	};

} // namespace hical
