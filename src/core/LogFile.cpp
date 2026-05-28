/**
 * @file LogFile.cpp
 * @brief 日志文件轮转引擎实现
 */

#include "LogFile.h"

#include <algorithm>
#include <ctime>
#include <stdexcept>
#include <vector>

namespace hical
{

	LogFile::LogFile(Options opts) : opts_(std::move(opts))
	{
		namespace fs = std::filesystem;
		fs::path p(opts_.basePath);
		dir_ = p.parent_path().string();
		stem_ = p.stem().string();
		ext_ = p.extension().string();

		if (dir_.empty())
		{
			dir_ = ".";
		}

		// 确保目录存在
		std::error_code ec;
		fs::create_directories(dir_, ec);

		openFile();
	}

	LogFile::~LogFile()
	{
		if (fp_ != nullptr)
		{
			fflush(fp_);
			fclose(fp_);
			fp_ = nullptr;
		}
	}

	LogFile::LogFile(LogFile&& other) noexcept
		: opts_(std::move(other.opts_))
		, fp_(other.fp_)
		, writtenBytes_(other.writtenBytes_)
		, rotationSeq_(other.rotationSeq_)
		, dir_(std::move(other.dir_))
		, stem_(std::move(other.stem_))
		, ext_(std::move(other.ext_))
	{
		other.fp_ = nullptr;
		other.writtenBytes_ = 0;
	}

	LogFile& LogFile::operator=(LogFile&& other) noexcept
	{
		if (this != &other)
		{
			if (fp_ != nullptr)
			{
				fclose(fp_);
			}
			opts_ = std::move(other.opts_);
			fp_ = other.fp_;
			writtenBytes_ = other.writtenBytes_;
			rotationSeq_ = other.rotationSeq_;
			dir_ = std::move(other.dir_);
			stem_ = std::move(other.stem_);
			ext_ = std::move(other.ext_);
			other.fp_ = nullptr;
			other.writtenBytes_ = 0;
		}
		return *this;
	}

	void LogFile::append(const char* data, size_t len)
	{
		if (fp_ == nullptr)
		{
			openFile();
		}

		// 写入前检查是否需要轮转
		if (opts_.maxFileSize > 0 && writtenBytes_ + len > opts_.maxFileSize)
		{
			rotate();
		}

		size_t written = fwrite(data, 1, len, fp_);
		writtenBytes_ += written;
	}

	void LogFile::flush()
	{
		if (fp_ != nullptr)
		{
			fflush(fp_);
		}
	}

	size_t LogFile::writtenBytes() const
	{
		return writtenBytes_;
	}

	void LogFile::openFile()
	{
		if (fp_ != nullptr)
		{
			fclose(fp_);
		}
#if defined(_WIN32)
		fp_ = _wfopen(std::filesystem::path(opts_.basePath).wstring().c_str(), L"ab");
#else
		fp_ = fopen(opts_.basePath.c_str(), "ab");
#endif
		if (fp_ == nullptr)
		{
			throw std::runtime_error("LogFile: cannot open file: " + opts_.basePath);
		}

		// 获取当前文件大小
		fseek(fp_, 0, SEEK_END);
		auto pos = ftell(fp_);
		writtenBytes_ = (pos >= 0) ? static_cast<size_t>(pos) : 0;
	}

	void LogFile::rotate()
	{
		if (fp_ != nullptr)
		{
			fflush(fp_);
			fclose(fp_);
			fp_ = nullptr;
		}

		// 生成轮转文件名
		auto newName = makeRotatedName();
		std::error_code ec;
		std::filesystem::rename(opts_.basePath, newName, ec);

		++rotationSeq_;

		// 清理超出限制的旧文件
		if (opts_.maxFiles > 0)
		{
			cleanOldFiles();
		}

		// 重新打开基础文件
		openFile();
	}

	std::string LogFile::makeRotatedName() const
	{
		auto now = std::chrono::system_clock::now();
		auto nowSec = std::chrono::system_clock::to_time_t(now);
		struct tm tmInfo {};
#if defined(_WIN32)
		localtime_s(&tmInfo, &nowSec);
#else
		localtime_r(&nowSec, &tmInfo);
#endif
		char timeBuf[32];
		snprintf(timeBuf,
				 sizeof(timeBuf),
				 "%02d%02d%02d-%02d%02d%02d",
				 (tmInfo.tm_year + 1900) % 100,
				 tmInfo.tm_mon + 1,
				 tmInfo.tm_mday,
				 tmInfo.tm_hour,
				 tmInfo.tm_min,
				 tmInfo.tm_sec);

		char seqBuf[16];
		snprintf(seqBuf, sizeof(seqBuf), ".%06u", static_cast<unsigned>(rotationSeq_ % 1000000));

		// dir/stem.YYMMDD-HHMMSS.NNNNNN.ext
		return dir_ + "/" + stem_ + "." + timeBuf + seqBuf + ext_;
	}

	void LogFile::cleanOldFiles()
	{
		namespace fs = std::filesystem;

		// 扫描目录查找匹配的轮转文件
		std::vector<std::string> rotatedFiles;
		std::error_code ec;
		for (const auto& entry : fs::directory_iterator(dir_, ec))
		{
			if (!entry.is_regular_file())
			{
				continue;
			}
			auto name = entry.path().filename().string();
			// 轮转文件格式：stem.YYMMDD-HHMMSS.NNNNNN.ext
			// 跳过基础文件本身
			if (name == stem_ + ext_)
			{
				continue;
			}
			// 严格匹配轮转文件名格式 stem.YYMMDD-HHMMSS.NNNNNN.ext
			if (name.starts_with(stem_ + ".") && name.ends_with(ext_))
			{
				// 中间段应为 YYMMDD-HHMMSS.NNNNNN（20 字符）
				auto prefixLen = stem_.size() + 1;
				auto suffixLen = ext_.size();
				if (name.size() > prefixLen + suffixLen)
				{
					auto middle = name.substr(prefixLen, name.size() - prefixLen - suffixLen);
					if (middle.size() == 20 && middle[6] == '-' && middle[13] == '.')
					{
						rotatedFiles.push_back(entry.path().string());
					}
				}
			}
		}

		// 按文件名排序（时间戳编码在名称中，字典序 = 时间序）
		std::sort(rotatedFiles.begin(), rotatedFiles.end());

		// 删除最老的文件，直到不超过 maxFiles
		while (rotatedFiles.size() > opts_.maxFiles)
		{
			fs::remove(rotatedFiles.front(), ec);
			rotatedFiles.erase(rotatedFiles.begin());
		}
	}

} // namespace hical
