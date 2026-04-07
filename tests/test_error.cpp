#include "core/Error.h"
#include <gtest/gtest.h>
#include <boost/asio/error.hpp>

using namespace hical;

// 测试无错误
TEST(ErrorCodeTest, NoError)
{
	boost::system::error_code ec;
	EXPECT_EQ(fromBoostError(ec), ErrorCode::hNoError);

	auto err = toNetworkError(ec);
	EXPECT_TRUE(err.ok());
	EXPECT_FALSE(static_cast<bool>(err));
}

// 测试 EOF
TEST(ErrorCodeTest, Eof)
{
	auto ec = boost::asio::error::eof;
	EXPECT_EQ(fromBoostError(ec), ErrorCode::hEof);

	auto err = toNetworkError(ec);
	EXPECT_TRUE(err.isEof());
	EXPECT_FALSE(err.ok());
	EXPECT_TRUE(static_cast<bool>(err));
}

// 测试连接重置
TEST(ErrorCodeTest, ConnectionReset)
{
	auto ec = boost::asio::error::connection_reset;
	EXPECT_EQ(fromBoostError(ec), ErrorCode::hConnectionReset);
}

// 测试连接被拒绝
TEST(ErrorCodeTest, ConnectionRefused)
{
	auto ec = boost::asio::error::connection_refused;
	EXPECT_EQ(fromBoostError(ec), ErrorCode::hConnectionRefused);
}

// 测试超时
TEST(ErrorCodeTest, TimedOut)
{
	auto ec = boost::asio::error::timed_out;
	EXPECT_EQ(fromBoostError(ec), ErrorCode::hTimedOut);
}

// 测试操作取消
TEST(ErrorCodeTest, OperationAborted)
{
	auto ec = boost::asio::error::operation_aborted;
	EXPECT_EQ(fromBoostError(ec), ErrorCode::hOperationAborted);

	auto err = toNetworkError(ec);
	EXPECT_TRUE(err.isCancelled());
}

// 测试连接中止
TEST(ErrorCodeTest, ConnectionAborted)
{
	auto ec = boost::asio::error::connection_aborted;
	EXPECT_EQ(fromBoostError(ec), ErrorCode::hConnectionAborted);
}

// 测试地址已在使用
TEST(ErrorCodeTest, AddressInUse)
{
	auto ec = boost::asio::error::address_in_use;
	EXPECT_EQ(fromBoostError(ec), ErrorCode::hAddressInUse);
}

// 测试网络不可达
TEST(ErrorCodeTest, NetworkUnreachable)
{
	auto ec = boost::asio::error::network_unreachable;
	EXPECT_EQ(fromBoostError(ec), ErrorCode::hNetworkUnreachable);
}

// 测试主机不可达
TEST(ErrorCodeTest, HostUnreachable)
{
	auto ec = boost::asio::error::host_unreachable;
	EXPECT_EQ(fromBoostError(ec), ErrorCode::hHostUnreachable);
}

// 测试管道破裂
TEST(ErrorCodeTest, BrokenPipe)
{
	auto ec = boost::asio::error::broken_pipe;
	EXPECT_EQ(fromBoostError(ec), ErrorCode::hBrokenPipe);
}

// 测试权限不足
TEST(ErrorCodeTest, AccessDenied)
{
	auto ec = boost::asio::error::access_denied;
	EXPECT_EQ(fromBoostError(ec), ErrorCode::hPermissionDenied);
}

// 测试描述符不足
TEST(ErrorCodeTest, NoDescriptors)
{
	auto ec = boost::asio::error::no_descriptors;
	EXPECT_EQ(fromBoostError(ec), ErrorCode::hTooManyOpenFiles);
}

// 测试 EAGAIN / EWOULDBLOCK
TEST(ErrorCodeTest, WouldBlock)
{
	auto ec = boost::asio::error::would_block;
	EXPECT_EQ(fromBoostError(ec), ErrorCode::hWouldBlock);
}

// 测试错误码字符串
TEST(ErrorCodeTest, ErrorCodeToString)
{
	EXPECT_STREQ(errorCodeToString(ErrorCode::hNoError), "No error");
	EXPECT_STREQ(errorCodeToString(ErrorCode::hEof), "End of file");
	EXPECT_STREQ(errorCodeToString(ErrorCode::hConnectionReset), "Connection reset by peer");
	EXPECT_STREQ(errorCodeToString(ErrorCode::hTimedOut), "Connection timed out");
	EXPECT_STREQ(errorCodeToString(ErrorCode::hOperationAborted), "Operation aborted");
	EXPECT_STREQ(errorCodeToString(ErrorCode::hUnknown), "Unknown error");
}

// 测试 NetworkError 结构体
TEST(ErrorCodeTest, NetworkErrorStruct)
{
	NetworkError noErr {ErrorCode::hNoError, ""};
	EXPECT_TRUE(noErr.ok());
	EXPECT_FALSE(noErr.isEof());
	EXPECT_FALSE(noErr.isCancelled());

	NetworkError eofErr {ErrorCode::hEof, "End of file"};
	EXPECT_FALSE(eofErr.ok());
	EXPECT_TRUE(eofErr.isEof());
	EXPECT_FALSE(eofErr.isCancelled());

	NetworkError cancelErr {ErrorCode::hOperationAborted, "Operation aborted"};
	EXPECT_FALSE(cancelErr.ok());
	EXPECT_FALSE(cancelErr.isEof());
	EXPECT_TRUE(cancelErr.isCancelled());
}

// 测试未知错误码
TEST(ErrorCodeTest, UnknownError)
{
	// 构造一个不在映射表中的错误码
	boost::system::error_code ec(999, boost::system::generic_category());
	EXPECT_EQ(fromBoostError(ec), ErrorCode::hUnknown);

	auto err = toNetworkError(ec);
	EXPECT_EQ(err.code, ErrorCode::hUnknown);
	EXPECT_FALSE(err.message.empty());
}
