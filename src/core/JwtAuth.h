/**
 * @file JwtAuth.h
 * @brief JWT 认证中间件，基于 OpenSSL EVP 自实现 HS256 签发和验证，零第三方依赖
 * 使用示例：
 * ```cpp
 * // 签发 Token
 * auto token = jwtSign({{"sub", "user123"}, {"role", "admin"}}, "my-secret");
 * // 验证 Token
 * auto payload = jwtVerify(token, "my-secret");
 * // 中间件用法
 * server.use(makeJwtAuthMiddleware({
 *     .secret = "my-secret",
 *     .skipPaths = {"/public/health", "/public/metrics"},
 * }));
 * // 在 handler 中读取注入的 payload：
 * auto jwt = req.getAttribute<boost::json::object>("jwt.payload");
 * ```
 */

#pragma once

#include "HttpRequest.h"
#include "HttpResponse.h"
#include "Middleware.h"
#include <boost/json.hpp>
#include <chrono>
#include <string>
#include <string_view>
#include <vector>

namespace hical
{

	/**
	 * @brief JWT 认证中间件配置
	 */
	struct JwtAuthOptions
	{
		/// HMAC-SHA256 共享密钥，中间件要求长度 >= 32 字节（HS256 最小安全长度，RFC 7518 §3.2）
		std::string secret;

		/// Token 签发者（iss claim），为空则不写入
		std::string issuer;

		/// Token 受众（aud claim），为空则不写入
		std::string audience;

		/// Token 有效时长，默认 24 小时。仅在 payload 未含 exp 字段时生效
		std::chrono::seconds tokenExpiry = std::chrono::hours {24};

		/// 白名单路径，这些路径不检查 Authorization 头直接放行
		std::vector<std::string> skipPaths;
	};

	/**
	 * @brief 签发 JWT Token（HS256）
	 * 若 payload 中未含 iat，自动添加当前时间。
	 * 若 payload 中未含 exp，自动添加 now + tokenExpiry。
	 * 若 JwtAuthOptions::issuer/audience 非空且 payload 未含 iss/aud，写入对应 claim。
	 * @param payload 自定义 claims
	 * @param opts 签发配置
	 * @return Base64URL 编码的 JWT 字符串（三段式：header.payload.signature）
	 */
	[[nodiscard]] std::string jwtSign(const boost::json::object& payload, const JwtAuthOptions& opts);

	/**
	 * @brief 签发 JWT Token（HS256），仅用 secret
	 * 便捷重载：当不需要 issuer/audience/自定义 expiry 时使用。
	 * exp 默认为签发时间 + 24 小时。
	 * @param payload 自定义 claims
	 * @param secret HMAC 密钥
	 * @return JWT 字符串
	 */
	[[nodiscard]] std::string jwtSign(const boost::json::object& payload, std::string_view secret);

	/**
	 * @brief 验证 JWT Token（HS256）并返回 payload
	 * 验证签名、检查 exp 过期。
	 * @param token JWT 字符串
	 * @param secret HMAC 密钥
	 * @return 解码后的 payload（包含 iat/exp 等标准字段）
	 * @throws std::runtime_error 签名不匹配、Token 过期、格式错误
	 */
	boost::json::object jwtVerify(std::string_view token, std::string_view secret);

	/**
	 * @brief 验证 JWT Token（HS256）并返回 payload，同时校验 iss/aud/nbf
	 * 在基本验证（签名、算法、exp）基础上，额外校验：
	 * - iss：若 opts.issuer 非空，要求 payload["iss"] 精确匹配
	 * - aud：若 opts.audience 非空，要求 payload["aud"] 等于或包含（支持字符串数组）opts.audience
	 * - nbf：若 payload["nbf"] 存在，要求当前时间 >= nbf
	 * @param token JWT 字符串
	 * @param opts 签发配置（secret 用于验签，issuer/audience 用于校验 claims）
	 * @return 解码后的 payload
	 * @throws std::runtime_error 签名不匹配、Token 过期、iss/aud 不匹配、格式错误
	 */
	boost::json::object jwtVerify(std::string_view token, const JwtAuthOptions& opts);

	/**
	 * @brief 创建 JWT 认证中间件（SyncBeforeHandler）
	 * 中间件行为：
	 * 1. 从 Authorization: Bearer <token> 头提取 Token（scheme 大小写不敏感）
	 * 2. 白名单路径直接放行（返回 nullopt）
	 * 3. 验证 Token：通过 → 注入 req.setAttribute("jwt.payload", payload) + 放行
	 * 4. 验证失败 → 返回 401（不泄漏内部错误详情）
	 * @param opts JWT 配置
	 * @return SyncBeforeHandler
	 */
	[[nodiscard]] SyncBeforeHandler makeJwtAuthMiddleware(JwtAuthOptions opts);

} // namespace hical
