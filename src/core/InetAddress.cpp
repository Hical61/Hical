/**
 * @file InetAddress.cpp
 * @brief 网络地址封装实现
 */

#include "InetAddress.h"
#include <cstring>

#ifdef _WIN32
	#include <winsock2.h>
	#include <ws2tcpip.h>
#else
	#include <arpa/inet.h>
#endif

namespace hical
{

	InetAddress::InetAddress(const std::string& ip, uint16_t port)
	{
		std::memset(&addr_, 0, sizeof(addr_));

		// 尝试解析为 IPv4
		if (inet_pton(AF_INET, ip.c_str(), &addr_.sin_addr) == 1)
		{
			addr_.sin_family = AF_INET;
			addr_.sin_port = htons(port);
			isIpV6_ = false;
		}
		else
		{
			// 尝试解析为 IPv6
			std::memset(&addr6_, 0, sizeof(addr6_));
			if (inet_pton(AF_INET6, ip.c_str(), &addr6_.sin6_addr) == 1)
			{
				addr6_.sin6_family = AF_INET6;
				addr6_.sin6_port = htons(port);
				isIpV6_ = true;
			}
		}
	}

	InetAddress::InetAddress(const struct sockaddr_in& addr)
	{
		addr_ = addr;
		isIpV6_ = false;
	}

	InetAddress::InetAddress(const struct sockaddr_in6& addr)
	{
		addr6_ = addr;
		isIpV6_ = true;
	}

	std::string InetAddress::toIp() const
	{
		char buf[INET6_ADDRSTRLEN];
		if (isIpV6_)
		{
			inet_ntop(AF_INET6, &addr6_.sin6_addr, buf, sizeof(buf));
		}
		else
		{
			inet_ntop(AF_INET, &addr_.sin_addr, buf, sizeof(buf));
		}
		return buf;
	}

	std::string InetAddress::toIpPort() const
	{
		return toIp() + ":" + std::to_string(port());
	}

	uint16_t InetAddress::port() const
	{
		return ntohs(isIpV6_ ? addr6_.sin6_port : addr_.sin_port);
	}

	bool InetAddress::isIpV6() const
	{
		return isIpV6_;
	}

	const struct sockaddr* InetAddress::getSockAddr() const
	{
		return isIpV6_ ? reinterpret_cast<const struct sockaddr*>(&addr6_)
					   : reinterpret_cast<const struct sockaddr*>(&addr_);
	}

	void InetAddress::setSockAddrInet4(const struct sockaddr_in& addr)
	{
		addr_ = addr;
		isIpV6_ = false;
	}

	void InetAddress::setSockAddrInet6(const struct sockaddr_in6& addr)
	{
		addr6_ = addr;
		isIpV6_ = true;
	}

} // namespace hical
