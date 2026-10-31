/**
 * @file SslContext.h
 * @brief SSL/TLS 上下文配置与证书加载
 */

#pragma once

#ifdef HICAL_HAS_HTTP2
	#include "Error.h"
	#include "HttpProtocol.h"
#endif
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

#ifdef HICAL_HAS_HTTP2
		/**
		 * @brief 设置服务端协议策略；默认 H1，冻结后不可修改
		 * 仅记录策略，不立即改变 context，可先后加载证书。配置过程只在启动线程执行。
		 * @param mode H1、优先 H2 并允许 H1，或仅 H2；无效枚举抛 invalid_argument
		 */
		void setServerHttpProtocolMode(HttpProtocolMode mode);

		/**
		 * @brief 启动前应用并冻结服务端 ALPN 和 H2 TLS 配置，重复调用无副作用
		 * 所有 SSL 对象必须在本调用后创建；冻结后不再通过 native() 改写协议或 TLS 参数。
		 * H2 配置失败抛 runtime_error；仅 H2 模式仍须在握手后检查协商结果。
		 */
		void freezeServerHttpProtocolConfig();

		/**
		 * @brief 获取服务端协议策略
		 * @return 配置的协议策略，冻结后只读
		 */
		[[nodiscard]] HttpProtocolMode serverHttpProtocolMode() const noexcept;

		/**
		 * @brief 查询服务端协议配置是否已冻结
		 * @return 已应用配置时返回 true
		 */
		[[nodiscard]] bool serverHttpProtocolConfigFrozen() const noexcept;

		/**
		 * @brief 在客户端单个 SSL 对象上设置 ALPN 列表和 H2 TLS 参数，不修改共享 context
		 * @param ssl 握手前的有效 SSL 对象，由调用方持有
		 * @param mode H1、优先 H2 并允许 H1，或仅 H2
		 * @return 配置错误；成功不代表 TLS 或协议协商已完成
		 */
		[[nodiscard]] static NetworkError configureClientHttpProtocols(SSL* ssl, HttpProtocolMode mode);

		/**
		 * @brief TLS 握手完成后读取协议并检查策略；仅 H2 时缺少 h2 明确失败
		 * @param ssl 完成握手的 SSL 对象，由调用方持有
		 * @param mode 该连接的协议策略
		 * @param protocol 输出实际协议；错误时保持 H1，调用方必须先检查返回错误
		 * @return 协商或 TLS 参数错误；自动模式的无 ALPN 结果回退 H1
		 */
		[[nodiscard]] static NetworkError negotiatedHttpProtocol(SSL* ssl,
																 HttpProtocolMode mode,
																 HttpProtocol& protocol);
#endif

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
#ifdef HICAL_HAS_HTTP2
		HttpProtocolMode serverHttpProtocolMode_ {HttpProtocolMode::hHttp1Only};
		bool serverHttpProtocolConfigFrozen_ {false};
#endif
	};

} // namespace hical
