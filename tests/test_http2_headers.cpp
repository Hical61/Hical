/**
 * @file test_http2_headers.cpp
 * @brief HTTP/2 头字段存储、预算和语义校验测试
 */

#include "core/http2/Http2Headers.h"
#include "core/HttpRequest.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <limits>
#include <string>
#include <vector>

using namespace hical;
using namespace hical::detail;

namespace
{
	using Headers = Http2Headers;
	using Kind = Headers::Kind;

	void requestPseudo(Headers& headers, std::string_view target = "/items?q=1")
	{
		ASSERT_TRUE(headers.append(":method", "GET").ok());
		ASSERT_TRUE(headers.append(":scheme", "https").ok());
		ASSERT_TRUE(headers.append(":authority", "example.test").ok());
		ASSERT_TRUE(headers.append(":path", target).ok());
	}

	void expectProtocolError(const NetworkError& error, int32_t streamId = 1)
	{
		EXPECT_EQ(error.code, ErrorCode::hHttp2StreamError);
		ASSERT_TRUE(error.http2.has_value());
		EXPECT_EQ(error.http2->streamId, streamId);
		EXPECT_EQ(error.http2->code, static_cast<uint32_t>(Http2Code::hProtocolError));
	}
} // namespace

TEST(Http2HeadersTest, SealsRequestViewsAndProvidesTheCanonicalHost)
{
	Headers headers(Kind::hRequest, 1);
	requestPseudo(headers);
	ASSERT_TRUE(headers.append("x-empty", "").ok());
	EXPECT_TRUE(headers.fields().empty());
	EXPECT_TRUE(headers.target().empty());
	ASSERT_TRUE(headers.seal().ok());
	EXPECT_TRUE(headers.sealed());
	EXPECT_EQ(headers.rawMethod(), "GET");
	EXPECT_EQ(headers.target(), "/items?q=1");
	EXPECT_EQ(headers.scheme(), "https");
	EXPECT_EQ(headers.authority(), "example.test");
	EXPECT_EQ(headers.find("HOST"), "example.test");
	EXPECT_EQ(headers.fields().size(), 2U);
	EXPECT_FALSE(headers.isConnect());
	EXPECT_FALSE(headers.contentLength().has_value());
	for (const auto& field : headers.fields())
	{
		EXPECT_FALSE(field.name.starts_with(':'));
	}
	const auto* before = headers.fields().data();
	ASSERT_TRUE(headers.seal().ok());
	EXPECT_EQ(headers.fields().data(), before);
	EXPECT_EQ(headers.append("x-after", "bad").code, ErrorCode::hInvalidConfiguration);
	EXPECT_EQ(headers.target(), "/items?q=1");
}

TEST(Http2HeadersTest, SourceReuseGrowthAndOwnerMovesDoNotInvalidateSealedViews)
{
	Headers original(Kind::hRequest, 3);
	std::string name = ":method";
	std::string value = "GET";
	ASSERT_TRUE(original.append(name, value).ok());
	name.assign(200, 'x');
	value.assign(200, 'y');
	ASSERT_TRUE(original.append(":scheme", "https").ok());
	ASSERT_TRUE(original.append(":authority", "a").ok());
	ASSERT_TRUE(original.append(":path", "/").ok());
	for (size_t index = 0; index < 40; ++index)
	{
		name = "x-" + std::to_string(index);
		value.assign(index + 1, 'z');
		ASSERT_TRUE(original.append(name, value).ok());
	}
	ASSERT_TRUE(original.seal().ok());
	const auto method = original.rawMethod();
	const auto target = original.target();
	const auto header = original.find("x-39");
	const auto* fieldData = original.fields().data();
	Headers moved(std::move(original));
	EXPECT_FALSE(original.sealed());
	EXPECT_EQ(moved.fields().data(), fieldData);
	EXPECT_EQ(method, "GET");
	EXPECT_EQ(target, "/");
	EXPECT_EQ(header, std::string(40, 'z'));
	EXPECT_EQ(moved.rawMethod().data(), method.data());

	Headers tiny(Kind::hTrailers, 7);
	ASSERT_TRUE(tiny.append("x", "y").ok());
	ASSERT_TRUE(tiny.seal().ok());
	auto smallValue = tiny.find("x");
	moved = std::move(tiny);
	EXPECT_EQ(smallValue, "y");
	EXPECT_EQ(smallValue.data(), moved.find("x").data());
}

TEST(Http2HeadersTest, CookieFragmentsMergeInOrderAndSetCookieRemainsSeparate)
{
	Headers request(Kind::hRequest, 1);
	requestPseudo(request);
	ASSERT_TRUE(request.append("cookie", "a=1").ok());
	ASSERT_TRUE(request.append("x-between", "kept").ok());
	ASSERT_TRUE(request.append("cookie", "b=2").ok());
	const auto originalCost = request.decodedBytes();
	ASSERT_TRUE(request.seal().ok());
	EXPECT_EQ(request.find("cookie"), "a=1; b=2");
	EXPECT_EQ(request.decodedFieldCount(), 7U);
	EXPECT_EQ(request.decodedBytes(), originalCost);
	EXPECT_EQ(std::ranges::count(request.fields(), std::string_view {"cookie"}, &Headers::Field::name), 1);

	Headers response(Kind::hResponse, 1);
	ASSERT_TRUE(response.append(":status", "200").ok());
	ASSERT_TRUE(response.append("set-cookie", "a=1; Path=/").ok());
	ASSERT_TRUE(response.append("set-cookie", "b=2; Path=/").ok());
	ASSERT_TRUE(response.seal().ok());
	ASSERT_EQ(response.fields().size(), 2U);
	EXPECT_EQ(response.fields()[0].value, "a=1; Path=/");
	EXPECT_EQ(response.fields()[1].value, "b=2; Path=/");
}

TEST(Http2HeadersTest, HeaderBudgetIncludesOriginalPseudoFieldsAndThirtyTwoByteOverhead)
{
	static_assert(Http2Options::kMaxHeaderFields == RequestHeaders::kMaxHeaders);
	Http2Options options;
	options.maxHeaderBytes = Http2Options::kHeaderFieldOverheadBytes + 2;
	Headers exact(Kind::hTrailers, 1, options);
	ASSERT_TRUE(exact.append("x", "y").ok());
	EXPECT_EQ(exact.decodedBytes(), options.maxHeaderBytes);
	ASSERT_TRUE(exact.seal().ok());
	Headers over(Kind::hTrailers, 3, options);
	auto error = over.append("x", "yy");
	EXPECT_EQ(error.code, ErrorCode::hResourceLimitExceeded);
	ASSERT_TRUE(error.http2.has_value());
	EXPECT_EQ(error.http2->code, static_cast<uint32_t>(Http2Code::hEnhanceYourCalm));
	EXPECT_EQ(over.decodedBytes(), 0U);
	EXPECT_EQ(over.decodedFieldCount(), 0U);
	EXPECT_FALSE(over.seal().ok());
}

TEST(Http2HeadersTest, SixtyFourFieldsArePreservedAndTheNextIsExplicitlyRejected)
{
	Headers headers(Kind::hTrailers, 1);
	for (size_t index = 0; index < RequestHeaders::kMaxHeaders; ++index)
	{
		ASSERT_TRUE(headers.append("x-field", std::to_string(index)).ok());
	}
	ASSERT_TRUE(headers.seal().ok());
	EXPECT_EQ(headers.fields().size(), RequestHeaders::kMaxHeaders);
	Headers overflow(Kind::hTrailers, 1);
	for (size_t index = 0; index < RequestHeaders::kMaxHeaders; ++index)
	{
		ASSERT_TRUE(overflow.append("x-field", "a").ok());
	}
	EXPECT_EQ(overflow.append("x-last", "b").code, ErrorCode::hResourceLimitExceeded);
	EXPECT_EQ(overflow.decodedFieldCount(), RequestHeaders::kMaxHeaders);
	EXPECT_TRUE(overflow.fields().empty());
}

TEST(Http2HeadersTest, PseudoHeadersMustMatchRoleOrderUniquenessAndCompleteness)
{
	Headers ordered(Kind::hRequest, 1);
	ASSERT_TRUE(ordered.append("x-regular", "value").ok());
	expectProtocolError(ordered.append(":method", "GET"));
	Headers duplicate(Kind::hRequest, 1);
	ASSERT_TRUE(duplicate.append(":method", "GET").ok());
	expectProtocolError(duplicate.append(":method", "GET"));
	Headers request(Kind::hRequest, 1);
	expectProtocolError(request.append(":status", "200"));
	Headers response(Kind::hResponse, 1);
	expectProtocolError(response.append(":path", "/"));
	Headers unknown(Kind::hRequest, 1);
	expectProtocolError(unknown.append(":unknown", "x"));
	Headers missing(Kind::hRequest, 1);
	ASSERT_TRUE(missing.append(":method", "GET").ok());
	expectProtocolError(missing.seal());
	Headers noStatus(Kind::hResponse, 1);
	expectProtocolError(noStatus.seal());
}

TEST(Http2HeadersTest, RejectsInvalidNamesValuesAndConnectionSpecificFields)
{
	const std::array<std::string_view, 9> names {"",
												 "Upper",
												 "bad name",
												 "bad:name",
												 "connection",
												 "keep-alive",
												 "proxy-connection",
												 "transfer-encoding",
												 "upgrade"};
	for (auto name : names)
	{
		Headers headers(Kind::hRequest, 1);
		requestPseudo(headers);
		expectProtocolError(headers.append(name, "x"));
	}
	const std::array<std::string, 7>
		values {"bad\r\nnext: x", std::string("a\0b", 3), "a\x01", "a\x7f", " leading", "trailing ", "\tleading"};
	for (const auto& value : values)
	{
		Headers headers(Kind::hTrailers, 1);
		expectProtocolError(headers.append("x-field", value));
	}
	Headers allowed(Kind::hTrailers, 1);
	ASSERT_TRUE(allowed.append("x-field", "middle\ttab").ok());
	ASSERT_TRUE(allowed.seal().ok());
}

TEST(Http2HeadersTest, TeAllowsOnlyTrailersInTheInitialRequest)
{
	Headers request(Kind::hRequest, 1);
	requestPseudo(request);
	ASSERT_TRUE(request.append("te", "Trailers").ok());
	ASSERT_TRUE(request.seal().ok());
	for (auto kind : {Kind::hRequest, Kind::hResponse, Kind::hTrailers})
	{
		Headers headers(kind, 1);
		expectProtocolError(headers.append("te", kind == Kind::hRequest ? "gzip" : "trailers"));
	}
}

TEST(Http2HeadersTest, ContentLengthRejectsDuplicatesNegativeListsAndOverflow)
{
	for (auto value : {"", "-1", "+1", "1, 1", "1x", "9223372036854775808", "18446744073709551616"})
	{
		Headers headers(Kind::hRequest, 1);
		requestPseudo(headers);
		expectProtocolError(headers.append("content-length", value));
	}
	Headers largest(Kind::hRequest, 1);
	requestPseudo(largest);
	ASSERT_TRUE(largest.append("content-length", "9223372036854775807").ok());
	ASSERT_TRUE(largest.seal().ok());
	EXPECT_EQ(largest.contentLength(), static_cast<uint64_t>(std::numeric_limits<int64_t>::max()));
	Headers duplicate(Kind::hRequest, 1);
	requestPseudo(duplicate);
	ASSERT_TRUE(duplicate.append("content-length", "0").ok());
	expectProtocolError(duplicate.append("content-length", "0"));
}

TEST(Http2HeadersTest, AuthorityAndHostUseSchemeDefaultsButKeepDifferentPortsDistinct)
{
	for (auto host : {"EXAMPLE.TEST", "example.test:443", "example.test:0443"})
	{
		Headers headers(Kind::hRequest, 1);
		requestPseudo(headers);
		ASSERT_TRUE(headers.append("host", host).ok());
		ASSERT_TRUE(headers.seal().ok());
		EXPECT_EQ(headers.find("host"), "example.test");
	}
	Headers mismatch(Kind::hRequest, 1);
	requestPseudo(mismatch);
	ASSERT_TRUE(mismatch.append("host", "example.test:444").ok());
	expectProtocolError(mismatch.seal());
	Headers fallback(Kind::hRequest, 1);
	ASSERT_TRUE(fallback.append(":method", "GET").ok());
	ASSERT_TRUE(fallback.append(":scheme", "https").ok());
	ASSERT_TRUE(fallback.append(":path", "/").ok());
	ASSERT_TRUE(fallback.append("host", "fallback.test").ok());
	ASSERT_TRUE(fallback.seal().ok());
	EXPECT_EQ(fallback.authority(), "fallback.test");
}

TEST(Http2HeadersTest, RejectsUserInfoEmptyAuthorityAndMalformedIpv6)
{
	for (auto authority : {"", "user@example.test", "[::1", "::1]", "[no-ipv6]", "[::1]:", "host:65536", "host/path"})
	{
		Headers headers(Kind::hRequest, 1);
		ASSERT_TRUE(headers.append(":method", "GET").ok());
		ASSERT_TRUE(headers.append(":scheme", "https").ok());
		auto error = headers.append(":authority", authority);
		if (error.ok())
		{
			ASSERT_TRUE(headers.append(":path", "/").ok());
			error = headers.seal();
		}
		expectProtocolError(error);
	}
	Headers ipv6(Kind::hRequest, 1);
	ASSERT_TRUE(ipv6.append(":method", "GET").ok());
	ASSERT_TRUE(ipv6.append(":scheme", "https").ok());
	ASSERT_TRUE(ipv6.append(":authority", "[::1]:443").ok());
	ASSERT_TRUE(ipv6.append(":path", "/").ok());
	ASSERT_TRUE(ipv6.append("host", "[0:0:0:0:0:0:0:1]").ok());
	ASSERT_TRUE(ipv6.seal().ok());
}

TEST(Http2HeadersTest, TraditionalConnectIsValidAndRemainsDistinctFromOrdinaryPaths)
{
	Headers connect(Kind::hRequest, 1);
	ASSERT_TRUE(connect.append(":method", "CONNECT").ok());
	ASSERT_TRUE(connect.append(":authority", "example.test:443").ok());
	ASSERT_TRUE(connect.seal().ok());
	EXPECT_TRUE(connect.isConnect());
	EXPECT_EQ(connect.target(), "example.test:443");
	EXPECT_TRUE(connect.scheme().empty());
	EXPECT_EQ(connect.find("host"), "example.test:443");
	Headers invalid(Kind::hRequest, 1);
	ASSERT_TRUE(invalid.append(":method", "CONNECT").ok());
	ASSERT_TRUE(invalid.append(":authority", "example.test:443").ok());
	ASSERT_TRUE(invalid.append(":scheme", "https").ok());
	expectProtocolError(invalid.seal());
	Headers options(Kind::hRequest, 1);
	ASSERT_TRUE(options.append(":method", "OPTIONS").ok());
	ASSERT_TRUE(options.append(":scheme", "https").ok());
	ASSERT_TRUE(options.append(":authority", "example.test").ok());
	ASSERT_TRUE(options.append(":path", "*").ok());
	ASSERT_TRUE(options.seal().ok());
}

TEST(Http2HeadersTest, TrailersStayIndependentAndRejectRoutingFramingAndAuthenticationFields)
{
	for (auto name : {":status",
					  "content-length",
					  "host",
					  "authorization",
					  "content-type",
					  "content-encoding",
					  "cookie",
					  "set-cookie",
					  "cache-control",
					  "trailer"})
	{
		Headers trailers(Kind::hTrailers, 1);
		expectProtocolError(trailers.append(name, "x"));
	}
	Headers initial(Kind::hResponse, 1);
	ASSERT_TRUE(initial.append(":status", "200").ok());
	ASSERT_TRUE(initial.append("x-checksum", "initial").ok());
	ASSERT_TRUE(initial.seal().ok());
	Headers trailers(Kind::hTrailers, 1);
	ASSERT_TRUE(trailers.append("x-checksum", "final").ok());
	ASSERT_TRUE(trailers.append("etag", "abc").ok());
	ASSERT_TRUE(trailers.seal().ok());
	EXPECT_EQ(initial.find("x-checksum"), "initial");
	EXPECT_EQ(trailers.find("x-checksum"), "final");
	EXPECT_EQ(trailers.status(), 0);
}

TEST(Http2HeadersTest, ResponsesRequireThreeDigitStatusAndRetainInformationalIdentity)
{
	for (auto value : {"", "20", "2000", "abc", "099", "101"})
	{
		Headers headers(Kind::hResponse, 1);
		auto error = headers.append(":status", value);
		if (error.ok())
		{
			error = headers.seal();
		}
		expectProtocolError(error);
	}
	Headers informational(Kind::hResponse, 1);
	ASSERT_TRUE(informational.append(":status", "103").ok());
	ASSERT_TRUE(informational.seal().ok());
	EXPECT_EQ(informational.status(), 103);
	EXPECT_TRUE(informational.fields().empty());
}
