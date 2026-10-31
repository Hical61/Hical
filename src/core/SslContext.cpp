/**
 * @file SslContext.cpp
 * @brief SSL/TLS 上下文实现
 */

#include "SslContext.h"
#ifdef HICAL_HAS_HTTP2
	#include <openssl/ssl.h>
	#include <span>
	#include <string_view>
#endif
#include <stdexcept>

namespace hical
{

#ifdef HICAL_HAS_HTTP2
	namespace
	{
		constexpr unsigned char kHttp1Alpn[] = {8, 'h', 't', 't', 'p', '/', '1', '.', '1'};
		constexpr unsigned char kHttp2Alpn[] = {2, 'h', '2'};
		constexpr unsigned char kHttp2AndHttp1Alpn[] = {2, 'h', '2', 8, 'h', 't', 't', 'p', '/', '1', '.', '1'};
		constexpr char kHttp2Tls12Ciphers[] = "ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-RSA-AES128-GCM-SHA256:"
											  "ECDHE-ECDSA-AES256-GCM-SHA384:ECDHE-RSA-AES256-GCM-SHA384:"
											  "ECDHE-ECDSA-CHACHA20-POLY1305:ECDHE-RSA-CHACHA20-POLY1305:"
											  "DHE-RSA-AES128-GCM-SHA256:DHE-RSA-AES256-GCM-SHA384";
		constexpr char kHttp2Groups[] = "X25519:P-256:P-384:ffdhe2048:ffdhe3072:ffdhe4096";
		constexpr auto kHttp2TlsOptions = SSL_OP_NO_COMPRESSION | SSL_OP_NO_RENEGOTIATION;

		bool validProtocolMode(HttpProtocolMode mode) noexcept
		{
			return mode == HttpProtocolMode::hHttp1Only || mode == HttpProtocolMode::hNegotiateHttp2
				   || mode == HttpProtocolMode::hHttp2Only;
		}

		std::span<const unsigned char> protocolList(HttpProtocolMode mode) noexcept
		{
			if (mode == HttpProtocolMode::hHttp1Only)
			{
				return kHttp1Alpn;
			}
			if (mode == HttpProtocolMode::hHttp2Only)
			{
				return kHttp2Alpn;
			}
			return kHttp2AndHttp1Alpn;
		}

		// 回调和输出列表都具有静态存储期，不借用 SslContext 的地址或可变配置。
		template <HttpProtocolMode Mode>
		int selectServerAlpn(SSL*,
							 const unsigned char** out,
							 unsigned char* outLength,
							 const unsigned char* offered,
							 unsigned int offeredLength,
							 void*) noexcept
		{
			const auto supported = protocolList(Mode);
			unsigned char* selected = nullptr;
			const int result = SSL_select_next_proto(&selected,
													 outLength,
													 supported.data(),
													 static_cast<unsigned int>(supported.size()),
													 offered,
													 offeredLength);
			if (result != OPENSSL_NPN_NEGOTIATED)
			{
				return SSL_TLSEXT_ERR_ALERT_FATAL;
			}
			*out = selected;
			return SSL_TLSEXT_ERR_OK;
		}

		bool configureServerHttp2Tls(SSL_CTX* context)
		{
			const auto maximum = SSL_CTX_get_max_proto_version(context);
			if (maximum != 0 && maximum < TLS1_2_VERSION)
			{
				return false;
			}
			const auto minimum = SSL_CTX_get_min_proto_version(context);
			if ((minimum < TLS1_2_VERSION && SSL_CTX_set_min_proto_version(context, TLS1_2_VERSION) != 1)
				|| SSL_CTX_set_cipher_list(context, kHttp2Tls12Ciphers) != 1
				|| SSL_CTX_set1_groups_list(context, kHttp2Groups) != 1 || SSL_CTX_set_dh_auto(context, 1) != 1)
			{
				return false;
			}
			SSL_CTX_set_options(context, kHttp2TlsOptions);
			SSL_CTX_set_post_handshake_auth(context, 0);
			return true;
		}

		bool configureClientHttp2Tls(SSL* ssl)
		{
			const auto maximum = SSL_get_max_proto_version(ssl);
			if (maximum != 0 && maximum < TLS1_2_VERSION)
			{
				return false;
			}
			const auto minimum = SSL_get_min_proto_version(ssl);
			if ((minimum < TLS1_2_VERSION && SSL_set_min_proto_version(ssl, TLS1_2_VERSION) != 1)
				|| SSL_set_cipher_list(ssl, kHttp2Tls12Ciphers) != 1 || SSL_set1_groups_list(ssl, kHttp2Groups) != 1)
			{
				return false;
			}
			SSL_set_options(ssl, kHttp2TlsOptions);
			SSL_set_post_handshake_auth(ssl, 0);
			return true;
		}
	} // namespace
#endif

	SslContext::SslContext() : ctx_(boost::asio::ssl::context::tls)
	{
	}

	SslContext::SslContext(boost::asio::ssl::context::method method) : ctx_(method)
	{
	}

	void SslContext::loadCertificate(const std::string& certFile)
	{
		boost::system::error_code ec;
		ctx_.use_certificate_chain_file(certFile, ec);
		if (ec)
		{
			throw std::runtime_error("加载证书失败: " + certFile + " - " + ec.message());
		}
	}

	void SslContext::loadPrivateKey(const std::string& keyFile)
	{
		boost::system::error_code ec;
		ctx_.use_private_key_file(keyFile, boost::asio::ssl::context::pem, ec);
		if (ec)
		{
			throw std::runtime_error("加载私钥失败: " + keyFile + " - " + ec.message());
		}
	}

	void SslContext::loadCaCertificate(const std::string& caFile)
	{
		boost::system::error_code ec;
		ctx_.load_verify_file(caFile, ec);
		if (ec)
		{
			throw std::runtime_error("加载 CA 证书失败: " + caFile + " - " + ec.message());
		}
	}

	void SslContext::setVerifyPeer(bool verifyPeer)
	{
		if (verifyPeer)
		{
			ctx_.set_verify_mode(boost::asio::ssl::verify_peer | boost::asio::ssl::verify_fail_if_no_peer_cert);
		}
		else
		{
			ctx_.set_verify_mode(boost::asio::ssl::verify_none);
		}
	}

#ifdef HICAL_HAS_HTTP2
	void SslContext::setServerHttpProtocolMode(HttpProtocolMode mode)
	{
		if (serverHttpProtocolConfigFrozen_)
		{
			throw std::logic_error("服务端 HTTP 协议配置已经冻结");
		}
		if (!validProtocolMode(mode))
		{
			throw std::invalid_argument("无效的服务端 HTTP 协议策略");
		}
		serverHttpProtocolMode_ = mode;
	}

	void SslContext::freezeServerHttpProtocolConfig()
	{
		if (serverHttpProtocolConfigFrozen_)
		{
			return;
		}
		auto* context = ctx_.native_handle();
		if (serverHttpProtocolMode_ != HttpProtocolMode::hHttp1Only && !configureServerHttp2Tls(context))
		{
			throw std::runtime_error("无法配置 HTTP/2 所需的 TLS 版本、密钥交换或密码套件");
		}
		switch (serverHttpProtocolMode_)
		{
			case HttpProtocolMode::hHttp1Only:
				SSL_CTX_set_alpn_select_cb(context, selectServerAlpn<HttpProtocolMode::hHttp1Only>, nullptr);
				break;
			case HttpProtocolMode::hNegotiateHttp2:
				SSL_CTX_set_alpn_select_cb(context, selectServerAlpn<HttpProtocolMode::hNegotiateHttp2>, nullptr);
				break;
			case HttpProtocolMode::hHttp2Only:
				SSL_CTX_set_alpn_select_cb(context, selectServerAlpn<HttpProtocolMode::hHttp2Only>, nullptr);
				break;
		}
		serverHttpProtocolConfigFrozen_ = true;
	}

	HttpProtocolMode SslContext::serverHttpProtocolMode() const noexcept
	{
		return serverHttpProtocolMode_;
	}

	bool SslContext::serverHttpProtocolConfigFrozen() const noexcept
	{
		return serverHttpProtocolConfigFrozen_;
	}

	NetworkError SslContext::configureClientHttpProtocols(SSL* ssl, HttpProtocolMode mode)
	{
		if (ssl == nullptr || !validProtocolMode(mode) || !SSL_in_before(ssl))
		{
			return {ErrorCode::hInvalidConfiguration, "必须在握手前为有效 SSL 对象设置 HTTP 协议策略"};
		}
		if (mode != HttpProtocolMode::hHttp1Only && !configureClientHttp2Tls(ssl))
		{
			return {ErrorCode::hInvalidConfiguration, "无法配置 HTTP/2 所需的客户端 TLS 参数"};
		}
		const auto protocols = protocolList(mode);
		if (SSL_set_alpn_protos(ssl, protocols.data(), static_cast<unsigned int>(protocols.size())) != 0)
		{
			return {ErrorCode::hInvalidConfiguration, "无法设置客户端 HTTP ALPN 列表"};
		}
		return {};
	}

	NetworkError SslContext::negotiatedHttpProtocol(SSL* ssl, HttpProtocolMode mode, HttpProtocol& protocol)
	{
		protocol = HttpProtocol::hHttp1;
		if (ssl == nullptr || !validProtocolMode(mode) || !SSL_is_init_finished(ssl))
		{
			return {ErrorCode::hInvalidConfiguration, "必须在 TLS 握手完成后读取 HTTP 协商结果"};
		}
		const unsigned char* selected = nullptr;
		unsigned int selectedLength = 0;
		SSL_get0_alpn_selected(ssl, &selected, &selectedLength);
		const auto name = selectedLength == 0
							  ? std::string_view {}
							  : std::string_view(reinterpret_cast<const char*>(selected), selectedLength);
		if (name == "h2" && mode != HttpProtocolMode::hHttp1Only)
		{
			if (SSL_version(ssl) < TLS1_2_VERSION || SSL_get_current_compression(ssl) != nullptr)
			{
				return {ErrorCode::hSslProtocolError, "HTTP/2 要求 TLS 1.2 及以上且不得使用 TLS 压缩"};
			}
			protocol = HttpProtocol::hHttp2;
			return {};
		}
		if ((name.empty() || name == "http/1.1") && mode != HttpProtocolMode::hHttp2Only)
		{
			return {};
		}
		return {ErrorCode::hProtocolNegotiationFailed, "对端未协商出协议策略允许的 HTTP 版本"};
	}
#endif

	boost::asio::ssl::context& SslContext::native()
	{
		return ctx_;
	}

	const boost::asio::ssl::context& SslContext::native() const
	{
		return ctx_;
	}

} // namespace hical
