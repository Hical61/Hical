#pragma once

#include "Cookie.h"
#include "HttpTypes.h"
#include <boost/beast/http.hpp>
#include <boost/json.hpp>
#include <string>
#include <string_view>

namespace hical
{

	/**
	 * @brief HTTP 响应封装
	 * 对 Boost.Beast http::response 的 hical 风格封装。
	 * 提供简洁的接口设置状态码、头部和消息体。
	 */
	class HttpResponse
	{
	public:
		using BeastResponse = boost::beast::http::response<boost::beast::http::string_body>;

		HttpResponse();

		/**
		 * @brief 从 Beast response 构造
		 * @param res Beast HTTP 响应
		 */
		explicit HttpResponse(BeastResponse res);

		/**
		 * @brief 获取状态码
		 * @return HTTP 状态码
		 */
		HttpStatusCode statusCode() const;

		/**
		 * @brief 设置状态码
		 * @param code HTTP 状态码
		 */
		void setStatus(HttpStatusCode code);

		/**
		 * @brief 获取指定头部字段
		 * @param name 字段名
		 * @return 字段值
		 */
		std::string_view header(std::string_view name) const;

		/**
		 * @brief 设置头部字段
		 * @param name 字段名
		 * @param value 字段值
		 */
		void setHeader(const std::string& name, const std::string& value);

		/**
		 * @brief 获取消息体
		 * @return 消息体字符串
		 */
		const std::string& body() const;

		/**
		 * @brief 设置消息体（纯文本）
		 * @param body 消息体
		 * @param contentType Content-Type（默认 text/plain）
		 */
		void setBody(const std::string& body, const std::string& contentType = "text/plain");

		/**
		 * @brief 设置 JSON 消息体
		 * @param json JSON 值
		 */
		void setJsonBody(const boost::json::value& json);

		/**
		 * @brief 添加 Set-Cookie 响应头
		 * @param name Cookie 名称
		 * @param value Cookie 值
		 * @param options Cookie 设置选项（有效期、路径、HttpOnly 等）
		 * 可多次调用以设置多个 Cookie，每次追加一个 Set-Cookie 头。
		 */
		void setCookie(const std::string& name, const std::string& value, const CookieOptions& options = {});

		/**
		 * @brief 获取底层 Beast 响应的引用
		 * @return Beast 响应引用
		 */
		BeastResponse& native();
		const BeastResponse& native() const;

		// ============ 快捷工厂方法 ============

		/**
		 * @brief 创建 200 OK 响应
		 * @param body 消息体
		 * @return HttpResponse
		 */
		static HttpResponse ok(const std::string& body = "");

		/**
		 * @brief 创建 JSON 200 OK 响应
		 * @param json JSON 值
		 * @return HttpResponse
		 */
		static HttpResponse json(const boost::json::value& json);

		/**
		 * @brief 创建 404 Not Found 响应
		 * @return HttpResponse
		 */
		static HttpResponse notFound();

		/**
		 * @brief 创建 400 Bad Request 响应
		 * @param message 错误信息
		 * @return HttpResponse
		 */
		static HttpResponse badRequest(const std::string& message = "Bad Request");

		/**
		 * @brief 创建 500 Internal Server Error 响应
		 * @return HttpResponse
		 */
		static HttpResponse serverError();

		/**
		 * @brief 创建重定向响应
		 * @param location 重定向目标 URL
		 * @param code 状态码（默认 302 Found，支持 301/302/307/308）
		 * @return HttpResponse
		 */
		static HttpResponse redirect(const std::string& location, HttpStatusCode code = HttpStatusCode::hFound);

	private:
		BeastResponse res_;
	};

} // namespace hical
