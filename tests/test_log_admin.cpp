#include "core/Log.h"
#include "core/LogAdmin.h"
#include "core/LogChannel.h"
#include "core/Router.h"
#include <gtest/gtest.h>
#include <string>

using namespace hical;

// LogAdmin 的端点需要 Router 实例，但不需要完整的 HTTP 服务器。
// 这里测试注册不会崩溃，以及级别解析的正确性。

TEST(LogAdminTest, RegisterEndpoints)
{
	Router router;
	// 注册不应抛异常
	EXPECT_NO_THROW(registerLogAdminEndpoints(router));
}

TEST(LogAdminTest, RegisterWithCustomPrefix)
{
	Router router;
	EXPECT_NO_THROW(registerLogAdminEndpoints(router, "/api/v1/admin"));
}

// 注意：完整的端点响应测试需要构造 HttpRequest 并调用 router.dispatch()，
// 这需要 io_context 协程环境。这里仅验证注册不崩溃和基本的级别解析逻辑。

TEST(LogAdminTest, LogLevelRoundTrip)
{
	// 验证所有级别名称都能正确往返
	auto levels =
		{LogLevel::hTrace, LogLevel::hDebug, LogLevel::hInfo, LogLevel::hWarn, LogLevel::hError, LogLevel::hFatal};

	for (auto lvl : levels)
	{
		auto str = logLevelToString(lvl);
		EXPECT_NE(str, nullptr);
		EXPECT_STRNE(str, "UNKNOWN") << "Level " << static_cast<int>(lvl) << " returned UNKNOWN";
	}
}

TEST(LogAdminTest, ChannelLevelDynamic)
{
	// 模拟 PUT /admin/log-level 的核心逻辑：动态调整通道级别
	auto ch = Logger::instance().channels().getOrCreate("admin_test");
	ch->setLevel(LogLevel::hInfo);
	EXPECT_EQ(ch->level(), LogLevel::hInfo);

	ch->setLevel(LogLevel::hDebug);
	EXPECT_EQ(ch->level(), LogLevel::hDebug);

	ch->setLevel(LogLevel::hError);
	EXPECT_EQ(ch->level(), LogLevel::hError);
}
