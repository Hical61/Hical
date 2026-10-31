/**
 * @file test_http2_contract.cpp
 * @brief HTTP 协议、资源预算与尾字段的公开契约
 */

#include "core/Http2Options.h"
#include "core/HttpRequest.h"
#include "core/HttpResponse.h"
#ifdef HICAL_HAS_HTTP_CLIENT
	#include "asio/HttpClient.h"
	#include <boost/asio/co_spawn.hpp>
	#include <boost/asio/io_context.hpp>
	#include <boost/asio/use_future.hpp>
#endif
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

#ifdef HICAL_HAS_HTTP_CLIENT
TEST(Http2ContractTest, ClientDefaultsToHttp1AndConservativeRetryDisposition)
{
	HttpClientOptions options;
	EXPECT_EQ(options.protocolMode, HttpProtocolMode::hHttp1Only);
	HttpClientResult result;
	EXPECT_EQ(result.protocol, HttpProtocol::hHttp1);
	EXPECT_EQ(result.requestDisposition, HttpRequestDisposition::hPossiblyProcessed);
	result.headers.set("x-checksum", "initial");
	result.trailers.set("x-checksum", "final");
	EXPECT_EQ(result.headers.find("x-checksum"), "initial");
	EXPECT_EQ(result.trailers.find("x-checksum"), "final");
}

	#ifndef HICAL_HAS_HTTP2
TEST(Http2ContractTest, DisabledHttp2ClientModesFailBeforeConnecting)
{
	for (auto mode : {HttpProtocolMode::hNegotiateHttp2, HttpProtocolMode::hHttp2Only})
	{
		boost::asio::io_context io;
		HttpClientOptions options;
		options.protocolMode = mode;
		HttpClient client(io, "invalid.invalid", 443, options);
		auto resultFuture = boost::asio::co_spawn(io, client.get("/"), boost::asio::use_future);
		io.run();
		auto result = resultFuture.get();
		EXPECT_EQ(result.error.code, ErrorCode::hProtocolUnavailable);
		EXPECT_EQ(result.requestDisposition, HttpRequestDisposition::hNotSent);
		EXPECT_EQ(result.status, 0);
		EXPECT_FALSE(client.isAlive());
	}
}
	#endif

TEST(Http2ContractTest, InvalidClientProtocolModeIsAConfigurationError)
{
	boost::asio::io_context io;
	HttpClientOptions options;
	options.protocolMode = static_cast<HttpProtocolMode>(255);
	HttpClient client(io, "invalid.invalid", 443, options);
	auto resultFuture = boost::asio::co_spawn(io, client.get("/"), boost::asio::use_future);
	io.run();
	auto result = resultFuture.get();
	EXPECT_EQ(result.error.code, ErrorCode::hInvalidConfiguration);
	EXPECT_EQ(result.requestDisposition, HttpRequestDisposition::hNotSent);
	EXPECT_FALSE(client.isAlive());
}
#endif
