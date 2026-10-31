/**
 * @file test_http2_request.cpp
 * @brief HTTP/2 请求头存储、字段映射和逐流缓存测试
 */

#include "core/http2/Http2ServerSession.h"
#include <boost/asio/io_context.hpp>
#include <gtest/gtest.h>
#include <array>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

using namespace hical;
using namespace hical::detail;

namespace
{
	class Http2RequestTest : public testing::Test
	{
	protected:
		boost::asio::io_context io;
		Http2Options options;
		Http2ServiceBudget service {Http2Options::kDefaultMaxServerApplicationTasks};
		Http2Budget budget {Http2Budget::Role::hServer, options, &service};
		InetAddress peer {"127.0.0.7", 43210};

		std::shared_ptr<Http2Stream> create(int32_t id = 1)
		{
			Http2Stream::Config config;
			config.streamId = id;
			config.options = options;
			return Http2Stream::create(io.get_executor(), config, budget);
		}

		void pseudo(Http2Stream& stream, std::string_view target = "/items?q=one", std::string_view method = "GET")
		{
			auto& headers = stream.initialHeaders();
			ASSERT_FALSE(headers.append(":method", method));
			ASSERT_FALSE(headers.append(":scheme", "https"));
			ASSERT_FALSE(headers.append(":authority", "example.test:8443"));
			ASSERT_FALSE(headers.append(":path", target));
		}
	};
} // namespace

TEST_F(Http2RequestTest, MapsHttp2SeparatelyFromHttp1VersionAndPreservesHeaderViews)
{
	auto stream = create();
	ASSERT_TRUE(stream);
	pseudo(*stream, "/items?q=one", "POST");
	ASSERT_FALSE(stream->initialHeaders().append("x-request-id", "trace"));
	ASSERT_FALSE(stream->initialHeaders().append("expect", "100-continue"));
	ASSERT_FALSE(stream->initialHeaders().seal());
	ASSERT_FALSE(Http2ServerSession::mapRequest(*stream, peer));
	const auto& request = stream->request();
	EXPECT_EQ(request.protocol(), HttpProtocol::hHttp2);
	EXPECT_EQ(request.native().httpVersionMajor, 1);
	EXPECT_EQ(request.native().httpVersionMinor, 1);
	EXPECT_EQ(request.method(), HttpMethod::hPost);
	EXPECT_EQ(request.path(), "/items");
	EXPECT_EQ(request.target(), "/items?q=one");
	EXPECT_EQ(request.header("HOST"), "example.test:8443");
	EXPECT_EQ(request.header("X-Request-Id"), "trace");
	EXPECT_EQ(request.target().data(), stream->initialHeaders().target().data());
	EXPECT_EQ(request.header("host").data(), stream->initialHeaders().find("host").data());
	EXPECT_EQ(request.header("x-request-id").data(), stream->initialHeaders().find("x-request-id").data());
	EXPECT_EQ(request.peerAddr().toIp(), "127.0.0.7");
	EXPECT_EQ(request.peerAddr().port(), 43210);
	EXPECT_TRUE(request.body().empty());
	EXPECT_TRUE(request.native().expectContinue);
	EXPECT_TRUE(request.native().keepAlive);
	EXPECT_FALSE(request.native().isUpgrade());
	for (const auto& field : request.native().headers)
	{
		EXPECT_FALSE(field.name.starts_with(':'));
	}
}

TEST_F(Http2RequestTest, ShortAndLongTargetsSurviveDecodeScratchReuse)
{
	auto first = create(1);
	auto second = create(3);
	ASSERT_TRUE(first);
	ASSERT_TRUE(second);
	std::string scratch = "/";
	pseudo(*first, scratch);
	ASSERT_FALSE(first->initialHeaders().append("x-small", "tiny"));
	ASSERT_FALSE(first->initialHeaders().seal());
	ASSERT_FALSE(Http2ServerSession::mapRequest(*first, peer));
	scratch = "/" + std::string(4096, 'a') + "?encoded=a%20b";
	const auto expected = scratch;
	pseudo(*second, scratch);
	for (size_t index = 0; index < 24; ++index)
	{
		ASSERT_FALSE(second->initialHeaders().append("x-" + std::to_string(index), std::string(24, 'z')));
	}
	ASSERT_FALSE(second->initialHeaders().seal());
	ASSERT_FALSE(Http2ServerSession::mapRequest(*second, peer));
	scratch.assign(8192, 'x');
	EXPECT_EQ(first->request().target(), "/");
	EXPECT_EQ(first->request().header("x-small"), "tiny");
	EXPECT_EQ(second->request().target(), expected);
	EXPECT_EQ(second->request().queryParam("encoded"), "a b");
	EXPECT_EQ(second->request().target().data(), second->initialHeaders().target().data());
	first.reset();
	EXPECT_EQ(second->request().target(), expected);
}

TEST_F(Http2RequestTest, AuthorityNormalizationAndHostFallbackReachRequestApi)
{
	auto first = create(1);
	auto second = create(3);
	ASSERT_TRUE(first);
	ASSERT_TRUE(second);
	auto& initial = first->initialHeaders();
	ASSERT_FALSE(initial.append(":method", "GET"));
	ASSERT_FALSE(initial.append(":scheme", "https"));
	ASSERT_FALSE(initial.append(":authority", "Example.Test"));
	ASSERT_FALSE(initial.append(":path", "/"));
	ASSERT_FALSE(initial.append("host", "example.test:443"));
	ASSERT_FALSE(initial.seal());
	ASSERT_FALSE(Http2ServerSession::mapRequest(*first, peer));
	EXPECT_EQ(first->request().header("host"), "Example.Test");
	auto& fallback = second->initialHeaders();
	ASSERT_FALSE(fallback.append(":method", "GET"));
	ASSERT_FALSE(fallback.append(":scheme", "https"));
	ASSERT_FALSE(fallback.append(":path", "/host-only"));
	ASSERT_FALSE(fallback.append("host", "fallback.test"));
	ASSERT_FALSE(fallback.seal());
	ASSERT_FALSE(Http2ServerSession::mapRequest(*second, peer));
	EXPECT_EQ(second->request().header("HOST"), "fallback.test");
}

TEST_F(Http2RequestTest, QueryCookieParametersAndAttributesRemainPerStream)
{
	auto first = create(1);
	auto second = create(3);
	ASSERT_TRUE(first);
	ASSERT_TRUE(second);
	pseudo(*first, "/one?name=a%20b&tag=x&tag=y");
	pseudo(*second, "/two?name=second&tag=z");
	ASSERT_FALSE(first->initialHeaders().append("cookie", "session=first"));
	ASSERT_FALSE(first->initialHeaders().append("cookie", "theme=dark"));
	ASSERT_FALSE(second->initialHeaders().append("cookie", "session=second"));
	ASSERT_FALSE(first->initialHeaders().seal());
	ASSERT_FALSE(second->initialHeaders().seal());
	ASSERT_FALSE(Http2ServerSession::mapRequest(*first, peer));
	ASSERT_FALSE(Http2ServerSession::mapRequest(*second, InetAddress("::1", 43211)));
	first->request().setParam("id", "first");
	first->request().setAttribute("trace", std::string("first"));
	EXPECT_EQ(first->request().queryParam("name"), "a b");
	EXPECT_EQ(second->request().queryParam("name"), "second");
	EXPECT_EQ(first->request().queryParams().count("tag"), 2U);
	EXPECT_EQ(second->request().queryParams().count("tag"), 1U);
	EXPECT_EQ(first->request().cookie("session"), "first");
	EXPECT_EQ(first->request().cookie("theme"), "dark");
	EXPECT_EQ(second->request().cookie("session"), "second");
	EXPECT_FALSE(second->request().hasCookie("theme"));
	EXPECT_FALSE(second->request().hasParam("id"));
	EXPECT_FALSE(second->request().getAttribute("trace"));
	EXPECT_EQ(first->request().peerAddr().port(), 43210);
	EXPECT_EQ(second->request().peerAddr().port(), 43211);
	EXPECT_TRUE(second->request().peerAddr().isIpV6());
	EXPECT_EQ(second->request().peerAddr().toIp(), "::1");
}

TEST_F(Http2RequestTest, PreReadCachesAreFreshWhenMappingCreatesTheRequest)
{
	auto stream = create();
	ASSERT_TRUE(stream);
	EXPECT_FALSE(stream->request().queryParam("q"));
	EXPECT_TRUE(stream->request().cookies().empty());
	EXPECT_TRUE(stream->request().jsonBody().is_null());
	pseudo(*stream);
	ASSERT_FALSE(stream->initialHeaders().append("cookie", "token=present"));
	ASSERT_FALSE(stream->initialHeaders().seal());
	ASSERT_FALSE(Http2ServerSession::mapRequest(*stream, peer));
	EXPECT_EQ(stream->request().queryParam("q"), "one");
	EXPECT_EQ(stream->request().cookie("token"), "present");
}

TEST_F(Http2RequestTest, BodyIsUnpublishedEvenWhenAlreadyReceivedBeforeMapping)
{
	auto stream = create();
	ASSERT_TRUE(stream);
	pseudo(*stream, "/json", "POST");
	ASSERT_FALSE(stream->initialHeaders().append("content-type", "application/json"));
	ASSERT_FALSE(stream->initialHeaders().seal());
	ASSERT_FALSE(stream->appendBody("{\"name\":\"one\"}"));
	ASSERT_FALSE(stream->completeBody());
	ASSERT_FALSE(Http2ServerSession::mapRequest(*stream, peer));
	EXPECT_TRUE(stream->request().body().empty());
	EXPECT_EQ(stream->body(), "{\"name\":\"one\"}");
	ASSERT_FALSE(stream->publishBody());
	EXPECT_EQ(stream->request().jsonBody().as_object().at("name").as_string(), "one");
}

TEST_F(Http2RequestTest, PublishedJsonAndFormCachesAreIndependentBetweenStreams)
{
	auto first = create(1);
	auto second = create(3);
	ASSERT_TRUE(first);
	ASSERT_TRUE(second);
	pseudo(*first, "/json", "POST");
	pseudo(*second, "/form", "POST");
	ASSERT_FALSE(first->initialHeaders().append("content-type", "application/json"));
	ASSERT_FALSE(second->initialHeaders().append("content-type", "application/x-www-form-urlencoded"));
	ASSERT_FALSE(first->initialHeaders().seal());
	ASSERT_FALSE(second->initialHeaders().seal());
	ASSERT_FALSE(Http2ServerSession::mapRequest(*first, peer));
	ASSERT_FALSE(Http2ServerSession::mapRequest(*second, peer));
	ASSERT_FALSE(first->appendBody("{\"id\":1}"));
	ASSERT_FALSE(second->appendBody("id=2&name=second"));
	ASSERT_FALSE(first->completeBody());
	ASSERT_FALSE(second->completeBody());
	ASSERT_FALSE(first->publishBody());
	ASSERT_FALSE(second->publishBody());
	EXPECT_EQ(first->request().jsonBody().as_object().at("id").as_int64(), 1);
	EXPECT_EQ(second->request().formParam("id"), "2");
	EXPECT_EQ(second->request().formParam("name"), "second");
	EXPECT_TRUE(second->request().jsonBody().is_null());
	EXPECT_FALSE(first->request().formParam("name"));
}

TEST_F(Http2RequestTest, MappingAfterBodyPublicationDoesNotEraseThePayload)
{
	auto stream = create();
	ASSERT_TRUE(stream);
	pseudo(*stream, "/body", "POST");
	ASSERT_FALSE(stream->initialHeaders().seal());
	ASSERT_FALSE(stream->appendBody("body"));
	ASSERT_FALSE(stream->completeBody());
	ASSERT_FALSE(stream->publishBody());
	EXPECT_EQ(Http2ServerSession::mapRequest(*stream, peer).code, ErrorCode::hOperationInProgress);
	EXPECT_EQ(stream->request().body(), "body");
}

TEST_F(Http2RequestTest, KnownMethodsMapExactlyAndExtensionsKeepTheirRawIdentity)
{
	const std::array<std::pair<std::string_view, HttpMethod>, 9> methods {{{"GET", HttpMethod::hGet},
																		   {"POST", HttpMethod::hPost},
																		   {"PUT", HttpMethod::hPut},
																		   {"DELETE", HttpMethod::hDelete},
																		   {"PATCH", HttpMethod::hPatch},
																		   {"HEAD", HttpMethod::hHead},
																		   {"OPTIONS", HttpMethod::hOptions},
																		   {"PROPFIND", HttpMethod::hUnknown},
																		   {"get", HttpMethod::hUnknown}}};
	int32_t id = 1;
	for (const auto& [raw, expected] : methods)
	{
		auto stream = create(id);
		ASSERT_TRUE(stream);
		const auto target = raw == "OPTIONS" ? "*" : "/method";
		pseudo(*stream, target, raw);
		ASSERT_FALSE(stream->initialHeaders().seal());
		ASSERT_FALSE(Http2ServerSession::mapRequest(*stream, peer));
		EXPECT_EQ(stream->request().method(), expected);
		EXPECT_EQ(stream->initialHeaders().rawMethod(), raw);
		EXPECT_EQ(stream->request().target(), target);
		id += 2;
	}
}

TEST_F(Http2RequestTest, OrdinaryConnectMapsAuthorityWithoutInventingAPathOrMethod)
{
	auto stream = create();
	ASSERT_TRUE(stream);
	auto& headers = stream->initialHeaders();
	ASSERT_FALSE(headers.append(":method", "CONNECT"));
	ASSERT_FALSE(headers.append(":authority", "tunnel.test:443"));
	ASSERT_FALSE(headers.seal());
	ASSERT_FALSE(Http2ServerSession::mapRequest(*stream, peer));
	EXPECT_TRUE(headers.isConnect());
	EXPECT_EQ(headers.rawMethod(), "CONNECT");
	EXPECT_EQ(stream->request().method(), HttpMethod::hUnknown);
	EXPECT_EQ(stream->request().target(), "tunnel.test:443");
	EXPECT_EQ(stream->request().header("host"), "tunnel.test:443");
	EXPECT_TRUE(stream->request().body().empty());
}

TEST_F(Http2RequestTest, RejectsUnsealedClosedResponseOrDuplicateMappingWithoutMutation)
{
	auto stream = create();
	ASSERT_TRUE(stream);
	pseudo(*stream);
	EXPECT_EQ(Http2ServerSession::mapRequest(*stream, peer).code, ErrorCode::hInvalidConfiguration);
	EXPECT_TRUE(stream->request().target().empty());
	ASSERT_FALSE(stream->initialHeaders().seal());
	ASSERT_FALSE(Http2ServerSession::mapRequest(*stream, peer));
	stream->request().setAttribute("keep", true);
	EXPECT_EQ(Http2ServerSession::mapRequest(*stream, peer).code, ErrorCode::hOperationInProgress);
	EXPECT_TRUE(stream->request().getAttribute<bool>("keep").value());
	stream->reset();
	EXPECT_EQ(Http2ServerSession::mapRequest(*stream, peer).code, ErrorCode::hHttp2StreamError);
	auto response = create(3);
	ASSERT_TRUE(response);
	response->initialHeaders() = Http2Headers(Http2Headers::Kind::hResponse, 3);
	ASSERT_FALSE(response->initialHeaders().append(":status", "200"));
	ASSERT_FALSE(response->initialHeaders().seal());
	EXPECT_EQ(Http2ServerSession::mapRequest(*response, peer).code, ErrorCode::hInvalidConfiguration);
	EXPECT_TRUE(response->request().target().empty());
}
