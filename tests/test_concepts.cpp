#include "core/Concepts.h"
#include "asio/AsioEventLoop.h"
#include "asio/AsioTimer.h"
#include "asio/SslConnection.h"
#include <gtest/gtest.h>

using namespace hical;

// ============ 编译期 Concept 验证（static_assert） ============

// AsioEventLoop 满足 EventLoopLike
static_assert(EventLoopLike<AsioEventLoop>, "AsioEventLoop must satisfy EventLoopLike concept");

// AsioTimer 满足 TimerLike
static_assert(TimerLike<AsioTimer>, "AsioTimer must satisfy TimerLike concept");

// PlainConnection（GenericConnection<tcp::socket>）满足 TcpConnectionLike
static_assert(TcpConnectionLike<PlainConnection>, "PlainConnection must satisfy TcpConnectionLike concept");

// SslConnection 满足 TcpConnectionLike
static_assert(TcpConnectionLike<SslConnection>, "SslConnection must satisfy TcpConnectionLike concept");

// ============ 运行时测试 ============

// 验证 AsioBackend 类型别名正确
TEST(ConceptsTest, AsioBackendTypeAliases)
{
	// 编译通过即验证了类型别名的正确性
	bool isEventLoop = std::is_same_v<AsioBackend::EventLoopType, AsioEventLoop>;
	bool isTimer = std::is_same_v<AsioBackend::TimerType, AsioTimer>;
	bool isConnection = std::is_same_v<AsioBackend::ConnectionType, TcpConnection>;

	EXPECT_TRUE(isEventLoop);
	EXPECT_TRUE(isTimer);
	EXPECT_TRUE(isConnection);
}

// 验证 EventLoopLike concept 的基本行为
TEST(ConceptsTest, EventLoopLikeBasicBehavior)
{
	AsioEventLoop loop;

	EXPECT_FALSE(loop.isRunning());
	EXPECT_EQ(loop.index(), 0);

	loop.setIndex(42);
	EXPECT_EQ(loop.index(), 42);

	// allocator 应返回有效分配器
	auto alloc = loop.allocator();
	auto* p = alloc.allocate_bytes(64);
	EXPECT_NE(p, nullptr);
	alloc.deallocate_bytes(p, 64);
}

// 验证模板化后端可作为模板参数使用
template <NetworkBackend Backend>
struct BackendTraits
{
	using Loop = typename Backend::EventLoopType;
	using Conn = typename Backend::ConnectionType;
	using Timer = typename Backend::TimerType;

	static constexpr bool hValid = true;
};

TEST(ConceptsTest, NetworkBackendAsTemplateParam)
{
	// AsioBackend 可以作为 NetworkBackend 约束的模板参数
	EXPECT_TRUE(BackendTraits<AsioBackend>::hValid);

	bool isLoop = std::is_same_v<BackendTraits<AsioBackend>::Loop, AsioEventLoop>;
	EXPECT_TRUE(isLoop);
}

// 验证 concept 对不满足条件的类型拒绝
struct IncompleteBackend
{
	using EventLoopType = int; // int 不满足 EventLoopLike
	using ConnectionType = int;
	using TimerType = int;
};

static_assert(!NetworkBackend<IncompleteBackend>, "IncompleteBackend must NOT satisfy NetworkBackend concept");

static_assert(!EventLoopLike<int>, "int must NOT satisfy EventLoopLike concept");

static_assert(!TcpConnectionLike<int>, "int must NOT satisfy TcpConnectionLike concept");

static_assert(!TimerLike<int>, "int must NOT satisfy TimerLike concept");

TEST(ConceptsTest, RejectInvalidTypes)
{
	EXPECT_FALSE(NetworkBackend<IncompleteBackend>);
	EXPECT_FALSE(EventLoopLike<int>);
	EXPECT_FALSE(TcpConnectionLike<int>);
	EXPECT_FALSE(TimerLike<int>);
}
