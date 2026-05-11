#pragma once

#include "FixedBuffer.h"
#include "HeaderMap.h"
#include "HttpRequest.h"
#include "WebSocket.h"
#include <charconv>
#include <cstdint>
#include <openssl/evp.h>
#include <string>
#include <string_view>

namespace hical
{

	// permessage-deflate 协商结果
	struct WsDeflateNegotiation
	{
		bool accepted = false;
		int serverMaxWindowBits = 15;
		int clientMaxWindowBits = 15;
		bool serverNoContextTakeover = false;
		bool clientNoContextTakeover = false;
	};

	/**
	 * @brief 标准 Base64 编码（RFC 4648）
	 * @param data 输入数据
	 * @param len  输入长度
	 * @return Base64 编码字符串
	 */
	inline std::string base64Encode(const uint8_t* data, size_t len)
	{
		static constexpr char kTable[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

		std::string result;
		result.reserve(((len + 2) / 3) * 4);

		size_t i = 0;
		for (; i + 2 < len; i += 3)
		{
			uint32_t triplet = (static_cast<uint32_t>(data[i]) << 16) | (static_cast<uint32_t>(data[i + 1]) << 8)
							   | static_cast<uint32_t>(data[i + 2]);
			result.push_back(kTable[(triplet >> 18) & 0x3F]);
			result.push_back(kTable[(triplet >> 12) & 0x3F]);
			result.push_back(kTable[(triplet >> 6) & 0x3F]);
			result.push_back(kTable[triplet & 0x3F]);
		}

		if (i + 1 == len)
		{
			uint32_t val = static_cast<uint32_t>(data[i]) << 16;
			result.push_back(kTable[(val >> 18) & 0x3F]);
			result.push_back(kTable[(val >> 12) & 0x3F]);
			result.push_back('=');
			result.push_back('=');
		}
		else if (i + 2 == len)
		{
			uint32_t val = (static_cast<uint32_t>(data[i]) << 16) | (static_cast<uint32_t>(data[i + 1]) << 8);
			result.push_back(kTable[(val >> 18) & 0x3F]);
			result.push_back(kTable[(val >> 12) & 0x3F]);
			result.push_back(kTable[(val >> 6) & 0x3F]);
			result.push_back('=');
		}

		return result;
	}

	/**
	 * @brief 计算 Sec-WebSocket-Accept（RFC 6455 §4.2.2）
	 * Accept = Base64( SHA1( clientKey + "258EAFA5-E914-47DA-95CA-5AB5DC65C174" ) )
	 * 测试向量：computeWsAcceptKey("dGhlIHNhbXBsZSBub25jZQ==") == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo="
	 * @param clientKey 客户端提供的 Sec-WebSocket-Key 值
	 * @return Sec-WebSocket-Accept 值
	 */
	inline std::string computeWsAcceptKey(std::string_view clientKey)
	{
		static constexpr std::string_view kGuid = "258EAFA5-E914-47DA-95CA-5AB5DC65C174";

		// 拼接 clientKey + GUID
		std::string concat;
		concat.reserve(clientKey.size() + kGuid.size());
		concat.append(clientKey);
		concat.append(kGuid);

		// SHA1
		uint8_t hash[EVP_MAX_MD_SIZE];
		unsigned int hashLen = 0;
		EVP_Digest(concat.data(), concat.size(), hash, &hashLen, EVP_sha1(), nullptr);

		return base64Encode(hash, hashLen);
	}

	/**
	 * @brief 验证 WebSocket 升级请求头部（RFC 6455 §4.2.1）
	 * 前置条件：NativeRequest::isUpgrade() 已返回 true（Connection: Upgrade + Upgrade: websocket 已检查）。
	 * 本函数额外验证 Sec-WebSocket-Version 和 Sec-WebSocket-Key。
	 * @param req 已解析的 NativeRequest
	 * @return Sec-WebSocket-Key 值（成功时非空），验证失败时返回空 string_view
	 */
	inline std::string_view validateWsUpgrade(const NativeRequest& req)
	{
		// 检查 Sec-WebSocket-Version: 13
		auto version = req.headers.find("Sec-WebSocket-Version");
		if (version != "13")
		{
			return {};
		}

		// 检查 Sec-WebSocket-Key 存在且为 24 字符 Base64（RFC 6455 §4.2.1：16 字节随机值的 Base64 编码）
		auto key = req.headers.find("Sec-WebSocket-Key");
		if (key.empty() || key.size() != 24)
		{
			return {};
		}

		return key;
	}

	namespace detail
	{

		// 辅助：trim 前后空格
		inline std::string_view trimWs(std::string_view sv)
		{
			while (!sv.empty() && sv.front() == ' ')
			{
				sv.remove_prefix(1);
			}
			while (!sv.empty() && sv.back() == ' ')
			{
				sv.remove_suffix(1);
			}
			return sv;
		}

		// 辅助：从 "param=value" 中提取 value（不含空格），param 不含 '=' 时返回空
		inline std::string_view extractParamValue(std::string_view token)
		{
			auto eq = token.find('=');
			if (eq == std::string_view::npos)
			{
				return {};
			}
			return trimWs(token.substr(eq + 1));
		}

	} // namespace detail

	/**
	 * @brief 协商 permessage-deflate 扩展（RFC 7692）
	 * 解析客户端 Sec-WebSocket-Extensions 头部中的 permessage-deflate offer，
	 * 与服务端配置取交集。
	 * @param extHeader  客户端 Sec-WebSocket-Extensions 头部值
	 * @param serverCfg  服务端压缩配置
	 * @return 协商结果，accepted=true 表示压缩可用
	 */
	inline WsDeflateNegotiation negotiateDeflate(std::string_view extHeader, const WsCompressionConfig& serverCfg)
	{
		WsDeflateNegotiation result;
		if (extHeader.empty() || !serverCfg.enabled)
		{
			return result;
		}

		// 按逗号分割多个扩展 offer，找到 permessage-deflate
		// 注意：扩展名和参数之间用分号分隔，多个扩展之间用逗号分隔
		// 例如："permessage-deflate; server_max_window_bits=10, another-ext"
		// 难点：参数值本身不含逗号，所以逗号作为顶层分隔符是安全的

		std::string_view remaining = extHeader;
		while (!remaining.empty())
		{
			// 取一个扩展 offer（逗号分割）
			auto comma = remaining.find(',');
			auto offer = (comma != std::string_view::npos) ? remaining.substr(0, comma) : remaining;
			remaining = (comma != std::string_view::npos) ? remaining.substr(comma + 1) : std::string_view {};

			offer = detail::trimWs(offer);

			// 检查此 offer 是否以 "permessage-deflate" 开头
			auto semi = offer.find(';');
			auto extName = detail::trimWs((semi != std::string_view::npos) ? offer.substr(0, semi) : offer);

			if (!HeaderMap::iequals(extName, "permessage-deflate"))
			{
				continue;
			}

			// 找到 permessage-deflate，解析参数
			result.accepted = true;
			result.serverMaxWindowBits = serverCfg.serverMaxWindowBits;
			result.clientMaxWindowBits = serverCfg.clientMaxWindowBits;
			result.serverNoContextTakeover = serverCfg.serverNoContextTakeover;

			if (semi == std::string_view::npos)
			{
				break; // 无参数
			}

			auto params = offer.substr(semi + 1);
			while (!params.empty())
			{
				auto nextSemi = params.find(';');
				auto token = detail::trimWs((nextSemi != std::string_view::npos) ? params.substr(0, nextSemi) : params);
				params = (nextSemi != std::string_view::npos) ? params.substr(nextSemi + 1) : std::string_view {};

				if (token.starts_with("server_max_window_bits"))
				{
					auto val = detail::extractParamValue(token);
					if (!val.empty())
					{
						int bits = 0;
						auto [ptr, ec] = std::from_chars(val.data(), val.data() + val.size(), bits);
						if (ec == std::errc {} && bits >= 8 && bits <= 15)
						{
							result.serverMaxWindowBits = std::min(bits, serverCfg.serverMaxWindowBits);
						}
					}
				}
				else if (token.starts_with("client_max_window_bits"))
				{
					auto val = detail::extractParamValue(token);
					if (!val.empty())
					{
						int bits = 0;
						auto [ptr, ec] = std::from_chars(val.data(), val.data() + val.size(), bits);
						if (ec == std::errc {} && bits >= 8 && bits <= 15)
						{
							result.clientMaxWindowBits = std::min(bits, serverCfg.clientMaxWindowBits);
						}
					}
					// 无值（"client_max_window_bits" 不带 =N）：使用服务端配置值
				}
				else if (token == "server_no_context_takeover")
				{
					result.serverNoContextTakeover = true;
				}
				else if (token == "client_no_context_takeover")
				{
					result.clientNoContextTakeover = true;
				}
			}
			break; // 只处理第一个匹配的 offer
		}

		return result;
	}

	/**
	 * @brief 构建 WebSocket 101 Switching Protocols 响应
	 * 输出到 FixedBuffer<512>（栈上零堆分配），格式完全符合 RFC 6455 §4.2.2。
	 * @param buf        输出缓冲区
	 * @param acceptKey  Sec-WebSocket-Accept 值
	 * @param deflateNeg permessage-deflate 协商结果（nullptr 或 accepted=false 时跳过扩展头）
	 */
	inline void buildWsAcceptResponse(FixedBuffer<512>& buf,
									  std::string_view acceptKey,
									  const WsDeflateNegotiation* deflateNeg = nullptr)
	{
		buf << "HTTP/1.1 101 Switching Protocols\r\n"
			<< "Upgrade: websocket\r\n"
			<< "Connection: Upgrade\r\n"
			<< "Sec-WebSocket-Accept: " << acceptKey << "\r\n";

		if (deflateNeg != nullptr && deflateNeg->accepted)
		{
			buf << "Sec-WebSocket-Extensions: permessage-deflate"
				<< "; server_max_window_bits=" << deflateNeg->serverMaxWindowBits
				<< "; client_max_window_bits=" << deflateNeg->clientMaxWindowBits;

			if (deflateNeg->serverNoContextTakeover)
			{
				buf << "; server_no_context_takeover";
			}
			if (deflateNeg->clientNoContextTakeover)
			{
				buf << "; client_no_context_takeover";
			}
			buf << "\r\n";
		}

		buf << "\r\n";
	}

} // namespace hical
