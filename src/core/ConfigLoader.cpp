/**
 * @file ConfigLoader.cpp
 * @brief JSON 配置加载器实现
 */

#include "core/ConfigLoader.h"

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace hical
{

	bool ConfigLoader::loadFile(const std::string& path)
	{
		std::ifstream file(path);
		if (!file.is_open())
		{
			return false;
		}

		std::ostringstream oss;
		oss << file.rdbuf();
		return loadString(oss.str());
	}

	bool ConfigLoader::loadString(std::string_view json)
	{
		try
		{
			auto parsed = boost::json::parse(json);
			if (!parsed.is_object())
			{
				return false;
			}
			config_ = std::move(parsed).as_object();
			return true;
		}
		catch (const std::exception&)
		{
			return false;
		}
	}

	void ConfigLoader::setEnvPrefix(const std::string& prefix)
	{
		envPrefix_ = prefix;
	}

	std::optional<std::string> ConfigLoader::resolveEnv(const std::string& key) const
	{
		// 检查缓存，避免重复构造 envName + getenv 系统调用
		auto cacheIt = envCache_.find(key);
		if (cacheIt != envCache_.end())
		{
			return cacheIt->second;
		}

		// 将 key 转为环境变量名：前缀 + 大写 + 点转下划线
		std::string envName = envPrefix_;
		envName.reserve(envPrefix_.size() + key.size());

		for (char ch : key)
		{
			if (ch == '.')
			{
				envName.push_back('_');
			}
			else
			{
				envName.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(ch))));
			}
		}

		const char* envValue = std::getenv(envName.c_str());
		if (envValue == nullptr)
		{
			envCache_.emplace(key, std::nullopt);
			return std::nullopt;
		}
		auto result = std::optional<std::string>(std::string(envValue));
		envCache_.emplace(key, result);
		return result;
	}

	const boost::json::value* ConfigLoader::findValue(const std::string& key) const
	{
		if (config_.empty() || key.empty())
		{
			return nullptr;
		}

		const boost::json::object* current = &config_;
		size_t start = 0;

		// 拆分与导航合并为单 pass，无需中间 vector 分配
		for (size_t dotPos = key.find('.', start); dotPos != std::string::npos; dotPos = key.find('.', start))
		{
			auto seg = std::string_view(key).substr(start, dotPos - start);
			auto it = current->find(seg);
			if (it == current->end() || !it->value().is_object())
			{
				return nullptr;
			}
			current = &it->value().as_object();
			start = dotPos + 1;
		}

		auto lastSeg = std::string_view(key).substr(start);
		auto it = current->find(lastSeg);
		if (it == current->end())
		{
			return nullptr;
		}
		return &it->value();
	}

} // namespace hical
