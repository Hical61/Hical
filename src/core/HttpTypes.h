/**
 * @file HttpTypes.h
 * @brief HTTP 方法/状态码枚举与工具函数
 */

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace hical
{

	/**
	 * @brief 透明字符串哈希（支持 string_view 异构查找，避免构造临时 std::string）
	 */
	struct StringHash
	{
		using is_transparent = void;

		size_t operator()(std::string_view sv) const noexcept
		{
			return std::hash<std::string_view> {}(sv);
		}
	};

	/**
	 * @brief 透明字符串相等比较（配合 StringHash 使用）
	 */
	struct StringEqual
	{
		using is_transparent = void;

		bool operator()(std::string_view a, std::string_view b) const
		{
			return a == b;
		}
	};

	/**
	 * @brief HTTP 请求方法枚举
	 */
	enum class HttpMethod : uint8_t
	{
		hGet,
		hPost,
		hPut,
		hDelete,
		hPatch,
		hHead,
		hOptions,
		hUnknown
	};

	/**
	 * @brief HTTP 状态码枚举
	 */
	enum class HttpStatusCode : uint16_t
	{
		// 2xx 成功
		hOk = 200,
		hCreated = 201,
		hAccepted = 202,
		hNoContent = 204,
		hPartialContent = 206,

		// 3xx 重定向
		hMovedPermanently = 301,
		hFound = 302,
		hNotModified = 304,
		hTemporaryRedirect = 307,
		hPermanentRedirect = 308,

		// 4xx 客户端错误
		hBadRequest = 400,
		hUnauthorized = 401,
		hForbidden = 403,
		hNotFound = 404,
		hMethodNotAllowed = 405,
		hConflict = 409,
		hPayloadTooLarge = 413,
		hRequestedRangeNotSatisfiable = 416,
		hTooManyRequests = 429,

		// 5xx 服务端错误
		hInternalServerError = 500,
		hNotImplemented = 501,
		hBadGateway = 502,
		hServiceUnavailable = 503
	};

	/**
	 * @brief 将 HttpMethod 转换为字符串
	 * @param method HTTP 方法
	 * @return 方法名字符串（如 "GET"）
	 */
	const char* httpMethodToString(HttpMethod method);

	/**
	 * @brief 将字符串转换为 HttpMethod
	 * @param str 方法名字符串
	 * @return HTTP 方法枚举值
	 */
	HttpMethod stringToHttpMethod(const std::string& str);

	/**
	 * @brief 获取状态码的默认描述文本
	 * @param code 状态码
	 * @return 描述文本（如 "OK"）
	 */
	const char* httpStatusCodeToString(HttpStatusCode code);

} // namespace hical
