#pragma once

/**
 * @brief 向后兼容头文件
 *
 * AsioTcpConnection 现在是 GenericConnection<tcp::socket> 的类型别名。
 * 包含此头文件即可获得 AsioTcpConnection / PlainConnection / SslConnection 三个类型。
 */
#include "GenericConnection.h"
