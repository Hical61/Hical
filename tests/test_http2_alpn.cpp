/**
 * @file test_http2_alpn.cpp
 * @brief 通过 TLS 握手测试 HTTP ALPN
 */

#include "core/SslContext.h"
#ifdef HICAL_HAS_HTTP2
	#include <gtest/gtest.h>
	#include <openssl/err.h>
	#include <openssl/ssl.h>
	#include <chrono>
	#include <cstdlib>
	#include <fstream>
	#include <memory>
	#include <stdexcept>
	#include <string_view>
	#include <utility>

using namespace hical;

namespace
{
	constexpr char kCertificate[] = "test_server.crt";
	constexpr char kPrivateKey[] = "test_server.key";
	constexpr unsigned char kHttp1[] = {8, 'h', 't', 't', 'p', '/', '1', '.', '1'};
	constexpr unsigned char kHttp2[] = {2, 'h', '2'};
	constexpr unsigned char kHttp1First[] = {8, 'h', 't', 't', 'p', '/', '1', '.', '1', 2, 'h', '2'};
	constexpr unsigned char kUnknown[] = {2, 'h', '3'};
	using SslHandle = std::unique_ptr<SSL, decltype(&SSL_free)>;
	using ContextHandle = std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)>;

	// BIO 代替 socket 传字节，两端仍执行完整的 TLS 握手。
	class TlsPair
	{
	public:
		TlsPair(SSL_CTX* serverContext, SSL_CTX* clientContext)
			: server(SSL_new(serverContext), SSL_free), client(SSL_new(clientContext), SSL_free)
		{
			BIO* serverBio = nullptr;
			BIO* clientBio = nullptr;
			const int created = BIO_new_bio_pair(&serverBio, 0, &clientBio, 0);
			std::unique_ptr<BIO, decltype(&BIO_free)> serverOwner(serverBio, BIO_free);
			std::unique_ptr<BIO, decltype(&BIO_free)> clientOwner(clientBio, BIO_free);
			if (!server || !client || created != 1)
			{
				throw std::runtime_error("无法创建测试 TLS 对端");
			}
			SSL_set_bio(server.get(), serverBio, serverBio);
			SSL_set_bio(client.get(), clientBio, clientBio);
			serverOwner.release();
			clientOwner.release();
			SSL_set_accept_state(server.get());
			SSL_set_connect_state(client.get());
			SSL_set_verify(client.get(), SSL_VERIFY_NONE, nullptr);
		}

		bool handshake()
		{
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds {10};
			while (std::chrono::steady_clock::now() < deadline)
			{
				if (!step(client.get()) || !step(server.get()))
				{
					return false;
				}
				if (SSL_is_init_finished(client.get()) && SSL_is_init_finished(server.get()))
				{
					return true;
				}
			}
			return false;
		}

		SslHandle server;
		SslHandle client;

	private:
		static bool step(SSL* ssl)
		{
			ERR_clear_error();
			const int result = SSL_do_handshake(ssl);
			if (result == 1)
			{
				return true;
			}
			const int error = SSL_get_error(ssl, result);
			return error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE;
		}
	};

	std::string_view selectedProtocol(SSL* ssl)
	{
		const unsigned char* bytes = nullptr;
		unsigned int size = 0;
		SSL_get0_alpn_selected(ssl, &bytes, &size);
		return size == 0 ? std::string_view {} : std::string_view(reinterpret_cast<const char*>(bytes), size);
	}

	// 原生服务端选择客户端列出的首项，用真实握手检查 Hical 客户端的协议顺序。
	int selectFirstOffered(SSL*,
						   const unsigned char** out,
						   unsigned char* outSize,
						   const unsigned char* offered,
						   unsigned int size,
						   void*) noexcept
	{
		if (size < 2 || offered[0] == 0 || offered[0] >= size)
		{
			return SSL_TLSEXT_ERR_ALERT_FATAL;
		}
		*out = offered + 1;
		*outSize = offered[0];
		return SSL_TLSEXT_ERR_OK;
	}

	class Http2AlpnTest : public testing::Test
	{
	protected:
		void SetUp() override
		{
			if (!std::ifstream(kCertificate).good() || !std::ifstream(kPrivateKey).good())
			{
				if (std::getenv("HICAL_REQUIRE_SSL_CERTS") != nullptr)
				{
					FAIL() << "要求 SSL 证书，但 test_server.crt/key 缺失";
				}
				GTEST_SKIP() << "请先在测试工作目录生成 SSL 证书";
			}
		}

		static void loadServerCertificate(SslContext& context)
		{
			context.loadCertificate(kCertificate);
			context.loadPrivateKey(kPrivateKey);
		}
	};
} // namespace

TEST_F(Http2AlpnTest, ServerPrefersHttp2AndFreezesConfiguration)
{
	SslContext server(boost::asio::ssl::context::tls_server);
	server.setServerHttpProtocolMode(HttpProtocolMode::hNegotiateHttp2);
	loadServerCertificate(server);
	server.freezeServerHttpProtocolConfig();
	EXPECT_TRUE(server.serverHttpProtocolConfigFrozen());
	EXPECT_THROW(server.setServerHttpProtocolMode(HttpProtocolMode::hHttp1Only), std::logic_error);
	EXPECT_NO_THROW(server.freezeServerHttpProtocolConfig());
	ContextHandle client(SSL_CTX_new(TLS_client_method()), SSL_CTX_free);
	ASSERT_NE(client, nullptr);
	TlsPair pair(server.native().native_handle(), client.get());
	ASSERT_EQ(SSL_set_alpn_protos(pair.client.get(), kHttp1First, sizeof(kHttp1First)), 0);
	ASSERT_TRUE(pair.handshake());
	EXPECT_EQ(selectedProtocol(pair.client.get()), "h2");
	HttpProtocol protocol {};
	EXPECT_TRUE(
		SslContext::negotiatedHttpProtocol(pair.server.get(), HttpProtocolMode::hNegotiateHttp2, protocol).ok());
	EXPECT_EQ(protocol, HttpProtocol::hHttp2);
}

TEST_F(Http2AlpnTest, ServerAcceptsHttp1AndNoAlpnAndRejectsDisjointList)
{
	for (int policy = 0; policy < 3; ++policy)
	{
		SslContext server(boost::asio::ssl::context::tls_server);
		loadServerCertificate(server);
		server.setServerHttpProtocolMode(HttpProtocolMode::hNegotiateHttp2);
		server.freezeServerHttpProtocolConfig();
		EXPECT_GE(SSL_CTX_get_min_proto_version(server.native().native_handle()), TLS1_2_VERSION);
		ContextHandle client(SSL_CTX_new(TLS_client_method()), SSL_CTX_free);
		ASSERT_NE(client, nullptr);
		TlsPair pair(server.native().native_handle(), client.get());
		if (policy == 0)
		{
			ASSERT_EQ(SSL_set_alpn_protos(pair.client.get(), kHttp1, sizeof(kHttp1)), 0);
		}
		else if (policy == 2)
		{
			ASSERT_EQ(SSL_set_alpn_protos(pair.client.get(), kUnknown, sizeof(kUnknown)), 0);
		}
		ASSERT_EQ(pair.handshake(), policy != 2);
		if (policy != 2)
		{
			HttpProtocol protocol = HttpProtocol::hHttp2;
			EXPECT_TRUE(
				SslContext::negotiatedHttpProtocol(pair.server.get(), HttpProtocolMode::hNegotiateHttp2, protocol)
					.ok());
			EXPECT_EQ(protocol, HttpProtocol::hHttp1);
		}
	}
}

TEST_F(Http2AlpnTest, DefaultServerOnlySelectsHttp1)
{
	SslContext server(boost::asio::ssl::context::tls_server);
	EXPECT_EQ(server.serverHttpProtocolMode(), HttpProtocolMode::hHttp1Only);
	loadServerCertificate(server);
	server.freezeServerHttpProtocolConfig();
	ContextHandle client(SSL_CTX_new(TLS_client_method()), SSL_CTX_free);
	ASSERT_NE(client, nullptr);
	TlsPair pair(server.native().native_handle(), client.get());
	ASSERT_EQ(SSL_set_alpn_protos(pair.client.get(), kHttp1First, sizeof(kHttp1First)), 0);
	ASSERT_TRUE(pair.handshake());
	EXPECT_EQ(selectedProtocol(pair.client.get()), "http/1.1");
}

TEST_F(Http2AlpnTest, PerSslClientListsDoNotMutateSharedContext)
{
	SslContext server(boost::asio::ssl::context::tls_server);
	loadServerCertificate(server);
	SSL_CTX_set_alpn_select_cb(server.native().native_handle(), selectFirstOffered, nullptr);
	ContextHandle sharedClient(SSL_CTX_new(TLS_client_method()), SSL_CTX_free);
	ASSERT_NE(sharedClient, nullptr);
	TlsPair automatic(server.native().native_handle(), sharedClient.get());
	TlsPair http1(server.native().native_handle(), sharedClient.get());
	ASSERT_TRUE(
		SslContext::configureClientHttpProtocols(automatic.client.get(), HttpProtocolMode::hNegotiateHttp2).ok());
	ASSERT_TRUE(SslContext::configureClientHttpProtocols(http1.client.get(), HttpProtocolMode::hHttp1Only).ok());
	TlsPair unchanged(server.native().native_handle(), sharedClient.get());
	ASSERT_TRUE(http1.handshake());
	ASSERT_TRUE(automatic.handshake());
	ASSERT_TRUE(unchanged.handshake());
	EXPECT_EQ(selectedProtocol(automatic.client.get()), "h2");
	EXPECT_EQ(selectedProtocol(http1.client.get()), "http/1.1");
	EXPECT_TRUE(selectedProtocol(unchanged.client.get()).empty());
	EXPECT_EQ(SSL_CTX_get_min_proto_version(sharedClient.get()), 0);
}

TEST_F(Http2AlpnTest, AutomaticClientFallsBackAndRequiredHttp2FailsWithoutAlpn)
{
	for (auto mode : {HttpProtocolMode::hNegotiateHttp2, HttpProtocolMode::hHttp2Only})
	{
		SslContext server(boost::asio::ssl::context::tls_server);
		loadServerCertificate(server);
		ContextHandle client(SSL_CTX_new(TLS_client_method()), SSL_CTX_free);
		ASSERT_NE(client, nullptr);
		TlsPair pair(server.native().native_handle(), client.get());
		ASSERT_TRUE(SslContext::configureClientHttpProtocols(pair.client.get(), mode).ok());
		ASSERT_TRUE(pair.handshake());
		HttpProtocol protocol = HttpProtocol::hHttp2;
		auto error = SslContext::negotiatedHttpProtocol(pair.client.get(), mode, protocol);
		EXPECT_EQ(error.code,
				  mode == HttpProtocolMode::hHttp2Only ? ErrorCode::hProtocolNegotiationFailed : ErrorCode::hNoError);
		EXPECT_EQ(protocol, HttpProtocol::hHttp1);
	}
}

TEST_F(Http2AlpnTest, CallbackRemainsValidAfterWrapperMoveAndDestruction)
{
	ContextHandle client(SSL_CTX_new(TLS_client_method()), SSL_CTX_free);
	ASSERT_NE(client, nullptr);
	std::unique_ptr<TlsPair> pair;
	{
		SslContext server(boost::asio::ssl::context::tls_server);
		loadServerCertificate(server);
		server.setServerHttpProtocolMode(HttpProtocolMode::hNegotiateHttp2);
		server.freezeServerHttpProtocolConfig();
		SslContext moved(std::move(server));
		pair = std::make_unique<TlsPair>(moved.native().native_handle(), client.get());
	}
	ASSERT_EQ(SSL_set_alpn_protos(pair->client.get(), kHttp2, sizeof(kHttp2)), 0);
	ASSERT_TRUE(pair->handshake());
	EXPECT_EQ(selectedProtocol(pair->client.get()), "h2");
}

TEST_F(Http2AlpnTest, RequiredServerRejectsHttp1AndClientRejectsUnknownNegotiatedProtocol)
{
	SslContext server(boost::asio::ssl::context::tls_server);
	loadServerCertificate(server);
	server.setServerHttpProtocolMode(HttpProtocolMode::hHttp2Only);
	server.freezeServerHttpProtocolConfig();
	ContextHandle client(SSL_CTX_new(TLS_client_method()), SSL_CTX_free);
	ASSERT_NE(client, nullptr);
	TlsPair http1(server.native().native_handle(), client.get());
	ASSERT_EQ(SSL_set_alpn_protos(http1.client.get(), kHttp1, sizeof(kHttp1)), 0);
	EXPECT_FALSE(http1.handshake());

	SslContext independentServer(boost::asio::ssl::context::tls_server);
	loadServerCertificate(independentServer);
	SSL_CTX_set_alpn_select_cb(independentServer.native().native_handle(), selectFirstOffered, nullptr);
	TlsPair unknown(independentServer.native().native_handle(), client.get());
	ASSERT_EQ(SSL_set_alpn_protos(unknown.client.get(), kUnknown, sizeof(kUnknown)), 0);
	ASSERT_TRUE(unknown.handshake());
	HttpProtocol protocol {};
	EXPECT_EQ(
		SslContext::negotiatedHttpProtocol(unknown.client.get(), HttpProtocolMode::hNegotiateHttp2, protocol).code,
		ErrorCode::hProtocolNegotiationFailed);
}

TEST_F(Http2AlpnTest, RejectsInvalidConfigurationAndPreservesStricterTlsMinimum)
{
	SslContext server(boost::asio::ssl::context::tls_server);
	loadServerCertificate(server);
	EXPECT_THROW(server.setServerHttpProtocolMode(static_cast<HttpProtocolMode>(255)), std::invalid_argument);
	ASSERT_EQ(SSL_CTX_set_min_proto_version(server.native().native_handle(), TLS1_3_VERSION), 1);
	server.setServerHttpProtocolMode(HttpProtocolMode::hNegotiateHttp2);
	server.freezeServerHttpProtocolConfig();
	EXPECT_EQ(SSL_CTX_get_min_proto_version(server.native().native_handle()), TLS1_3_VERSION);
	ContextHandle client(SSL_CTX_new(TLS_client_method()), SSL_CTX_free);
	ASSERT_NE(client, nullptr);
	TlsPair pair(server.native().native_handle(), client.get());
	ASSERT_EQ(SSL_set_min_proto_version(pair.client.get(), TLS1_3_VERSION), 1);
	ASSERT_TRUE(SslContext::configureClientHttpProtocols(pair.client.get(), HttpProtocolMode::hHttp2Only).ok());
	EXPECT_EQ(SSL_get_min_proto_version(pair.client.get()), TLS1_3_VERSION);
	HttpProtocol protocol {};
	EXPECT_EQ(SslContext::negotiatedHttpProtocol(pair.client.get(), HttpProtocolMode::hHttp2Only, protocol).code,
			  ErrorCode::hInvalidConfiguration);
	ASSERT_TRUE(pair.handshake());
	EXPECT_EQ(SslContext::configureClientHttpProtocols(pair.client.get(), HttpProtocolMode::hHttp2Only).code,
			  ErrorCode::hInvalidConfiguration);
	EXPECT_EQ(SslContext::configureClientHttpProtocols(nullptr, HttpProtocolMode::hHttp2Only).code,
			  ErrorCode::hInvalidConfiguration);
}

TEST_F(Http2AlpnTest, Http2RequiresTls12AndForwardSecretAeadCipher)
{
	for (int policy = 0; policy < 3; ++policy)
	{
		SslContext server(boost::asio::ssl::context::tls_server);
		loadServerCertificate(server);
		server.setServerHttpProtocolMode(HttpProtocolMode::hNegotiateHttp2);
		server.freezeServerHttpProtocolConfig();
		ContextHandle client(SSL_CTX_new(TLS_client_method()), SSL_CTX_free);
		ASSERT_NE(client, nullptr);
		TlsPair pair(server.native().native_handle(), client.get());
		ASSERT_EQ(SSL_set_alpn_protos(pair.client.get(), kHttp2, sizeof(kHttp2)), 0);
		ASSERT_EQ(SSL_set_max_proto_version(pair.client.get(), policy == 0 ? TLS1_1_VERSION : TLS1_2_VERSION), 1);
		ASSERT_EQ(
			SSL_set_cipher_list(pair.client.get(),
								policy != 2 ? "AES128-SHA:@SECLEVEL=0" : "ECDHE-RSA-AES128-GCM-SHA256:@SECLEVEL=0"),
			1);
		ASSERT_EQ(pair.handshake(), policy == 2);
		if (policy == 2)
		{
			EXPECT_EQ(SSL_version(pair.client.get()), TLS1_2_VERSION);
			EXPECT_EQ(selectedProtocol(pair.client.get()), "h2");
			EXPECT_EQ(SSL_get_current_compression(pair.client.get()), nullptr);
			EXPECT_NE(SSL_get_options(pair.server.get()) & SSL_OP_NO_RENEGOTIATION, 0U);
		}
	}
}
#endif
