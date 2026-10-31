/**
 * @file InteropCommon.h
 * @brief 互操作探针的参数解析、报告写入和进程超时
 */

#pragma once

#include "core/Log.h"
#include <boost/json.hpp>
#include <openssl/evp.h>
#include <array>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace interop
{
	inline constexpr std::string_view kHello = "Hello, Hical!\n";
	inline constexpr size_t kMaxFixtureBytes = 16 * 1024 * 1024;

	/**
	 * @brief 解析不重复的“--名称 值”参数，由各入口指定允许的参数名。
	 */
	class Arguments
	{
	public:
		/**
		 * @brief 解析命令行参数，缺值或名称重复时抛出异常。
		 * @param argc 参数数量
		 * @param argv main 接收的原始参数
		 */
		Arguments(int argc, char** argv)
		{
			for (int index = 1; index < argc; index += 2)
			{
				const std::string name = argv[index];
				if (!name.starts_with("--") || index + 1 == argc || !values_.emplace(name, argv[index + 1]).second)
				{
					throw std::invalid_argument("参数须为不重复的 --名称 值");
				}
			}
		}

		/**
		 * @brief 检查是否传入了不支持的参数。
		 * @param names 本探针允许的参数名
		 */
		void allow(std::initializer_list<std::string_view> names) const
		{
			for (const auto& [name, value] : values_)
			{
				bool found = false;
				for (const auto allowed : names)
				{
					found = found || name == allowed;
				}
				if (!found)
				{
					throw std::invalid_argument("未知参数：" + name);
				}
			}
		}

		/**
		 * @brief 读取参数，未提供时使用默认值。
		 * @param name 参数名
		 * @param fallback 默认值
		 * @return 参数值或默认值
		 */
		[[nodiscard]] std::string get(std::string_view name, std::string fallback = {}) const
		{
			const auto found = values_.find(name);
			return found == values_.end() ? std::move(fallback) : found->second;
		}

		/**
		 * @brief 读取必填参数，缺失或为空时抛出异常。
		 * @param name 参数名
		 * @return 参数值
		 */
		[[nodiscard]] std::string require(std::string_view name) const
		{
			auto value = get(name);
			if (value.empty())
			{
				throw std::invalid_argument("缺少必填参数：" + std::string(name));
			}
			return value;
		}

		/**
		 * @brief 读取整数参数，格式错误或越界时抛出异常。
		 * @param name 参数名
		 * @param fallback 默认值
		 * @param minimum 最小值，含边界
		 * @param maximum 最大值，含边界
		 * @return 解析后的整数
		 */
		[[nodiscard]] int number(std::string_view name, int fallback, int minimum, int maximum) const
		{
			const auto value = get(name, std::to_string(fallback));
			int result = 0;
			const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
			if (parsed.ec != std::errc() || parsed.ptr != value.data() + value.size() || result < minimum
				|| result > maximum)
			{
				throw std::invalid_argument("整数参数越界：" + std::string(name));
			}
			return result;
		}

	private:
		std::map<std::string, std::string, std::less<>> values_;
	};

	/**
	 * @brief 读取二进制夹具，文件超过 16 MiB 时抛出异常。
	 * @param path 文件路径
	 * @return 拥有全部文件内容的字符串
	 */
	inline std::string readBinary(const std::filesystem::path& path)
	{
		std::ifstream input(path, std::ios::binary | std::ios::ate);
		const auto length = input.tellg();
		if (!input || length < 0 || static_cast<uint64_t>(length) > kMaxFixtureBytes)
		{
			throw std::runtime_error("夹具文件不存在或超过 16 MiB：" + path.string());
		}
		std::string result(static_cast<size_t>(length), '\0');
		input.seekg(0);
		input.read(result.data(), static_cast<std::streamsize>(result.size()));
		if (!input)
		{
			throw std::runtime_error("夹具文件未读完整：" + path.string());
		}
		return result;
	}

	/**
	 * @brief 计算全部输入字节的 SHA256。
	 * @param bytes 输入字节
	 * @return 小写十六进制摘要
	 */
	inline std::string sha256(std::string_view bytes)
	{
		std::array<unsigned char, EVP_MAX_MD_SIZE> hash {};
		unsigned int length = 0;
		if (EVP_Digest(bytes.data(), bytes.size(), hash.data(), &length, EVP_sha256(), nullptr) != 1)
		{
			throw std::runtime_error("SHA256 计算失败");
		}
		constexpr std::string_view kHex = "0123456789abcdef";
		std::string result;
		result.reserve(length * 2);
		for (unsigned int index = 0; index < length; ++index)
		{
			result.push_back(kHex[hash[index] >> 4]);
			result.push_back(kHex[hash[index] & 15]);
		}
		return result;
	}

	/**
	 * @brief 先写临时文件，再重命名为报告文件；目标已存在时抛出异常。
	 * @param path 报告路径
	 * @param value 要保存的 JSON
	 */
	inline void writeJson(const std::filesystem::path& path, const boost::json::value& value)
	{
		if (!path.parent_path().empty())
		{
			std::filesystem::create_directories(path.parent_path());
		}
		if (std::filesystem::exists(path))
		{
			throw std::runtime_error("报告路径已存在，拒绝混入旧结果：" + path.string());
		}
		const auto temporary = path.string() + ".tmp";
		std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
		output << boost::json::serialize(value) << '\n';
		output.close();
		if (!output)
		{
			throw std::runtime_error("报告写入失败：" + path.string());
		}
		std::filesystem::rename(temporary, path);
	}

	/**
	 * @brief 不依赖 io_context 的进程看门狗，超时以退出码 124 结束进程。
	 */
	class ProcessDeadline
	{
	public:
		/**
		 * @brief 启动看门狗，析构时取消等待并回收线程。
		 * @param timeout 允许的运行时间
		 */
		explicit ProcessDeadline(std::chrono::milliseconds timeout)
			: thread_(
				  [this, timeout]()
				  {
					  std::unique_lock lock(mutex_);
					  if (!changed_.wait_for(lock,
											 timeout,
											 [this]()
											 {
												 return finished_;
											 }))
					  {
						  // 即使事件循环或清理协程卡住，也直接退出，由父进程记录退出码。
						  std::_Exit(124);
					  }
				  })
		{
		}

		/**
		 * @brief 取消超时等待并回收看门狗线程。
		 */
		~ProcessDeadline()
		{
			{
				std::lock_guard lock(mutex_);
				finished_ = true;
			}
			changed_.notify_one();
			thread_.join();
		}

		ProcessDeadline(const ProcessDeadline&) = delete;
		ProcessDeadline& operator=(const ProcessDeadline&) = delete;

	private:
		std::mutex mutex_;
		std::condition_variable changed_;
		bool finished_ {false};
		std::thread thread_;
	};
} // namespace interop
