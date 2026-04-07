#pragma once

#include "HttpTypes.h"
#include <boost/beast/http.hpp>
#include <boost/json.hpp>
#include <memory>
#include <memory_resource>
#include <string>
#include <string_view>
#include <unordered_map>

namespace hical
{

	/**
 * @brief HTTP 请求封装
 *
 * 对 Boost.Beast http::request 的 hical 风格封装。
 * 提供简洁的接口访问请求方法、路径、头部和消息体。
 * 消息体使用 pmr 分配器管理内存。
 */
	class HttpRequest
	{
	public:
		using BeastRequest = boost::beast::http::request<boost::beast::http::string_body>;

		HttpRequest();

		/**
     * @brief 从 Beast request 构造
     * @param req Beast HTTP 请求
     */
		explicit HttpRequest(BeastRequest req);

		/**
     * @brief 获取 HTTP 方法
     * @return HTTP 方法枚举
     */
		HttpMethod method() const;

		/**
     * @brief 获取请求路径（不含查询参数）
     * @return 路径字符串
     */
		std::string path() const;

		/**
     * @brief 获取完整的 URI（含查询参数）
     * @return URI 字符串
     */
		std::string target() const;

		/**
     * @brief 获取查询字符串（? 后面的部分）
     * @return 查询字符串
     */
		std::string query() const;

		/**
     * @brief 获取指定头部字段
     * @param name 头部字段名
     * @return 字段值，未找到返回空字符串
     */
		std::string header(const std::string& name) const;

		/**
     * @brief 获取消息体
     * @return 消息体字符串
     */
		const std::string& body() const;

		/**
     * @brief 将消息体解析为 JSON
     * @return boost::json::value
     */
		boost::json::value jsonBody() const;

		/**
     * @brief 获取 Content-Type 头
     * @return Content-Type 字符串
     */
		std::string contentType() const;

		/**
     * @brief 获取底层 Beast 请求的引用
     * @return Beast 请求引用
     */
		BeastRequest& native();
		const BeastRequest& native() const;

		/**
     * @brief 设置 HTTP 方法（用于构建请求）
     * @param method HTTP 方法
     */
		void setMethod(HttpMethod method);

		/**
     * @brief 设置请求路径（用于构建请求）
     * @param target 目标 URI
     */
		void setTarget(const std::string& target);

		/**
     * @brief 设置头部字段（用于构建请求）
     * @param name 字段名
     * @param value 字段值
     */
		void setHeader(const std::string& name, const std::string& value);

		/**
     * @brief 设置消息体（用于构建请求）
     * @param body 消息体
     */
		void setBody(const std::string& body);

		// ============ 路径参数 ============

		/**
     * @brief 获取路径参数
     * @param name 参数名（如 "id"）
     * @return 参数值，未找到返回空字符串
     *
     * 路径 "/users/{id}" 匹配 "/users/123" 时，param("id") 返回 "123"
     */
		std::string param(const std::string& name) const;

		/**
     * @brief 设置路径参数（由 Router 内部调用）
     * @param name 参数名
     * @param value 参数值
     */
		void setParam(const std::string& name, const std::string& value);

		/**
     * @brief 是否有指定路径参数
     * @param name 参数名
     * @return true 如果存在
     */
		bool hasParam(const std::string& name) const;

	private:
		BeastRequest req_;
		std::unordered_map<std::string, std::string> pathParams_;
	};

} // namespace hical
