#include "GenericConnection.h"
#include "AsioEventLoop.h"

namespace hical
{

// 显式实例化普通 TCP 连接
template class GenericConnection<boost::asio::ip::tcp::socket>;

// 显式实例化 SSL 连接
template class GenericConnection<
    boost::asio::ssl::stream<boost::asio::ip::tcp::socket>>;

}  // namespace hical
