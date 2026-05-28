/**
 * @file SslContext.h
 * @brief SSL/TLS 上下文配置与证书加载
 */

#pragma once

#include <boost/asio/ssl.hpp>
#include <string>

namespace hical
{

	/**
	 * @brief SSL/TLS 上下文配置封装
	 * 管理证书、私钥、CA 证书的加载，以及验证模式的设置。
	 * 内部持有 boost::asio::ssl::context。
	 */
	class SslContext
	{
	public:
		/**
		 * @brief 构造函数（默认使用 TLS 客户端/服务端自适应方法）
		 */
		SslContext();

		/**
		 * @brief 以指定 SSL 方法构造
		 * @param method SSL 方法（如 tls_server, tls_client）
		 */
		explicit SslContext(boost::asio::ssl::context::method method);

		/**
		 * @brief 加载服务端证书文件（PEM 格式）
		 * @param certFile 证书文件路径
		 */
		void loadCertificate(const std::string& certFile);

		/**
		 * @brief 加载服务端私钥文件（PEM 格式）
		 * @param keyFile 私钥文件路径
		 */
		void loadPrivateKey(const std::string& keyFile);

		/**
		 * @brief 加载 CA 证书文件（PEM 格式，用于验证对端证书）
		 * @param caFile CA 证书文件路径
		 */
		void loadCaCertificate(const std::string& caFile);

		/**
		 * @brief 设置是否验证对端证书
		 * @param verifyPeer true 则验证对端证书
		 */
		void setVerifyPeer(bool verifyPeer);

		/**
		 * @brief 获取底层 boost::asio::ssl::context 引用
		 * @return ssl context 引用
		 */
		boost::asio::ssl::context& native();

		/**
		 * @brief 获取底层 ssl::context 的 const 引用
		 * @return ssl context const 引用
		 */
		const boost::asio::ssl::context& native() const;

	private:
		boost::asio::ssl::context ctx_;
	};

} // namespace hical
