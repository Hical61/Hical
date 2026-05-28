/**
 * @file SslConnection.h
 * @brief SSL/TLS 加密连接类型别名
 * 仅在需要 SSL 连接时包含此文件，避免非 SSL 场景拉入 OpenSSL 重量级头文件。
 * 普通 TCP 连接请使用 GenericConnection.h 中的 PlainConnection。
 */

#pragma once

#include "GenericConnection.h"
#include <boost/asio/ssl.hpp>

namespace hical
{

	/** SSL/TLS 加密连接 */
	using SslConnection = GenericConnection<boost::asio::ssl::stream<boost::asio::ip::tcp::socket>>;

} // namespace hical
