/**
 * @file GenericConnection.cpp
 * @brief GenericConnection 显式模板实例化
 */

#include "GenericConnection.h"
#include "SslConnection.h"
#include "AsioEventLoop.h"

// 引入模板方法实现（仅此文件需要完整定义）
#define HICAL_BUILDING_GENERIC_CONNECTION
#include "GenericConnection.hci"

namespace hical
{

	// 显式实例化普通 TCP 连接
	template class GenericConnection<boost::asio::ip::tcp::socket>;

	// 显式实例化 SSL 连接
	template class GenericConnection<boost::asio::ssl::stream<boost::asio::ip::tcp::socket>>;

} // namespace hical
