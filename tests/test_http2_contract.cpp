/**
 * @file test_http2_contract.cpp
 * @brief HTTP 协议、资源预算与尾字段的公开契约
 */

#include "core/Http2Options.h"
#include "core/HttpRequest.h"
#include "core/HttpResponse.h"
#include <gtest/gtest.h>
#include <chrono>
#include <utility>

using namespace hical;

TEST(Http2ContractTest, RequestDefaultsToHttp1AndKeepsVersionFields)
{
	HttpRequest request;
	EXPECT_EQ(request.protocol(), HttpProtocol::hHttp1);
	EXPECT_EQ(request.native().httpVersionMajor, 1);
	EXPECT_EQ(request.native().httpVersionMinor, 1);

	request.native().protocol = HttpProtocol::hHttp2;
	EXPECT_EQ(request.protocol(), HttpProtocol::hHttp2);
	EXPECT_EQ(request.native().httpVersionMinor, 1);
}

TEST(Http2ContractTest, RequestTrailersOwnBytesAndRemainSeparateAfterMove)
{
	HttpRequest request;
	request.setHeader("X-Checksum", "initial");
	{
		std::string checksum = "final";
		request.native().trailers.set("x-checksum", checksum);
	}
	HttpRequest moved = std::move(request);
	EXPECT_EQ(moved.header("x-checksum"), "initial");
	EXPECT_EQ(moved.trailer("X-Checksum"), "final");
	EXPECT_EQ(moved.trailers().size(), 1U);
}

TEST(Http2ContractTest, ResponseTrailersDoNotBecomeInitialHeaders)
{
	auto response = HttpResponse::ok("body");
	response.setHeader("X-Checksum", "initial");
	response.setTrailer("X-Checksum", "final");
	response.setTrailer("bad\r\nname", "ignored");
	response.setTrailer("x-injection", "bad\r\nvalue");
	EXPECT_EQ(response.header("x-checksum"), "initial");
	EXPECT_EQ(response.trailer("x-checksum"), "final");
	EXPECT_EQ(response.trailers().size(), 1U);
	EXPECT_EQ(response.native().httpVersionMinor, 1);
}

TEST(Http2ContractTest, DefaultBudgetsAreFiniteAndConsistent)
{
	Http2Options options;
	EXPECT_TRUE(options.validationError().empty());
	EXPECT_NO_THROW(options.validate());
	EXPECT_EQ(options.maxConcurrentStreams, 64U);
	EXPECT_EQ(options.maxApplicationTasks, 64U);
	EXPECT_EQ(options.maxServerApplicationTasks, 4096U);
	EXPECT_EQ(options.maxHeaderBytes, 8U * 1024U);
	EXPECT_EQ(options.maxHeaderFields, RequestHeaders::kMaxHeaders);
	EXPECT_EQ(options.maxConnectionReceiveBytes, 8U * 1024U * 1024U);
	EXPECT_EQ(options.maxConnectionSendBytes, 8U * 1024U * 1024U);
	EXPECT_EQ(options.maxResponseBytes, 16U * 1024U * 1024U);
	EXPECT_EQ(options.maxClientConnectionReceiveBytes, 32U * 1024U * 1024U);
}

TEST(Http2ContractTest, RejectsInvalidWindowAndFrameBoundaries)
{
	Http2Options options;
	options.initialStreamWindow = Http2Options::kMaxWindowSize;
	EXPECT_NO_THROW(options.validate());
	options.initialStreamWindow = Http2Options::kMaxWindowSize + 1U;
	EXPECT_THROW(options.validate(), std::invalid_argument);
	options = {};
	options.maxFrameBytes = Http2Options::kMinFrameBytes - 1U;
	EXPECT_THROW(options.validate(), std::invalid_argument);
	options.maxFrameBytes = Http2Options::kMaxFrameBytes + 1U;
	EXPECT_THROW(options.validate(), std::invalid_argument);
	options.maxFrameBytes = Http2Options::kMaxFrameBytes;
	EXPECT_NO_THROW(options.validate());
}

TEST(Http2ContractTest, RejectsZeroOrIncompatibleMemoryAndTaskLimits)
{
	auto rejected = [](auto member, auto value)
	{
		Http2Options options;
		options.*member = value;
		EXPECT_FALSE(options.validationError().empty());
		EXPECT_THROW(options.validate(), std::invalid_argument);
	};
	rejected(&Http2Options::maxConcurrentStreams, 0U);
	rejected(&Http2Options::maxConcurrentStreams, Http2Options::kMaxWindowSize + 1U);
	rejected(&Http2Options::maxApplicationTasks, 0U);
	rejected(&Http2Options::maxServerApplicationTasks, 0U);
	rejected(&Http2Options::maxHeaderFields, RequestHeaders::kMaxHeaders + 1);
	rejected(&Http2Options::maxHeaderFields, 0U);
	rejected(&Http2Options::maxHeaderBytes, 0U);
	rejected(&Http2Options::maxBodyBytes, Http2Options::kDefaultConnectionBufferBytes + 1);
	rejected(&Http2Options::maxOutputBatchBytes, Http2Options::kDefaultConnectionBufferBytes + 1);
	rejected(&Http2Options::maxFilePrefetchBytes, Http2Options::kDefaultConnectionBufferBytes + 1);
	rejected(&Http2Options::maxResponseBytes, Http2Options::kDefaultClientConnectionReceiveBytes + 1);
	rejected(&Http2Options::maxConnectionsPerOrigin, 0U);
	rejected(&Http2Options::maxQueuedRequestsPerOrigin, 0U);
	rejected(&Http2Options::bodyTimeout, std::chrono::seconds {0});
	rejected(&Http2Options::writeTimeout, std::chrono::seconds {-1});
}
