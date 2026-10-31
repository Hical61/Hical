/**
 * @file test_http2_client_tls.cpp
 * @brief 通过 TLS 对端验证 ALPN、证书身份与 H2 收发任务的生命周期
 */

#include "asio/HttpClient.h"
#include <boost/asio/post.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/read_until.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/asio/write.hpp>
#include <gtest/gtest.h>
#include <openssl/ec.h>
#include <openssl/x509v3.h>
#include <array>
#include <chrono>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using namespace hical;

namespace
{
	using Tcp = boost::asio::ip::tcp;
	using TlsSocket = boost::asio::ssl::stream<Tcp::socket>;
	using Key = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
	using Certificate = std::unique_ptr<X509, decltype(&X509_free)>;
	constexpr auto kTestTimeout = std::chrono::seconds {5};
	constexpr size_t kMaxWireBytes = 64 * 1024;
	constexpr std::string_view kPreface = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";

	void require(bool success)
	{
		if (!success)
		{
			throw std::runtime_error("无法在内存中生成测试 CA、SAN 证书或配置 TLS");
		}
	}

	Key makeKey()
	{
		std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> context(EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr),
																			EVP_PKEY_CTX_free);
		require(context && EVP_PKEY_keygen_init(context.get()) == 1
				&& EVP_PKEY_CTX_set_ec_paramgen_curve_nid(context.get(), NID_X9_62_prime256v1) == 1);
		EVP_PKEY* key = nullptr;
		require(EVP_PKEY_keygen(context.get(), &key) == 1);
		return {key, EVP_PKEY_free};
	}

	void addExtension(X509* certificate, X509* issuer, int kind, const char* value)
	{
		X509V3_CTX context;
		X509V3_set_ctx(&context, issuer, certificate, nullptr, nullptr, 0);
		std::unique_ptr<X509_EXTENSION, decltype(&X509_EXTENSION_free)> extension(
			X509V3_EXT_conf_nid(nullptr, &context, kind, value),
			X509_EXTENSION_free);
		require(extension && X509_add_ext(certificate, extension.get(), -1) == 1);
	}

	Certificate makeCertificate(EVP_PKEY* key, X509* issuer, EVP_PKEY* issuerKey, const char* san)
	{
		Certificate certificate(X509_new(), X509_free);
		require(certificate && X509_set_version(certificate.get(), 2) == 1
				&& ASN1_INTEGER_set(X509_get_serialNumber(certificate.get()), issuer ? 2 : 1) == 1
				&& X509_gmtime_adj(X509_getm_notBefore(certificate.get()), -60)
				&& X509_gmtime_adj(X509_getm_notAfter(certificate.get()), 24 * 60 * 60)
				&& X509_set_pubkey(certificate.get(), key) == 1);
		auto* name = X509_get_subject_name(certificate.get());
		require(
			X509_NAME_add_entry_by_txt(name,
									   "CN",
									   MBSTRING_ASC,
									   reinterpret_cast<const unsigned char*>(issuer ? "localhost" : "Hical test CA"),
									   -1,
									   -1,
									   0)
				== 1
			&& X509_set_issuer_name(certificate.get(), issuer ? X509_get_subject_name(issuer) : name) == 1);
		addExtension(certificate.get(),
					 issuer ? issuer : certificate.get(),
					 NID_basic_constraints,
					 issuer ? "critical,CA:FALSE" : "critical,CA:TRUE");
		addExtension(certificate.get(),
					 issuer ? issuer : certificate.get(),
					 NID_key_usage,
					 issuer ? "critical,digitalSignature" : "critical,keyCertSign,cRLSign");
		if (issuer)
		{
			addExtension(certificate.get(), issuer, NID_ext_key_usage, "serverAuth");
			addExtension(certificate.get(), issuer, NID_subject_alt_name, san);
		}
		require(X509_sign(certificate.get(), issuerKey ? issuerKey : key, EVP_sha256()) > 0);
		return certificate;
	}

	struct Identity
	{
		Key caKey {makeKey()};
		Certificate ca {makeCertificate(caKey.get(), nullptr, nullptr, nullptr)};
		Key key {makeKey()};
		Certificate leaf;

		explicit Identity(const char* san) : leaf(makeCertificate(key.get(), ca.get(), caKey.get(), san))
		{
		}
	};

	struct Peer
	{
		std::shared_ptr<TlsSocket> socket;
		std::vector<std::string> offered;
		std::string selected;
		std::string sni;
		std::string preface;
		bool settings {false};
		bool settingsAck {false};
		bool closed {false};
	};

	struct Harness
	{
		boost::asio::io_context io;
		Tcp::acceptor acceptor;
		boost::asio::steady_timer watchdog {io};
		Identity identity;
		std::shared_ptr<SslContext> serverContext {std::make_shared<SslContext>(boost::asio::ssl::context::tls_server)};
		std::shared_ptr<SslContext> clientContext {std::make_shared<SslContext>(boost::asio::ssl::context::tls_client)};
		std::vector<std::shared_ptr<Peer>> peers;
		std::vector<std::string> supported {"h2", "http/1.1"};
		bool timedOut {false};
		size_t http1Requests {0};

		explicit Harness(const char* san = "DNS:localhost,IP:127.0.0.1,IP:::1", bool ipv6 = false)
			: acceptor(io,
					   {ipv6 ? boost::asio::ip::address(boost::asio::ip::address_v6::loopback())
							 : boost::asio::ip::address(boost::asio::ip::address_v4::loopback()),
						0})
			, identity(san)
		{
			auto* context = serverContext->native().native_handle();
			require(peerIndex() >= 0 && SSL_CTX_use_certificate(context, identity.leaf.get()) == 1
					&& SSL_CTX_use_PrivateKey(context, identity.key.get()) == 1
					&& X509_STORE_add_cert(SSL_CTX_get_cert_store(clientContext->native().native_handle()),
										   identity.ca.get())
						   == 1);
			SSL_CTX_set_alpn_select_cb(context, selectProtocol, this);
		}

		static int peerIndex()
		{
			static const int index = SSL_get_ex_new_index(0, nullptr, nullptr, nullptr, nullptr);
			return index;
		}

		static int selectProtocol(SSL* ssl,
								  const unsigned char** output,
								  unsigned char* outputLength,
								  const unsigned char* input,
								  unsigned int length,
								  void* data) noexcept
		{
			auto& harness = *static_cast<Harness*>(data);
			auto* peer = static_cast<Peer*>(SSL_get_ex_data(ssl, peerIndex()));
			try
			{
				for (size_t offset = 0; offset < length;)
				{
					const auto size = input[offset++];
					if (size == 0 || size > length - offset)
					{
						return SSL_TLSEXT_ERR_ALERT_FATAL;
					}
					peer->offered.emplace_back(reinterpret_cast<const char*>(input + offset), size);
					offset += size;
				}
				for (const auto& supported : harness.supported)
				{
					for (size_t offset = 0; offset < length;)
					{
						const auto size = input[offset++];
						if (std::string_view(reinterpret_cast<const char*>(input + offset), size) == supported)
						{
							*output = input + offset;
							*outputLength = size;
							return SSL_TLSEXT_ERR_OK;
						}
						offset += size;
					}
				}
				return SSL_TLSEXT_ERR_NOACK;
			}
			catch (...)
			{
				return SSL_TLSEXT_ERR_ALERT_FATAL;
			}
		}

		uint16_t port() const
		{
			return acceptor.local_endpoint().port();
		}

		HttpClientOptions options(HttpProtocolMode mode = HttpProtocolMode::hNegotiateHttp2) const
		{
			HttpClientOptions result;
			result.tlsContext = clientContext;
			result.protocolMode = mode;
			return result;
		}

		void stop()
		{
			boost::system::error_code ignored;
			acceptor.close(ignored);
			for (const auto& peer : peers)
			{
				peer->socket->next_layer().close(ignored);
			}
			watchdog.cancel();
		}

		Awaitable<void> serve(std::shared_ptr<Peer> peer)
		{
			boost::system::error_code error;
			co_await peer->socket->async_handshake(boost::asio::ssl::stream_base::server,
												   boost::asio::redirect_error(boost::asio::use_awaitable, error));
			if (!error)
			{
				const unsigned char* selected = nullptr;
				unsigned int length = 0;
				SSL_get0_alpn_selected(peer->socket->native_handle(), &selected, &length);
				peer->selected = length ? std::string(reinterpret_cast<const char*>(selected), length) : std::string {};
				const auto* sni = SSL_get_servername(peer->socket->native_handle(), TLSEXT_NAMETYPE_host_name);
				peer->sni = sni ? sni : "";
				if (peer->selected == "h2")
				{
					co_await serveHttp2(peer);
				}
				else
				{
					std::string request;
					co_await boost::asio::async_read_until(
						*peer->socket,
						boost::asio::dynamic_buffer(request, kMaxWireBytes),
						"\r\n\r\n",
						boost::asio::redirect_error(boost::asio::use_awaitable, error));
					if (!error)
					{
						++http1Requests;
						const std::string response =
							"HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok";
						co_await boost::asio::async_write(
							*peer->socket,
							boost::asio::buffer(response),
							boost::asio::redirect_error(boost::asio::use_awaitable, error));
					}
				}
			}
			peer->closed = true;
		}

		Awaitable<void> serveHttp2(std::shared_ptr<Peer> peer)
		{
			boost::system::error_code error;
			peer->preface.resize(kPreface.size());
			co_await boost::asio::async_read(*peer->socket,
											 boost::asio::buffer(peer->preface),
											 boost::asio::redirect_error(boost::asio::use_awaitable, error));
			if (error)
			{
				co_return;
			}
			EXPECT_EQ(peer->preface, kPreface);
			const std::array<unsigned char, 9> settings {0, 0, 0, 4, 0, 0, 0, 0, 0};
			co_await boost::asio::async_write(*peer->socket,
											  boost::asio::buffer(settings),
											  boost::asio::redirect_error(boost::asio::use_awaitable, error));
			while (!error)
			{
				std::array<unsigned char, 9> head {};
				co_await boost::asio::async_read(*peer->socket,
												 boost::asio::buffer(head),
												 boost::asio::redirect_error(boost::asio::use_awaitable, error));
				if (error)
				{
					break;
				}
				const size_t length = (size_t(head[0]) << 16U) | (size_t(head[1]) << 8U) | head[2];
				if (length > kMaxWireBytes)
				{
					ADD_FAILURE() << "对端帧超过测试读取上限";
					co_return;
				}
				std::string payload(length, '\0');
				if (length != 0)
				{
					co_await boost::asio::async_read(*peer->socket,
													 boost::asio::buffer(payload),
													 boost::asio::redirect_error(boost::asio::use_awaitable, error));
				}
				if (!error && head[3] == 4)
				{
					peer->settings |= (head[4] & 1U) == 0;
					peer->settingsAck |= (head[4] & 1U) != 0;
				}
				if (!error && head[3] == 1 && (head[4] & 4U) != 0)
				{
					// 独立对端的 HPACK 静态索引 8 为 :status 200，最终头同时结束响应。
					const std::array<unsigned char, 10> response {0,
																  0,
																  1,
																  1,
																  5,
																  static_cast<unsigned char>(head[5] & 0x7fU),
																  head[6],
																  head[7],
																  head[8],
																  0x88};
					co_await boost::asio::async_write(*peer->socket,
													  boost::asio::buffer(response),
													  boost::asio::redirect_error(boost::asio::use_awaitable, error));
				}
			}
		}

		Awaitable<void> accept()
		{
			for (;;)
			{
				auto peer = std::make_shared<Peer>();
				peer->socket = std::make_shared<TlsSocket>(io, serverContext->native());
				boost::system::error_code error;
				co_await acceptor.async_accept(peer->socket->next_layer(),
											   boost::asio::redirect_error(boost::asio::use_awaitable, error));
				if (error)
				{
					co_return;
				}
				require(SSL_set_ex_data(peer->socket->native_handle(), peerIndex(), peer.get()) == 1);
				peers.push_back(peer);
				coSpawn(io, serve(std::move(peer)));
			}
		}

		Awaitable<void> ready(size_t count)
		{
			if (count == 0)
			{
				ADD_FAILURE() << "尚未建立可等待的 TLS 对端";
				co_return;
			}
			while (!timedOut && (peers.size() < count || !peers[count - 1]->settingsAck))
			{
				co_await boost::asio::post(boost::asio::use_awaitable);
			}
		}

		void run(Awaitable<void> scenario)
		{
			watchdog.expires_at(std::chrono::steady_clock::now() + kTestTimeout);
			watchdog.async_wait(
				[this](boost::system::error_code error)
				{
					if (!error)
					{
						timedOut = true;
						stop();
					}
				});
			coSpawn(io, accept());
			auto future = boost::asio::co_spawn(io, std::move(scenario), boost::asio::use_future);
			while (future.wait_for(std::chrono::seconds::zero()) != std::future_status::ready && !timedOut)
			{
				if (io.run_one() == 0)
				{
					break;
				}
			}
			stop();
			io.restart();
			io.run();
			ASSERT_EQ(future.wait_for(std::chrono::seconds::zero()), std::future_status::ready);
			EXPECT_NO_THROW(future.get());
			EXPECT_FALSE(timedOut);
		}
	};

#ifdef HICAL_HAS_HTTP2
	void expectH2Response(const HttpClientResult& result)
	{
		EXPECT_EQ(result.protocol, HttpProtocol::hHttp2);
		EXPECT_EQ(result.requestDisposition, HttpRequestDisposition::hPossiblyProcessed);
		EXPECT_TRUE(result.ok()) << result.error.message;
		EXPECT_EQ(result.status, 200);
	}
#endif
} // namespace

TEST(Http2ClientTlsTest, DefaultModeOffersOnlyHttp1AndKeepsThePublishedResponsePath)
{
	Harness harness;
	HttpClientOptions options;
	options.tlsContext = harness.clientContext;
	HttpClient client(harness.io, "localhost", harness.port(), options);
	auto scenario = [&]() -> Awaitable<void>
	{
		const auto result = co_await client.get("/");
		EXPECT_TRUE(result.ok()) << result.error.message;
		EXPECT_EQ(result.protocol, HttpProtocol::hHttp1);
		EXPECT_EQ(result.body, "ok");
	};
	harness.run(scenario());
	ASSERT_EQ(harness.peers.size(), 1U);
	EXPECT_EQ(harness.peers[0]->offered, (std::vector<std::string> {"http/1.1"}));
}

#ifdef HICAL_HAS_HTTP2
TEST(Http2ClientTlsTest, AutoNegotiatesHttp2AndRunsOnePrefaceSettingsPumpWithoutSendingHttp1)
{
	Harness harness;
	HttpClient client(harness.io, "localhost", harness.port(), harness.options());
	auto scenario = [&]() -> Awaitable<void>
	{
		expectH2Response(co_await client.get("/first"));
		co_await harness.ready(1);
		expectH2Response(co_await client.get("/second"));
		client.close();
	};
	harness.run(scenario());
	ASSERT_EQ(harness.peers.size(), 1U);
	EXPECT_EQ(harness.peers[0]->offered, (std::vector<std::string> {"h2", "http/1.1"}));
	EXPECT_EQ(harness.peers[0]->selected, "h2");
	EXPECT_TRUE(harness.peers[0]->settings);
	EXPECT_TRUE(harness.peers[0]->settingsAck);
	EXPECT_EQ(harness.http1Requests, 0U);
}

TEST(Http2ClientTlsTest, ForcedHttp2OffersOnlyHttp2AndOwnsBothPumpsUntilFacadeDestruction)
{
	Harness harness;
	auto client = std::make_unique<HttpClient>(harness.io,
											   "127.0.0.1",
											   harness.port(),
											   harness.options(HttpProtocolMode::hHttp2Only));
	auto scenario = [&]() -> Awaitable<void>
	{
		expectH2Response(co_await client->get("/"));
		co_await harness.ready(1);
		client.reset();
		while (!harness.timedOut && !harness.peers[0]->closed)
		{
			co_await boost::asio::post(boost::asio::use_awaitable);
		}
	};
	harness.run(scenario());
	ASSERT_EQ(harness.peers.size(), 1U);
	EXPECT_EQ(harness.peers[0]->offered, (std::vector<std::string> {"h2"}));
	EXPECT_TRUE(harness.peers[0]->closed);
}

TEST(Http2ClientTlsTest, AutoFallsBackForHttp1OnlyAndAbsentAlpn)
{
	for (const auto& supported : {std::vector<std::string> {"http/1.1"}, std::vector<std::string> {}})
	{
		Harness harness;
		harness.supported = supported;
		HttpClient client(harness.io, "localhost", harness.port(), harness.options());
		auto scenario = [&]() -> Awaitable<void>
		{
			const auto result = co_await client.get("/");
			EXPECT_TRUE(result.ok()) << result.error.message;
			EXPECT_EQ(result.protocol, HttpProtocol::hHttp1);
		};
		harness.run(scenario());
		EXPECT_EQ(harness.http1Requests, 1U);
	}
}

TEST(Http2ClientTlsTest, ForcedHttp2RejectsHttp1OnlyAndAbsentAlpnWithoutSendingAnHttp1Request)
{
	for (const auto& supported : {std::vector<std::string> {"http/1.1"}, std::vector<std::string> {}})
	{
		Harness harness;
		harness.supported = supported;
		HttpClient client(harness.io, "localhost", harness.port(), harness.options(HttpProtocolMode::hHttp2Only));
		auto scenario = [&]() -> Awaitable<void>
		{
			const auto result = co_await client.get("/");
			EXPECT_EQ(result.error.code, ErrorCode::hProtocolNegotiationFailed);
			EXPECT_EQ(result.requestDisposition, HttpRequestDisposition::hNotSent);
			EXPECT_FALSE(client.isAlive());
		};
		harness.run(scenario());
		EXPECT_EQ(harness.peers.size(), 1U);
		EXPECT_EQ(harness.http1Requests, 0U);
	}
}

TEST(Http2ClientTlsTest, SharedContextKeepsEachClientsAlpnAndVerificationConfigurationIndependent)
{
	Harness harness;
	const auto minimum = SSL_CTX_get_min_proto_version(harness.clientContext->native().native_handle());
	const auto verification = SSL_CTX_get_verify_mode(harness.clientContext->native().native_handle());
	auto scenario = [&]() -> Awaitable<void>
	{
		for (const auto mode : {HttpProtocolMode::hHttp1Only,
								HttpProtocolMode::hNegotiateHttp2,
								HttpProtocolMode::hHttp2Only,
								HttpProtocolMode::hHttp1Only})
		{
			// 比较各 SSL 对象的配置时固定 IPv4 地址，避免 DNS 双栈回落消耗测试期限。
			HttpClient client(harness.io, "127.0.0.1", harness.port(), harness.options(mode));
			const auto result = co_await client.get("/");
			if (mode == HttpProtocolMode::hHttp1Only)
			{
				EXPECT_TRUE(result.ok()) << result.error.message;
			}
			else
			{
				expectH2Response(result);
				co_await harness.ready(harness.peers.size());
			}
			client.close();
		}
	};
	harness.run(scenario());
	ASSERT_EQ(harness.peers.size(), 4U);
	EXPECT_EQ(harness.peers[0]->offered, (std::vector<std::string> {"http/1.1"}));
	EXPECT_EQ(harness.peers[1]->offered, (std::vector<std::string> {"h2", "http/1.1"}));
	EXPECT_EQ(harness.peers[2]->offered, (std::vector<std::string> {"h2"}));
	EXPECT_EQ(harness.peers[3]->offered, (std::vector<std::string> {"http/1.1"}));
	EXPECT_EQ(SSL_CTX_get_min_proto_version(harness.clientContext->native().native_handle()), minimum);
	EXPECT_EQ(SSL_CTX_get_verify_mode(harness.clientContext->native().native_handle()), verification);
}

TEST(Http2ClientTlsTest, CloseAndImmediateReconnectWaitForTheOldHttp2PumpsToJoin)
{
	Harness harness;
	HttpClient client(harness.io, "localhost", harness.port(), harness.options());
	auto scenario = [&]() -> Awaitable<void>
	{
		expectH2Response(co_await client.get("/old"));
		co_await harness.ready(1);
		client.close();
		expectH2Response(co_await client.get("/new"));
		co_await harness.ready(2);
		while (!harness.timedOut && !harness.peers[0]->closed)
		{
			co_await boost::asio::post(boost::asio::use_awaitable);
		}
		EXPECT_TRUE(harness.peers[0]->closed);
		client.close();
	};
	harness.run(scenario());
	EXPECT_EQ(harness.peers.size(), 2U);
}

TEST(Http2ClientTlsTest, Http2ModesWithoutTlsFailBeforeConnecting)
{
	for (const auto mode : {HttpProtocolMode::hNegotiateHttp2, HttpProtocolMode::hHttp2Only})
	{
		boost::asio::io_context io;
		HttpClientOptions options;
		options.protocolMode = mode;
		HttpClient client(io, "invalid.invalid", 443, options);
		auto future = boost::asio::co_spawn(io, client.get("/"), boost::asio::use_future);
		io.run();
		const auto result = future.get();
		EXPECT_EQ(result.error.code, ErrorCode::hInvalidConfiguration);
		EXPECT_EQ(result.requestDisposition, HttpRequestDisposition::hNotSent);
	}
}
#endif

TEST(Http2ClientTlsTest, WrongDnsSanFailsEvenThoughTheSigningCaIsTrusted)
{
	Harness harness("DNS:wrong.example");
	HttpClient client(harness.io, "localhost", harness.port(), harness.options(HttpProtocolMode::hHttp1Only));
	auto scenario = [&]() -> Awaitable<void>
	{
		const auto result = co_await client.get("/");
		EXPECT_EQ(result.error.code, ErrorCode::hSslInvalidCertificate);
		EXPECT_EQ(result.requestDisposition, HttpRequestDisposition::hNotSent);
	};
	harness.run(scenario());
	EXPECT_EQ(harness.http1Requests, 0U);
}

TEST(Http2ClientTlsTest, IpLiteralRequiresAnIpSanRatherThanTheSameTextInDnsSan)
{
	Harness harness("DNS:127.0.0.1");
	HttpClient client(harness.io, "127.0.0.1", harness.port(), harness.options(HttpProtocolMode::hHttp1Only));
	auto scenario = [&]() -> Awaitable<void>
	{
		const auto result = co_await client.get("/");
		EXPECT_EQ(result.error.code, ErrorCode::hSslInvalidCertificate);
	};
	harness.run(scenario());
	EXPECT_EQ(harness.http1Requests, 0U);
}

TEST(Http2ClientTlsTest, Ipv6LiteralUsesItsIpSan)
{
	Harness harness("IP:::1", true);
	HttpClient client(harness.io, "::1", harness.port(), harness.options(HttpProtocolMode::hHttp1Only));
	auto scenario = [&]() -> Awaitable<void>
	{
		const auto result = co_await client.get("/");
		EXPECT_TRUE(result.ok()) << result.error.message;
	};
	harness.run(scenario());
}

TEST(Http2ClientTlsTest, SniOverrideRoutesTheHandshakeWithoutReplacingTheTargetIdentity)
{
	for (const bool matchingTarget : {true, false})
	{
		Harness harness(matchingTarget ? "DNS:localhost" : "DNS:route.example");
		auto options = harness.options(HttpProtocolMode::hHttp1Only);
		options.sniHost = "route.example";
		HttpClient client(harness.io, "localhost", harness.port(), options);
		auto scenario = [&]() -> Awaitable<void>
		{
			const auto result = co_await client.get("/");
			if (matchingTarget)
			{
				EXPECT_TRUE(result.ok()) << result.error.message;
			}
			else
			{
				EXPECT_EQ(result.error.code, ErrorCode::hSslInvalidCertificate);
			}
		};
		harness.run(scenario());
		if (matchingTarget)
		{
			ASSERT_EQ(harness.peers.size(), 1U);
			EXPECT_EQ(harness.peers[0]->sni, "route.example");
		}
	}
}

TEST(Http2ClientTlsTest, UntrustedChainDoesNotReconnectUsingPlaintext)
{
	Harness harness;
	auto options = harness.options(
#ifdef HICAL_HAS_HTTP2
		HttpProtocolMode::hNegotiateHttp2
#else
		HttpProtocolMode::hHttp1Only
#endif
	);
	options.tlsContext = std::make_shared<SslContext>(boost::asio::ssl::context::tls_client);
	HttpClient client(harness.io, "localhost", harness.port(), options);
	auto scenario = [&]() -> Awaitable<void>
	{
		const auto result = co_await client.get("/");
		EXPECT_EQ(result.error.code, ErrorCode::hSslInvalidCertificate);
		EXPECT_FALSE(client.isAlive());
	};
	harness.run(scenario());
	EXPECT_EQ(harness.peers.size(), 1U);
	EXPECT_EQ(harness.http1Requests, 0U);
}

TEST(Http2ClientTlsTest, VerifyNoneOnOneClientDoesNotDisableAnotherClientsChainAndIdentityChecks)
{
	Harness harness("DNS:wrong.example");
	harness.clientContext = std::make_shared<SslContext>(boost::asio::ssl::context::tls_client);
	auto scenario = [&]() -> Awaitable<void>
	{
		for (const bool verify : {false, true, false})
		{
			auto options = harness.options(HttpProtocolMode::hHttp1Only);
			options.verifyPeer = verify;
			// DNS 身份另有专项覆盖；这里固定监听地址，确保期限用于三次实际 TLS 交换。
			HttpClient client(harness.io, "127.0.0.1", harness.port(), options);
			const auto result = co_await client.get("/");
			if (verify)
			{
				EXPECT_EQ(result.error.code, ErrorCode::hSslInvalidCertificate);
			}
			else
			{
				EXPECT_TRUE(result.ok()) << result.error.message;
			}
		}
	};
	harness.run(scenario());
	EXPECT_EQ(harness.peers.size(), 3U);
	EXPECT_EQ(harness.http1Requests, 2U);
}
