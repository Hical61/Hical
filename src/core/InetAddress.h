/**
 * @file InetAddress.h
 * @brief 网络地址封装
 */

#pragma once

#include <cstdint>
#include <cstring>
#include <string>

#ifdef _WIN32
	#include <winsock2.h>
	#include <ws2tcpip.h>
#else
	#include <netinet/in.h>
	#include <sys/socket.h>
#endif

namespace hical
{

	/**
	 * @brief 网络地址封装
	 * 封装 IPv4/IPv6 地址和端口
	 */
	class InetAddress
	{
	public:
		InetAddress()
		{
			std::memset(&addr6_, 0, sizeof(addr6_));
		}

		/**
		 * @brief 构造函数（IPv4）
		 * @param ip IP 地址字符串
		 * @param port 端口号
		 */
		InetAddress(const std::string& ip, uint16_t port);

		/**
		 * @brief 构造函数（从 sockaddr）
		 * @param addr sockaddr 结构
		 */
		explicit InetAddress(const struct sockaddr_in& addr);
		explicit InetAddress(const struct sockaddr_in6& addr);

		/**
		 * @brief 获取 IP 地址字符串
		 * @return IP 地址
		 */
		std::string toIp() const;

		/**
		 * @brief 获取 IP:Port 字符串
		 * @return IP:Port
		 */
		std::string toIpPort() const;

		/**
		 * @brief 获取端口号
		 * @return 端口号
		 */
		uint16_t port() const;

		/**
		 * @brief 是否为 IPv6 地址
		 * @return true 如果是 IPv6
		 */
		bool isIpV6() const;

		/**
		 * @brief 获取底层 sockaddr 结构
		 * @return sockaddr 指针
		 */
		const struct sockaddr* getSockAddr() const;

		/**
		 * @brief 设置底层 sockaddr 结构
		 * @param addr sockaddr 结构
		 */
		void setSockAddrInet4(const struct sockaddr_in& addr);
		void setSockAddrInet6(const struct sockaddr_in6& addr);

	private:
		union
		{
			struct sockaddr_in addr_;
			struct sockaddr_in6 addr6_;
		};

		bool isIpV6_ {false};
	};

} // namespace hical
