/**
 * @file example.cpp
 * @brief 安装包的功能宏、公开接口与静态链接消费检查
 */

#include <hical/core/Http2Options.h>
#include <hical/core/HttpProtocol.h>
#include <hical/core/HttpServer.h>
#include <hical/core/Log.h>

#if HICAL_TEST_EXPECT_HTTP_CLIENT
	#ifndef HICAL_HAS_HTTP_CLIENT
		#error "安装包未导出 HICAL_HAS_HTTP_CLIENT"
	#endif
	#include <hical/asio/HttpClient.h>
	#include <hical/asio/HttpClientPool.h>
#else
	#ifdef HICAL_HAS_HTTP_CLIENT
		#error "客户端关闭的安装包不应导出 HICAL_HAS_HTTP_CLIENT"
	#endif
	#if __has_include(<hical/asio/HttpClient.h>) || __has_include(<hical/asio/HttpClientPool.h>)
		#error "客户端关闭的安装包不应安装客户端头文件"
	#endif
#endif

#if HICAL_TEST_EXPECT_HTTP2
	#include <nghttp2/nghttp2.h>
	#ifndef HICAL_HAS_HTTP2
		#error "安装包未导出 HICAL_HAS_HTTP2"
	#endif
#else
	#ifdef HICAL_HAS_HTTP2
		#error "HTTP/2 关闭的安装包不应导出 HICAL_HAS_HTTP2"
	#endif
#endif

int main()
{
	hical::Http2Options http2;
	http2.validate();
	hical::HttpServer server;
	server.router().get("/test",
						[](const hical::HttpRequest&) -> hical::HttpResponse
						{
							return hical::HttpResponse::ok("test");
						});

	hical::HttpRequest request;
	if (request.protocol() != hical::HttpProtocol::hHttp1 || !request.trailers().empty())
	{
		return 1;
	}
	auto response = hical::HttpResponse::ok("test");
	response.setTrailer("x-package-test", "ok");
	if (response.trailers().find("x-package-test") != "ok")
	{
		return 1;
	}

#if HICAL_TEST_EXPECT_HTTP2
	constexpr int kMinimumNghttp2Version = 0x014600;
	constexpr int kNextNghttp2MajorVersion = 0x020000;
	const auto* dependency = nghttp2_version(0);
	if (!dependency || dependency->version_num < kMinimumNghttp2Version
		|| dependency->version_num >= kNextNghttp2MajorVersion || dependency->version_num != NGHTTP2_VERSION_NUM)
	{
		return 1;
	}
	server.enableHttp2(http2);
	hical::SslContext tls;
	tls.setServerHttpProtocolMode(hical::HttpProtocolMode::hNegotiateHttp2);
	tls.freezeServerHttpProtocolConfig();
#endif

#if HICAL_TEST_EXPECT_HTTP_CLIENT
	boost::asio::io_context io;
	hical::HttpClientOptions options;
	if (options.protocolMode != hical::HttpProtocolMode::hHttp1Only)
	{
		return 1;
	}
	hical::HttpClient client(io, "localhost", 0, options);
	client.close();
	{
		hical::HttpClientPool pool(io);
		auto pooledClient = pool.acquire("localhost", 0, options);
		pooledClient->close();
		pool.release(std::move(pooledClient));
		pool.shutdown();
	}
	io.run();
#endif

	HICAL_LOG_INFO("Hical 安装包消费检查通过：HTTP_CLIENT={}, HTTP2={}",
				   HICAL_TEST_EXPECT_HTTP_CLIENT,
				   HICAL_TEST_EXPECT_HTTP2);
	return 0;
}
