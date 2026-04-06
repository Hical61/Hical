#pragma once

#include <cstdint>
#include <string>

namespace hical
{

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

    // 3xx 重定向
    hMovedPermanently = 301,
    hFound = 302,
    hNotModified = 304,

    // 4xx 客户端错误
    hBadRequest = 400,
    hUnauthorized = 401,
    hForbidden = 403,
    hNotFound = 404,
    hMethodNotAllowed = 405,
    hConflict = 409,
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

}  // namespace hical
