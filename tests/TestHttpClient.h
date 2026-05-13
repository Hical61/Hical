#pragma once

/**
 * @file TestHttpClient.h
 * @brief 自研轻量级测试客户端（HTTP + WebSocket）
 * 轻量级测试客户端（HTTP + WebSocket），使用 raw socket + picohttpparser 解析响应。
 * 仅用于测试，不追求工业级健壮性。
 */

#include "core/WsFrame.h"
#include <boost/asio.hpp>
#include <charconv>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

// picohttpparser（C 库）
extern "C"
{
#include "picohttpparser.h"
}

namespace hical::test
{

	using boost::asio::ip::tcp;

	namespace detail
	{

		/// 辅助：读取直到 buf 中包含 "\r\n\r\n"，返回 header_end 位置
		inline size_t readUntilHeaderEnd(tcp::socket& sock, std::string& buf)
		{
			for (;;)
			{
				auto pos = buf.find("\r\n\r\n");
				if (pos != std::string::npos)
				{
					return pos + 4;
				}
				char tmp[4096];
				auto n = sock.read_some(boost::asio::buffer(tmp));
				buf.append(tmp, n);
			}
		}

		struct ParsedResponse
		{
			unsigned status = 0;
			std::string body;
			// 简易头部存储
			std::vector<std::pair<std::string, std::string>> headers;

			std::string findHeader(const std::string& name) const
			{
				for (const auto& [k, v] : headers)
				{
					if (k.size() == name.size())
					{
						bool match = true;
						for (size_t i = 0; i < k.size(); ++i)
						{
							if (std::tolower(static_cast<unsigned char>(k[i]))
								!= std::tolower(static_cast<unsigned char>(name[i])))
							{
								match = false;
								break;
							}
						}
						if (match)
						{
							return v;
						}
					}
				}
				return {};
			}
		};

		/// 从 socket 读取完整 HTTP 响应（头部 + body）
		inline ParsedResponse readHttpResponse(tcp::socket& sock, std::string& residual)
		{
			// 读取到头部结束
			size_t headerEnd = 0;
			for (;;)
			{
				auto pos = residual.find("\r\n\r\n");
				if (pos != std::string::npos)
				{
					headerEnd = pos + 4;
					break;
				}
				char tmp[4096];
				auto n = sock.read_some(boost::asio::buffer(tmp));
				residual.append(tmp, n);
			}

			// 解析响应头（使用 picohttpparser）
			int minorVersion = 0;
			int status = 0;
			const char* msg = nullptr;
			size_t msgLen = 0;
			struct phr_header headers[64];
			size_t numHeaders = 64;

			int ret = phr_parse_response(residual.data(),
										 headerEnd,
										 &minorVersion,
										 &status,
										 &msg,
										 &msgLen,
										 headers,
										 &numHeaders,
										 0);

			if (ret < 0)
			{
				throw std::runtime_error("Failed to parse HTTP response");
			}

			ParsedResponse result;
			result.status = static_cast<unsigned>(status);

			size_t contentLength = 0;
			bool isChunked = false;

			for (size_t i = 0; i < numHeaders; ++i)
			{
				std::string name(headers[i].name, headers[i].name_len);
				std::string value(headers[i].value, headers[i].value_len);
				result.headers.emplace_back(name, value);

				// 检测 Content-Length
				if (name.size() == 14)
				{
					bool match = true;
					const char* cl = "content-length";
					for (size_t j = 0; j < 14; ++j)
					{
						if (std::tolower(static_cast<unsigned char>(name[j])) != cl[j])
						{
							match = false;
							break;
						}
					}
					if (match)
					{
						std::from_chars(value.data(), value.data() + value.size(), contentLength);
					}
				}
				// 检测 Transfer-Encoding: chunked
				if (name.size() == 17)
				{
					bool match = true;
					const char* te = "transfer-encoding";
					for (size_t j = 0; j < 17; ++j)
					{
						if (std::tolower(static_cast<unsigned char>(name[j])) != te[j])
						{
							match = false;
							break;
						}
					}
					if (match && value.find("chunked") != std::string::npos)
					{
						isChunked = true;
					}
				}
			}

			// 读取 body
			std::string bodyData;
			size_t residualBody = residual.size() - headerEnd;

			if (!isChunked && contentLength > 0)
			{
				bodyData.append(residual.data() + headerEnd, residualBody);
				while (bodyData.size() < contentLength)
				{
					char tmp[4096];
					auto n = sock.read_some(boost::asio::buffer(tmp));
					bodyData.append(tmp, n);
				}
				bodyData.resize(contentLength);
			}
			else if (!isChunked)
			{
				// Connection: close 模式或无 body
				bodyData.append(residual.data() + headerEnd, residualBody);
			}

			result.body = std::move(bodyData);

			// 保留 body 之后的残余数据（keep-alive 场景下可能包含下一个响应的开头）
			if (!isChunked && contentLength > 0)
			{
				size_t totalConsumed = headerEnd + contentLength;
				if (totalConsumed < residual.size())
				{
					residual = residual.substr(totalConsumed);
				}
				else
				{
					residual.clear();
				}
			}
			else
			{
				residual.clear();
			}
			return result;
		}

	} // namespace detail

	/**
	 * @brief 发送 HTTP GET 请求并返回 {状态码, 响应体}
	 */
	inline std::pair<unsigned, std::string> httpGet(const std::string& host, uint16_t port, const std::string& target)
	{
		boost::asio::io_context ioCtx;
		tcp::socket sock(ioCtx);
		sock.connect(tcp::endpoint(boost::asio::ip::make_address(host), port));

		std::string req = "GET " + target
						  + " HTTP/1.1\r\n"
							"Host: "
						  + host
						  + "\r\n"
							"Connection: close\r\n"
							"\r\n";
		boost::asio::write(sock, boost::asio::buffer(req));

		std::string buf;
		auto result = detail::readHttpResponse(sock, buf);

		boost::system::error_code ec;
		sock.shutdown(tcp::socket::shutdown_both, ec);
		return {result.status, result.body};
	}

	/**
	 * @brief 发送 HTTP GET 请求并返回完整 ParsedResponse（含头部）
	 */
	inline detail::ParsedResponse httpGetFull(const std::string& host, uint16_t port, const std::string& target)
	{
		boost::asio::io_context ioCtx;
		tcp::socket sock(ioCtx);
		sock.connect(tcp::endpoint(boost::asio::ip::make_address(host), port));

		std::string req = "GET " + target
						  + " HTTP/1.1\r\n"
							"Host: "
						  + host
						  + "\r\n"
							"Connection: close\r\n"
							"\r\n";
		boost::asio::write(sock, boost::asio::buffer(req));

		std::string buf;
		auto result = detail::readHttpResponse(sock, buf);

		boost::system::error_code ec;
		sock.shutdown(tcp::socket::shutdown_both, ec);
		return result;
	}

	/**
	 * @brief 发送 HTTP POST 请求并返回 {状态码, 响应体}
	 */
	inline std::pair<unsigned, std::string> httpPost(const std::string& host,
													 uint16_t port,
													 const std::string& target,
													 const std::string& body)
	{
		boost::asio::io_context ioCtx;
		tcp::socket sock(ioCtx);
		sock.connect(tcp::endpoint(boost::asio::ip::make_address(host), port));

		std::string clStr = std::to_string(body.size());
		std::string req = "POST " + target
						  + " HTTP/1.1\r\n"
							"Host: "
						  + host
						  + "\r\n"
							"Content-Length: "
						  + clStr
						  + "\r\n"
							"Connection: close\r\n"
							"\r\n"
						  + body;
		boost::asio::write(sock, boost::asio::buffer(req));

		std::string buf;
		auto result = detail::readHttpResponse(sock, buf);

		boost::system::error_code ec;
		sock.shutdown(tcp::socket::shutdown_both, ec);
		return {result.status, result.body};
	}

	/**
	 * @brief HTTP Pipelining 测试：一次 TCP write 发送 N 个请求，然后顺序读取 N 个响应
	 * 与 httpKeepAliveRequests 不同，这里所有请求在一次 write 中发送（不等待中间响应）。
	 */
	inline std::vector<detail::ParsedResponse> httpPipelinedRequests(const std::string& host,
																	 uint16_t port,
																	 const std::vector<std::string>& targets)
	{
		boost::asio::io_context ioCtx;
		tcp::socket sock(ioCtx);
		sock.connect(tcp::endpoint(boost::asio::ip::make_address(host), port));

		// 将所有请求拼接到一个缓冲区，一次性发送
		std::string allRequests;
		for (size_t i = 0; i < targets.size(); ++i)
		{
			bool isLast = (i == targets.size() - 1);
			allRequests += "GET " + targets[i]
						   + " HTTP/1.1\r\n"
							 "Host: "
						   + host
						   + "\r\n"
							 "Connection: "
						   + (isLast ? "close" : "keep-alive")
						   + "\r\n"
							 "\r\n";
		}

		boost::asio::write(sock, boost::asio::buffer(allRequests));

		// 顺序读取所有响应
		std::vector<detail::ParsedResponse> results;
		std::string buf;
		for (size_t i = 0; i < targets.size(); ++i)
		{
			results.push_back(detail::readHttpResponse(sock, buf));
		}

		boost::system::error_code ec;
		sock.shutdown(tcp::socket::shutdown_both, ec);
		return results;
	}

	/**
	 * @brief 在同一连接上发送多个 GET 请求（Keep-Alive）
	 */
	inline std::vector<detail::ParsedResponse> httpKeepAliveRequests(
		const std::string& host,
		uint16_t port,
		const std::vector<std::pair<std::string, std::string>>& requests) // {method, target}
	{
		boost::asio::io_context ioCtx;
		tcp::socket sock(ioCtx);
		sock.connect(tcp::endpoint(boost::asio::ip::make_address(host), port));

		std::vector<detail::ParsedResponse> results;
		std::string buf;

		for (size_t i = 0; i < requests.size(); ++i)
		{
			bool isLast = (i == requests.size() - 1);
			auto& [method, target] = requests[i];

			std::string req = method + " " + target
							  + " HTTP/1.1\r\n"
								"Host: "
							  + host
							  + "\r\n"
								"Connection: "
							  + (isLast ? "close" : "keep-alive")
							  + "\r\n"
								"\r\n";
			boost::asio::write(sock, boost::asio::buffer(req));

			results.push_back(detail::readHttpResponse(sock, buf));
		}

		return results;
	}

	/**
	 * @brief 轻量级 WebSocket 测试客户端
	 * 使用 raw socket 实现 WS 握手和帧收发。
	 */
	class TestWsClient
	{
	public:
		explicit TestWsClient(boost::asio::io_context& ioc) : m_socket(ioc)
		{
		}

		/**
		 * @brief TCP 连接 + HTTP 升级握手
		 */
		void connect(const std::string& host, uint16_t port, const std::string& path)
		{
			m_socket.connect(tcp::endpoint(boost::asio::ip::make_address(host), port));

			// 固定的 Sec-WebSocket-Key（测试用，不需要随机）
			static constexpr const char* kTestKey = "dGVzdC13ZWJzb2NrZXQta2V5";

			std::string req = "GET " + path
							  + " HTTP/1.1\r\n"
								"Host: "
							  + host + ":" + std::to_string(port)
							  + "\r\n"
								"Upgrade: websocket\r\n"
								"Connection: Upgrade\r\n"
								"Sec-WebSocket-Version: 13\r\n"
								"Sec-WebSocket-Key: "
							  + kTestKey
							  + "\r\n"
								"\r\n";

			boost::asio::write(m_socket, boost::asio::buffer(req));

			// 读取 101 响应
			std::string buf;
			detail::readUntilHeaderEnd(m_socket, buf);

			// 简单验证包含 "101"
			if (buf.find("101") == std::string::npos)
			{
				throw std::runtime_error("WebSocket handshake failed: " + buf.substr(0, 80));
			}

			// 保存 101 响应后的残余数据
			auto headerEnd = buf.find("\r\n\r\n") + 4;
			if (headerEnd < buf.size())
			{
				m_readBuf.assign(buf.begin() + static_cast<std::string::difference_type>(headerEnd), buf.end());
			}
		}

		/**
		 * @brief 发送文本消息（客户端必须 mask）
		 */
		void write(const std::string& msg)
		{
			// 测试用固定 mask key
			static constexpr uint8_t kMaskKey[4] = {0x12, 0x34, 0x56, 0x78};
			auto frame = hical::buildMaskedWsFrame(hical::WsOpcode::hText, msg, kMaskKey);
			boost::asio::write(m_socket, boost::asio::buffer(frame));
		}

		/**
		 * @brief 同步读取一条完整消息
		 */
		std::string read()
		{
			for (;;)
			{
				auto hdr = tryParseFrame();
				if (hdr)
				{
					size_t frameSize = hdr->headerSize + static_cast<size_t>(hdr->payloadLength);
					if (m_readBuf.size() >= frameSize)
					{
						// 提取载荷（服务端发送不 mask，无需 unmask）
						std::string payload(m_readBuf.data() + hdr->headerSize,
											m_readBuf.data() + hdr->headerSize
												+ static_cast<size_t>(hdr->payloadLength));

						m_readBuf.erase(m_readBuf.begin(), m_readBuf.begin() + static_cast<long>(frameSize));
						return payload;
					}
				}

				// 需要更多数据
				char tmp[4096];
				auto n = m_socket.read_some(boost::asio::buffer(tmp));
				m_readBuf.append(tmp, n);
			}
		}

		/**
		 * @brief 带 error_code 版本的 read（超时/连接关闭检测用）
		 */
		std::string read(boost::system::error_code& ec)
		{
			try
			{
				return read();
			}
			catch (const boost::system::system_error& e)
			{
				ec = e.code();
				return {};
			}
		}

		/**
		 * @brief 发送 close 帧
		 */
		void close(hical::WsCloseCode code = hical::WsCloseCode::hNormal)
		{
			auto closePayload = hical::buildClosePayload(code);
			static constexpr uint8_t kMaskKey[4] = {0xAA, 0xBB, 0xCC, 0xDD};
			auto frame = hical::buildMaskedWsFrame(hical::WsOpcode::hClose, closePayload, kMaskKey);

			boost::system::error_code ec;
			boost::asio::write(m_socket, boost::asio::buffer(frame), ec);
		}

		tcp::socket& socket()
		{
			return m_socket;
		}

	private:
		std::optional<hical::WsFrameHeader> tryParseFrame()
		{
			if (m_readBuf.size() < 2)
			{
				return std::nullopt;
			}
			return hical::parseWsFrameHeader(reinterpret_cast<const uint8_t*>(m_readBuf.data()), m_readBuf.size());
		}

		tcp::socket m_socket;
		std::string m_readBuf;
	};

} // namespace hical::test
