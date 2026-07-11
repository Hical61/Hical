/**
 * @file ConfigLoader.h
 * @brief JSON 配置加载器，支持层级字段访问和环境变量覆盖
 * 使用示例：
 * ```cpp
 * ConfigLoader loader;
 * loader.loadFile("config.json");
 * auto host = loader.get<std::string>("db.host", "localhost");
 * auto port = loader.get<int64_t>("db.port", 3306);
 * ```
 * 环境变量覆盖规则：
 * - 前缀默认为 "HICAL_"，可通过 setEnvPrefix() 修改
 * - key 中的点转为下划线，字母转大写：db.host → HICAL_DB_HOST
 * - 环境变量优先级高于 JSON 配置文件
 */

#pragma once

#include <boost/json.hpp>

#include <charconv>
#include <cstdint>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace hical
{

	/**
	 * @brief JSON 配置加载器
	 * 从 JSON 文件或字符串加载配置，支持用 "." 分隔的层级 key 访问嵌套字段，
	 * 并自动检测环境变量覆盖（环境变量优先级高于文件配置）。
	 * @note 线程安全：本类不提供内部同步，预期在单线程启动阶段使用。
	 * 运行时并发调用 loadFile/setEnvPrefix 和 get<T> 是数据竞争。
	 */
	class ConfigLoader
	{
	public:
		ConfigLoader() = default;

		/**
		 * @brief 从文件加载 JSON 配置
		 * @param path 配置文件路径
		 * @return 加载成功返回 true，文件不存在或 JSON 解析失败返回 false
		 */
		bool loadFile(const std::string& path);

		/**
		 * @brief 从字符串加载 JSON 配置
		 * @param json JSON 字符串
		 * @return 解析成功返回 true，格式错误返回 false
		 */
		bool loadString(std::string_view json);

		/**
		 * @brief 获取配置值（层级访问）
		 * 查找优先级：环境变量 > JSON 配置 > 默认值。
		 * key 用 "." 分隔层级，如 "db.host" 对应 JSON 中的 {"db":{"host":"..."}}。
		 * @tparam T 目标类型，支持 int64_t / std::string / bool / double / std::vector<std::string>
		 * @param key 配置键，支持 "." 分隔的嵌套路径
		 * @param defaultVal key 不存在时返回的默认值
		 * @return 配置值或默认值
		 * @throws std::runtime_error 类型不匹配
		 */
		template <typename T>
		T get(const std::string& key, const T& defaultVal = {}) const;

		/**
		 * @brief 设置环境变量前缀
		 * @param prefix 前缀字符串，默认为 "HICAL_"
		 */
		void setEnvPrefix(const std::string& prefix);

		/**
		 * @brief 检查环境变量覆盖（带缓存）
		 * 首次调用时构造环境变量名、调用 getenv，结果缓存到 envCache_，
		 * 后续相同 key 直接返回缓存值，避免重复构造字符串 + 系统调用。
		 * @param key 配置键
		 * @return 环境变量存在时返回其值，否则返回 std::nullopt
		 */
		[[nodiscard]] std::optional<std::string> resolveEnv(const std::string& key) const;

	private:
		/**
		 * @brief 在 JSON 对象树中按 "." 分隔路径查找值
		 * @param key 用 "." 分隔的路径
		 * @return 找到则返回指向值的指针，否则返回 nullptr
		 */
		[[nodiscard]] const boost::json::value* findValue(const std::string& key) const;

		boost::json::object config_;
		std::string envPrefix_ = "HICAL_";
		mutable std::map<std::string, std::optional<std::string>> envCache_;
	};

	// ──────────────────────────────────────────────
	// get<T>() 模板实现
	// ──────────────────────────────────────────────

	template <typename T>
	T ConfigLoader::get(const std::string& key, const T& defaultVal) const
	{
		// 1. 优先检查环境变量覆盖（仅对标量类型有意义）
		if constexpr (std::is_same_v<T, std::string> || std::is_same_v<T, int64_t> || std::is_same_v<T, bool>
					  || std::is_same_v<T, double>)
		{
			auto envVal = resolveEnv(key);
			if (envVal.has_value())
			{
				if constexpr (std::is_same_v<T, std::string>)
				{
					return *envVal;
				}
				else if constexpr (std::is_same_v<T, int64_t>)
				{
					int64_t result = 0;
					auto [ptr, ec] = std::from_chars(envVal->data(), envVal->data() + envVal->size(), result);
					if (ec != std::errc())
					{
						throw std::runtime_error("ConfigLoader: cannot convert env value '" + *envVal
												 + "' to int64_t for key '" + key + "'");
					}
					return result;
				}
				else if constexpr (std::is_same_v<T, bool>)
				{
					auto& s = *envVal;
					if (s == "true" || s == "1")
					{
						return true;
					}
					if (s == "false" || s == "0")
					{
						return false;
					}
					throw std::runtime_error("ConfigLoader: cannot convert env value '" + s + "' to bool for key '"
											 + key + "' (expected \"true\", \"1\", \"false\", or \"0\")");
				}
				else if constexpr (std::is_same_v<T, double>)
				{
					double result = 0.0;
					auto [ptr, ec] = std::from_chars(envVal->data(), envVal->data() + envVal->size(), result);
					if (ec != std::errc())
					{
						throw std::runtime_error("ConfigLoader: cannot convert env value '" + *envVal
												 + "' to double for key '" + key + "'");
					}
					return result;
				}
			}
		}

		// 2. 回退到 JSON 配置树
		auto* val = findValue(key);
		if (val == nullptr)
		{
			return defaultVal;
		}

		// 3. 类型提取
		if constexpr (std::is_same_v<T, int64_t>)
		{
			if (!val->is_int64())
			{
				throw std::runtime_error("ConfigLoader: key '" + key + "' is not an integer");
			}
			return val->as_int64();
		}
		else if constexpr (std::is_same_v<T, std::string>)
		{
			if (!val->is_string())
			{
				throw std::runtime_error("ConfigLoader: key '" + key + "' is not a string");
			}
			return std::string(val->as_string());
		}
		else if constexpr (std::is_same_v<T, bool>)
		{
			if (!val->is_bool())
			{
				throw std::runtime_error("ConfigLoader: key '" + key + "' is not a boolean");
			}
			return val->as_bool();
		}
		else if constexpr (std::is_same_v<T, double>)
		{
			if (!val->is_double())
			{
				throw std::runtime_error("ConfigLoader: key '" + key + "' is not a double");
			}
			return val->as_double();
		}
		else if constexpr (std::is_same_v<T, std::vector<std::string>>)
		{
			if (!val->is_array())
			{
				throw std::runtime_error("ConfigLoader: key '" + key + "' is not an array");
			}
			std::vector<std::string> result;
			for (const auto& elem : val->as_array())
			{
				if (!elem.is_string())
				{
					throw std::runtime_error("ConfigLoader: key '" + key + "' array element is not a string");
				}
				result.push_back(std::string(elem.as_string()));
			}
			return result;
		}
		else
		{
			static_assert(sizeof(T) == 0, "ConfigLoader::get<T>: unsupported type");
		}
	}

} // namespace hical
