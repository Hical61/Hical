/**
 * @file JwtAuth.cpp
 * @brief JWT 认证中间件实现（HS256 签发/验证 + SyncBeforeHandler）
 */

#include "core/JwtAuth.h"
#include "core/Log.h"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>

namespace hical
{

	// ============== Base64URL 编解码 ==============

	namespace
	{

		/// Base64 编码表（标准）
		constexpr char kBase64Table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

		/**
		 * @brief Base64URL 编码（URL-safe，无填充）
		 * RFC 7515：JWT 使用 Base64URL 编码，将 + 替换为 -，/ 替换为 _，去掉末尾 =
		 */
		[[nodiscard]] std::string base64UrlEncode(std::string_view data)
		{
			std::string result;
			size_t outLen = 4 * ((data.size() + 2) / 3);
			result.reserve(outLen);

			for (size_t i = 0; i < data.size(); i += 3)
			{
				auto a = static_cast<uint8_t>(data[i]);
				auto b = (i + 1 < data.size()) ? static_cast<uint8_t>(data[i + 1]) : uint8_t(0);
				auto c = (i + 2 < data.size()) ? static_cast<uint8_t>(data[i + 2]) : uint8_t(0);

				result.push_back(kBase64Table[(a >> 2) & 0x3F]);
				result.push_back(kBase64Table[((a << 4) | (b >> 4)) & 0x3F]);
				result.push_back((i + 1 < data.size()) ? kBase64Table[((b << 2) | (c >> 6)) & 0x3F] : '=');
				result.push_back((i + 2 < data.size()) ? kBase64Table[c & 0x3F] : '=');
			}

			// URL-safe 转换：+ → -，/ → _
			for (auto& ch : result)
			{
				if (ch == '+')
				{
					ch = '-';
				}
				else if (ch == '/')
				{
					ch = '_';
				}
			}

			// 去掉末尾填充 =
			while (!result.empty() && result.back() == '=')
			{
				result.pop_back();
			}

			return result;
		}

		/**
		 * @brief Base64URL 解码
		 * 先将 URL-safe 字符还原为标准 Base64，补齐 = 后解码。
		 */
		[[nodiscard]] std::string base64UrlDecode(std::string_view input)
		{
			std::string normalized;
			normalized.reserve(input.size() + 3);
			normalized = input;

			// URL-safe → 标准 Base64
			for (auto& ch : normalized)
			{
				if (ch == '-')
				{
					ch = '+';
				}
				else if (ch == '_')
				{
					ch = '/';
				}
			}

			// 补齐 = 到 4 的倍数
			while (normalized.size() % 4 != 0)
			{
				normalized.push_back('=');
			}

			std::string result;
			result.reserve((normalized.size() / 4) * 3);

			// 标准 Base64 解码表（反向查找）
			// clang-format off
	static constexpr int8_t kDecodeTable[256] = {
		-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
		-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
		-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 62, -1, -1, -1, 63,
		52, 53, 54, 55, 56, 57, 58, 59, 60, 61, -1, -1, -1, -1, -1, -1,
		-1,  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14,
		15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, -1, -1, -1, -1, -1,
		-1, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40,
		41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, -1, -1, -1, -1, -1,
		-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
		-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
		-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
		-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
		-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
		-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
		-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
		-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
	};
			// clang-format on

			for (size_t i = 0; i < normalized.size(); i += 4)
			{
				auto v0 = kDecodeTable[static_cast<uint8_t>(normalized[i])];
				auto v1 = kDecodeTable[static_cast<uint8_t>(normalized[i + 1])];
				auto v2 = kDecodeTable[static_cast<uint8_t>(normalized[i + 2])];
				auto v3 = kDecodeTable[static_cast<uint8_t>(normalized[i + 3])];

				if (v0 < 0 || v1 < 0)
				{
					throw std::runtime_error("JWT: invalid base64 encoding");
				}

				result.push_back(static_cast<char>((v0 << 2) | (v1 >> 4)));

				if (v2 >= 0)
				{
					result.push_back(static_cast<char>((v1 << 4) | (v2 >> 2)));
				}
				if (v3 >= 0)
				{
					result.push_back(static_cast<char>((v2 << 6) | v3));
				}
			}

			return result;
		}

	} // namespace

	// ============== HMAC-SHA256 ==============

	namespace
	{

		/// SHA-256 输出长度（字节）
		constexpr size_t kSha256OutputLen = 32;

		/**
		 * @brief 计算 HMAC-SHA256
		 * @param data 待签名数据
		 * @param key 密钥
		 * @return 32 字节的 HMAC 结果（原始二进制）
		 */
		[[nodiscard]] std::array<uint8_t, kSha256OutputLen> hmacSha256(std::string_view data, std::string_view key)
		{
			std::array<uint8_t, kSha256OutputLen> result {};
			unsigned int len = kSha256OutputLen;

			HMAC(EVP_sha256(),
				 key.data(),
				 static_cast<int>(key.size()),
				 reinterpret_cast<const unsigned char*>(data.data()),
				 data.size(),
				 result.data(),
				 &len);

			return result;
		}

	} // namespace

	// ============== JWT 签发 ==============

	std::string jwtSign(const boost::json::object& payload, const JwtAuthOptions& opts)
	{
		// 构建完整 payload：用户 claims + 自动 iat/exp + 可选 iss/aud
		// iat/exp 仅在用户未设置时写入，尊重用户自定义值
		boost::json::object fullPayload = payload;

		auto now = std::chrono::system_clock::now();
		auto nowSec = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();

		if (!fullPayload.contains("iat"))
		{
			fullPayload["iat"] = static_cast<int64_t>(nowSec);
		}
		if (!fullPayload.contains("exp"))
		{
			auto expSec =
				std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch() + opts.tokenExpiry).count();
			fullPayload["exp"] = static_cast<int64_t>(expSec);
		}

		if (!opts.issuer.empty() && !fullPayload.contains("iss"))
		{
			fullPayload["iss"] = opts.issuer;
		}
		if (!opts.audience.empty() && !fullPayload.contains("aud"))
		{
			fullPayload["aud"] = opts.audience;
		}

		// Header: {"alg":"HS256","typ":"JWT"}
		boost::json::object header;
		header["alg"] = "HS256";
		header["typ"] = "JWT";

		auto headerJson = boost::json::serialize(header);
		auto payloadJson = boost::json::serialize(fullPayload);

		auto headerB64 = base64UrlEncode(headerJson);
		auto payloadB64 = base64UrlEncode(payloadJson);

		// 签名：HMAC-SHA256(headerB64 + "." + payloadB64, secret)
		auto signInput = headerB64 + "." + payloadB64;
		auto sig = hmacSha256(signInput, opts.secret);
		auto sigB64 = base64UrlEncode(std::string_view(reinterpret_cast<const char*>(sig.data()), sig.size()));

		return headerB64 + "." + payloadB64 + "." + sigB64;
	}

	std::string jwtSign(const boost::json::object& payload, std::string_view secret)
	{
		JwtAuthOptions opts;
		opts.secret = std::string(secret);
		return jwtSign(payload, opts);
	}

	// ============== JWT 验证 ==============

	boost::json::object jwtVerify(std::string_view token, std::string_view secret)
	{
		// 1. 按 . 拆分为三段
		auto firstDot = token.find('.');
		if (firstDot == std::string_view::npos)
		{
			throw std::runtime_error("JWT: invalid token format, missing dots");
		}

		auto secondDot = token.find('.', firstDot + 1);
		if (secondDot == std::string_view::npos)
		{
			throw std::runtime_error("JWT: invalid token format, missing signature");
		}

		auto headerB64 = token.substr(0, firstDot);
		auto payloadB64 = token.substr(firstDot + 1, secondDot - firstDot - 1);
		auto sigB64 = token.substr(secondDot + 1);

		// 2. 解码原始签名做常数时间比较，消除时序侧信道
		auto sigRaw = base64UrlDecode(sigB64);
		if (sigRaw.size() != kSha256OutputLen)
		{
			throw std::runtime_error("JWT: invalid signature length");
		}

		auto signInput = std::string(headerB64) + "." + std::string(payloadB64);
		auto expectedSig = hmacSha256(signInput, secret);

		if (CRYPTO_memcmp(sigRaw.data(), expectedSig.data(), kSha256OutputLen) != 0)
		{
			throw std::runtime_error("JWT: signature verification failed");
		}

		// 3. 解码 header（验证算法）
		auto headerJson = base64UrlDecode(headerB64);
		auto header = boost::json::parse(headerJson).as_object();

		auto algIt = header.find("alg");
		if (algIt == header.end() || algIt->value().as_string() != "HS256")
		{
			throw std::runtime_error("JWT: unsupported algorithm, expected HS256");
		}

		// 4. 解码 payload
		auto payloadJson = base64UrlDecode(payloadB64);
		auto payload = boost::json::parse(payloadJson).as_object();

		// 5. 检查过期
		auto expIt = payload.find("exp");
		if (expIt != payload.end())
		{
			auto expVal = expIt->value().as_int64();
			auto now =
				std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
					.count();

			if (now > expVal)
			{
				throw std::runtime_error("JWT: token has expired");
			}
		}

		return payload;
	}

	boost::json::object jwtVerify(std::string_view token, const JwtAuthOptions& opts)
	{
		auto payload = jwtVerify(token, opts.secret);

		// 校验 iss（RFC 7519 §4.1.1）
		if (!opts.issuer.empty())
		{
			auto issIt = payload.find("iss");
			if (issIt == payload.end() || issIt->value().as_string() != opts.issuer)
			{
				throw std::runtime_error("JWT: issuer mismatch");
			}
		}

		// 校验 aud（RFC 7519 §4.1.3）—— 支持单字符串或字符串数组
		if (!opts.audience.empty())
		{
			auto audIt = payload.find("aud");
			if (audIt == payload.end())
			{
				throw std::runtime_error("JWT: audience missing");
			}

			auto& audVal = audIt->value();
			bool matched = false;
			if (audVal.is_string())
			{
				matched = (audVal.as_string() == opts.audience);
			}
			else if (audVal.is_array())
			{
				for (const auto& v : audVal.as_array())
				{
					if (v.is_string() && v.as_string() == opts.audience)
					{
						matched = true;
						break;
					}
				}
			}

			if (!matched)
			{
				throw std::runtime_error("JWT: audience mismatch");
			}
		}

		// 校验 nbf（RFC 7519 §4.1.5）
		auto nbfIt = payload.find("nbf");
		if (nbfIt != payload.end())
		{
			auto nbfVal = nbfIt->value().as_int64();
			auto now =
				std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
					.count();

			if (now < nbfVal)
			{
				throw std::runtime_error("JWT: token not yet valid (nbf)");
			}
		}

		return payload;
	}

	// ============== 中间件工厂 ==============

	namespace
	{

		/**
		 * @brief 大小写不敏感比较两个 string_view 是否相等
		 */
		[[nodiscard]] bool iequals(std::string_view a, std::string_view b)
		{
			return a.size() == b.size()
				   && std::equal(a.begin(),
								 a.end(),
								 b.begin(),
								 [](char ca, char cb)
								 {
									 return std::tolower(ca) == std::tolower(cb);
								 });
		}

		/// HS256 最小密钥长度（字节），RFC 7518 §3.2
		constexpr size_t kMinSecretLength = 32;

	} // namespace

	SyncBeforeHandler makeJwtAuthMiddleware(JwtAuthOptions opts)
	{
		// 拒绝空密钥和过短密钥，避免生产环境因配置遗漏而 fail-open
		if (opts.secret.size() < kMinSecretLength)
		{
			throw std::invalid_argument("JWT: secret is required and must be at least 32 bytes for HS256");
		}

		auto options = std::make_shared<JwtAuthOptions>(std::move(opts));

		return [options](HttpRequest& req) -> SyncMiddlewareResult
		{
			// 白名单路径：直接放行
			auto path = req.path();
			for (const auto& skipPath : options->skipPaths)
			{
				if (path == skipPath)
				{
					return std::nullopt;
				}
			}

			// 提取 Authorization 头
			auto authHeader = req.header("Authorization");
			if (authHeader.empty())
			{
				HttpResponse res;
				res.setStatus(HttpStatusCode::hUnauthorized);
				res.setJsonBody({{"error", "Unauthorized"}, {"message", "missing Authorization header"}});
				return res;
			}

			// 必须是 "Bearer" + 空格/TAB + <token>（RFC 7235 §2.1）
			constexpr std::string_view kScheme = "Bearer";
			constexpr size_t kSchemeLen = 6;
			if (authHeader.size() <= kSchemeLen || !iequals(authHeader.substr(0, kSchemeLen), kScheme)
				|| (authHeader[kSchemeLen] != ' ' && authHeader[kSchemeLen] != '\t'))
			{
				HttpResponse res;
				res.setStatus(HttpStatusCode::hUnauthorized);
				res.setJsonBody(
					{{"error", "Unauthorized"}, {"message", "Authorization header must use Bearer scheme"}});
				return res;
			}

			// 跳过 scheme 后的空格/TAB 得到 token
			auto tokenPos = kSchemeLen;
			while (tokenPos < authHeader.size() && (authHeader[tokenPos] == ' ' || authHeader[tokenPos] == '\t'))
			{
				++tokenPos;
			}
			auto token = authHeader.substr(tokenPos);

			// 验证 Token（含 iss/aud/nbf 校验）
			try
			{
				auto payload = jwtVerify(token, *options);
				req.setAttribute("jwt.payload", std::move(payload));
				return std::nullopt;
			}
			catch (const std::exception& e)
			{
				HICAL_LOG_DEBUG("JWT verification failed: {}", e.what());

				HttpResponse res;
				res.setStatus(HttpStatusCode::hUnauthorized);
				res.setJsonBody({{"error", "Unauthorized"}, {"message", "Invalid token"}});
				return res;
			}
		};
	}

} // namespace hical
