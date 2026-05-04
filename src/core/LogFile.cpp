#include "LogFile.h"

#include <algorithm>
#include <ctime>
#include <stdexcept>
#include <vector>

namespace hical
{

	LogFile::LogFile(Options opts) : m_opts(std::move(opts))
	{
		namespace fs = std::filesystem;
		fs::path p(m_opts.basePath);
		m_dir = p.parent_path().string();
		m_stem = p.stem().string();
		m_ext = p.extension().string();

		if (m_dir.empty())
		{
			m_dir = ".";
		}

		// 确保目录存在
		std::error_code ec;
		fs::create_directories(m_dir, ec);

		openFile();
	}

	LogFile::~LogFile()
	{
		if (m_fp != nullptr)
		{
			fflush(m_fp);
			fclose(m_fp);
			m_fp = nullptr;
		}
	}

	LogFile::LogFile(LogFile&& other) noexcept
		: m_opts(std::move(other.m_opts))
		, m_fp(other.m_fp)
		, m_writtenBytes(other.m_writtenBytes)
		, m_rotationSeq(other.m_rotationSeq)
		, m_dir(std::move(other.m_dir))
		, m_stem(std::move(other.m_stem))
		, m_ext(std::move(other.m_ext))
	{
		other.m_fp = nullptr;
		other.m_writtenBytes = 0;
	}

	LogFile& LogFile::operator=(LogFile&& other) noexcept
	{
		if (this != &other)
		{
			if (m_fp != nullptr)
			{
				fclose(m_fp);
			}
			m_opts = std::move(other.m_opts);
			m_fp = other.m_fp;
			m_writtenBytes = other.m_writtenBytes;
			m_rotationSeq = other.m_rotationSeq;
			m_dir = std::move(other.m_dir);
			m_stem = std::move(other.m_stem);
			m_ext = std::move(other.m_ext);
			other.m_fp = nullptr;
			other.m_writtenBytes = 0;
		}
		return *this;
	}

	void LogFile::append(const char* data, size_t len)
	{
		if (m_fp == nullptr)
		{
			openFile();
		}

		// 写入前检查是否需要轮转
		if (m_opts.maxFileSize > 0 && m_writtenBytes + len > m_opts.maxFileSize)
		{
			rotate();
		}

		size_t written = fwrite(data, 1, len, m_fp);
		m_writtenBytes += written;
	}

	void LogFile::flush()
	{
		if (m_fp != nullptr)
		{
			fflush(m_fp);
		}
	}

	size_t LogFile::writtenBytes() const
	{
		return m_writtenBytes;
	}

	void LogFile::openFile()
	{
		if (m_fp != nullptr)
		{
			fclose(m_fp);
		}
#if defined(_WIN32)
		m_fp = _wfopen(std::filesystem::path(m_opts.basePath).wstring().c_str(), L"ab");
#else
		m_fp = fopen(m_opts.basePath.c_str(), "ab");
#endif
		if (m_fp == nullptr)
		{
			throw std::runtime_error("LogFile: cannot open file: " + m_opts.basePath);
		}

		// 获取当前文件大小
		fseek(m_fp, 0, SEEK_END);
		auto pos = ftell(m_fp);
		m_writtenBytes = (pos >= 0) ? static_cast<size_t>(pos) : 0;
	}

	void LogFile::rotate()
	{
		if (m_fp != nullptr)
		{
			fflush(m_fp);
			fclose(m_fp);
			m_fp = nullptr;
		}

		// 生成轮转文件名
		auto newName = makeRotatedName();
		std::error_code ec;
		std::filesystem::rename(m_opts.basePath, newName, ec);

		++m_rotationSeq;

		// 清理超出限制的旧文件
		if (m_opts.maxFiles > 0)
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
		snprintf(seqBuf, sizeof(seqBuf), ".%06u", static_cast<unsigned>(m_rotationSeq % 1000000));

		// dir/stem.YYMMDD-HHMMSS.NNNNNN.ext
		return m_dir + "/" + m_stem + "." + timeBuf + seqBuf + m_ext;
	}

	void LogFile::cleanOldFiles()
	{
		namespace fs = std::filesystem;

		// 扫描目录查找匹配的轮转文件
		std::vector<std::string> rotatedFiles;
		std::error_code ec;
		for (const auto& entry : fs::directory_iterator(m_dir, ec))
		{
			if (!entry.is_regular_file())
			{
				continue;
			}
			auto name = entry.path().filename().string();
			// 轮转文件格式：stem.YYMMDD-HHMMSS.NNNNNN.ext
			// 跳过基础文件本身
			if (name == m_stem + m_ext)
			{
				continue;
			}
			// 严格匹配轮转文件名格式 stem.YYMMDD-HHMMSS.NNNNNN.ext
			if (name.starts_with(m_stem + ".") && name.ends_with(m_ext))
			{
				// 中间段应为 YYMMDD-HHMMSS.NNNNNN（20 字符）
				auto prefixLen = m_stem.size() + 1;
				auto suffixLen = m_ext.size();
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
		while (rotatedFiles.size() > m_opts.maxFiles)
		{
			fs::remove(rotatedFiles.front(), ec);
			rotatedFiles.erase(rotatedFiles.begin());
		}
	}

} // namespace hical
