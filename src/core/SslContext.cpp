/**
 * @file SslContext.cpp
 * @brief SSL/TLS 上下文实现
 */

#include "SslContext.h"
#include <stdexcept>

namespace hical
{

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

	boost::asio::ssl::context& SslContext::native()
	{
		return ctx_;
	}

	const boost::asio::ssl::context& SslContext::native() const
	{
		return ctx_;
	}

} // namespace hical
