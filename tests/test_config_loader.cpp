/**
 * @file test_config_loader.cpp
 * @brief ConfigLoader 配置加载器单元测试
 */

#include "core/ConfigLoader.h"
#include <gtest/gtest.h>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <vector>

using namespace hical;

// ──────────────────────────────────────────────
// 1. loadString 加载合法 JSON 成功
// ──────────────────────────────────────────────
TEST(ConfigLoaderTest, LoadString_ValidJson_ReturnsTrue)
{
	ConfigLoader loader;
	EXPECT_TRUE(loader.loadString(R"({"host":"localhost","port":8080})"));
}

// ──────────────────────────────────────────────
// 2. 单层 key 访问
// ──────────────────────────────────────────────
TEST(ConfigLoaderTest, Get_SingleLevelKey_ReturnsValue)
{
	ConfigLoader loader;
	ASSERT_TRUE(loader.loadString(R"({"host":"localhost","port":8080})"));

	EXPECT_EQ(loader.get<std::string>("host"), "localhost");
	EXPECT_EQ(loader.get<int64_t>("port"), 8080);
}

// ──────────────────────────────────────────────
// 3. 嵌套 key 访问：db.port → 3306
// ──────────────────────────────────────────────
TEST(ConfigLoaderTest, Get_NestedKey_ReturnsNestedValue)
{
	ConfigLoader loader;
	ASSERT_TRUE(loader.loadString(R"({"db":{"host":"127.0.0.1","port":3306}})"));
	EXPECT_EQ(loader.get<std::string>("db.host"), "127.0.0.1");
	EXPECT_EQ(loader.get<int64_t>("db.port"), 3306);
}

// ──────────────────────────────────────────────
// 4. 多级嵌套：a.b.c
// ──────────────────────────────────────────────
TEST(ConfigLoaderTest, Get_MultiLevelNested_ReturnsDeepValue)
{
	ConfigLoader loader;
	ASSERT_TRUE(loader.loadString(R"({"a":{"b":{"c":"deep"}}})"));
	EXPECT_EQ(loader.get<std::string>("a.b.c"), "deep");
}

// ──────────────────────────────────────────────
// 5. 缺失 key 返回默认值
// ──────────────────────────────────────────────
TEST(ConfigLoaderTest, Get_MissingKey_ReturnsDefault)
{
	ConfigLoader loader;
	ASSERT_TRUE(loader.loadString(R"({"host":"localhost"})"));

	EXPECT_EQ(loader.get<int64_t>("missing", 42), 42);
	EXPECT_EQ(loader.get<std::string>("nested.missing", "fallback"), "fallback");
	EXPECT_EQ(loader.get<bool>("flag", true), true);
	EXPECT_EQ(loader.get<double>("score", 3.14), 3.14);
}

// ──────────────────────────────────────────────
// 6. 类型不匹配抛异常
// ──────────────────────────────────────────────
TEST(ConfigLoaderTest, Get_TypeMismatch_ThrowsRuntimeError)
{
	ConfigLoader loader;
	ASSERT_TRUE(loader.loadString(R"({"host":"localhost","count":"not_a_number"})"));

	// 把字符串当 int 取
	EXPECT_THROW(loader.get<int64_t>("host"), std::runtime_error);
	// 把字符串当 bool 取
	EXPECT_THROW(loader.get<bool>("host"), std::runtime_error);
	// 把字符串当 double 取
	EXPECT_THROW(loader.get<double>("host"), std::runtime_error);
	// 非数组当 vector<string> 取
	EXPECT_THROW((loader.get<std::vector<std::string>>("host")), std::runtime_error);
}

// ──────────────────────────────────────────────
// 7. 环境变量覆盖配置文件值
// ──────────────────────────────────────────────
TEST(ConfigLoaderTest, Get_EnvVarOverridesConfigValue)
{
	ConfigLoader loader;
	ASSERT_TRUE(loader.loadString(R"({"db":{"host":"127.0.0.1","port":3306}})"));
	loader.setEnvPrefix("HICAL_");

	// 设置环境变量 HICAL_DB_PORT=9999
	constexpr const char* kEnvName = "HICAL_DB_PORT";
	constexpr const char* kEnvValue = "9999";
#ifdef _WIN32
	_putenv_s(kEnvName, kEnvValue);
#else
	setenv(kEnvName, kEnvValue, 1);
#endif

	EXPECT_EQ(loader.get<int64_t>("db.port"), 9999);

	// 清理环境变量，避免影响后续测试
#ifdef _WIN32
	_putenv_s(kEnvName, "");
#else
	unsetenv(kEnvName);
#endif
}

// ──────────────────────────────────────────────
// 7b. 环境变量覆盖字符串值（包含大小写和分隔符转换验证）
// ──────────────────────────────────────────────
TEST(ConfigLoaderTest, Get_EnvVarOverridesStringConfig)
{
	ConfigLoader loader;
	ASSERT_TRUE(loader.loadString(R"({"app":{"name":"default"}})"));
	loader.setEnvPrefix("MYAPP_");

#ifdef _WIN32
	_putenv_s("MYAPP_APP_NAME", "production");
#else
	setenv("MYAPP_APP_NAME", "production", 1);
#endif

	EXPECT_EQ(loader.get<std::string>("app.name"), "production");

#ifdef _WIN32
	_putenv_s("MYAPP_APP_NAME", "");
#else
	unsetenv("MYAPP_APP_NAME");
#endif
}

// ──────────────────────────────────────────────
// 8. loadFile 不存在的文件返回 false
// ──────────────────────────────────────────────
TEST(ConfigLoaderTest, LoadFile_FileNotFound_ReturnsFalse)
{
	ConfigLoader loader;
	EXPECT_FALSE(loader.loadFile("/nonexistent/path/config.json"));
}

// ──────────────────────────────────────────────
// 9. loadString 非法 JSON 返回 false
// ──────────────────────────────────────────────
TEST(ConfigLoaderTest, LoadString_InvalidJson_ReturnsFalse)
{
	ConfigLoader loader;
	EXPECT_FALSE(loader.loadString(R"({invalid json})"));
	EXPECT_FALSE(loader.loadString(R"("just a string, not an object")"));
	EXPECT_FALSE(loader.loadString(""));
}

// ──────────────────────────────────────────────
// 10a. 支持不同 type：string / int64_t / bool / double
// ──────────────────────────────────────────────
TEST(ConfigLoaderTest, Get_PrimitiveTypes_ReturnsCorrectValues)
{
	ConfigLoader loader;
	ASSERT_TRUE(loader.loadString(R"({
        "name": "hical",
        "port": 8080,
        "debug": true,
        "timeout": 30.5
    })"));

	EXPECT_EQ(loader.get<std::string>("name"), "hical");
	EXPECT_EQ(loader.get<int64_t>("port"), 8080);
	EXPECT_EQ(loader.get<bool>("debug"), true);
	EXPECT_DOUBLE_EQ(loader.get<double>("timeout"), 30.5);
}

// ──────────────────────────────────────────────
// 10b. 支持 vector<string> 类型
// ──────────────────────────────────────────────
TEST(ConfigLoaderTest, Get_StringVector_ReturnsCorrectValues)
{
	ConfigLoader loader;
	ASSERT_TRUE(loader.loadString(R"({
        "allowed_hosts": ["example.com", "test.com", "localhost"]
    })"));

	auto hosts = loader.get<std::vector<std::string>>("allowed_hosts");
	ASSERT_EQ(hosts.size(), 3);
	EXPECT_EQ(hosts[0], "example.com");
	EXPECT_EQ(hosts[1], "test.com");
	EXPECT_EQ(hosts[2], "localhost");
}

// ──────────────────────────────────────────────
// 10c. vector<string> 默认值
// ──────────────────────────────────────────────
TEST(ConfigLoaderTest, Get_StringVectorMissing_ReturnsDefault)
{
	ConfigLoader loader;
	ASSERT_TRUE(loader.loadString(R"({"name":"hical"})"));

	std::vector<std::string> defaultVal = {"a", "b"};
	auto result = loader.get<std::vector<std::string>>("missing_array", defaultVal);
	ASSERT_EQ(result.size(), 2);
	EXPECT_EQ(result[0], "a");
	EXPECT_EQ(result[1], "b");
}

// ──────────────────────────────────────────────
// 11. resolveEnv 正确转换 key 名
// ──────────────────────────────────────────────
TEST(ConfigLoaderTest, ResolveEnv_ConvertsKeyToEnvName)
{
	ConfigLoader loader;
	loader.setEnvPrefix("APP_");

	// 无环境变量时返回 nullopt
	EXPECT_FALSE(loader.resolveEnv("nonexistent_key").has_value());
}

// ──────────────────────────────────────────────
// 12. 空 key 查询返回默认值
// ──────────────────────────────────────────────
TEST(ConfigLoaderTest, Get_EmptyKey_ReturnsDefault)
{
	ConfigLoader loader;
	ASSERT_TRUE(loader.loadString(R"({"host":"localhost"})"));

	EXPECT_EQ(loader.get<std::string>("", "default"), "default");
}

// ──────────────────────────────────────────────
// 13. 部分匹配的嵌套 key（中间段不存在）
// ──────────────────────────────────────────────
TEST(ConfigLoaderTest, Get_PartialNestedKeyMissing_ReturnsDefault)
{
	ConfigLoader loader;
	ASSERT_TRUE(loader.loadString(R"({"db":{"host":"localhost"}})"));

	EXPECT_EQ(loader.get<std::string>("db.missing.deep", "nope"), "nope");
}

// ──────────────────────────────────────────────
// 14. 嵌套路径中间段不是 object 时返回默认值
// ──────────────────────────────────────────────
TEST(ConfigLoaderTest, Get_MidPathNotObject_ReturnsDefault)
{
	ConfigLoader loader;
	ASSERT_TRUE(loader.loadString(R"({"db":"flat_string"})"));

	// "db" 是 string 不是 object，访问 "db.host" 应返回默认值
	EXPECT_EQ(loader.get<std::string>("db.host", "fallback"), "fallback");
}
