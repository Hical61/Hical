#pragma once

#include "HttpTypes.h"
#include <any>
#include <boost/beast/http.hpp>
#include <boost/json.hpp>
#include <memory>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace hical
{

	/**
	 * @brief HTTP 请求封装
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
		 * @return 路径 string_view（零拷贝，生命周期与请求对象一致）
		 */
		std::string_view path() const;

		/**
		 * @brief 获取完整的 URI（含查询参数）
		 * @return URI string_view
		 */
		std::string_view target() const;

		/**
		 * @brief 获取查询字符串（? 后面的部分）
		 * @return 查询字符串 string_view
		 */
		std::string_view query() const;

		/**
		 * @brief 获取指定头部字段
		 * @param name 头部字段名
		 * @return 字段值的 string_view（生命周期与请求对象一致），未找到返回空
		 */
		std::string_view header(std::string_view name) const;

		/**
		 * @brief 获取消息体
		 * @return 消息体字符串
		 */
		const std::string& body() const;

		/**
		 * @brief 将消息体解析为 JSON（带缓存，多次调用零拷贝）
		 * @return boost::json::value 的常引用
		 */
		const boost::json::value& jsonBody() const;

		/**
		 * @brief 将消息体反序列化为指定类型（反射驱动）
		 * @tparam T 目标类型（需标注 HICAL_JSON 或支持 C++26 反射）
		 * @return T 反序列化后的对象
		 * 注意：使用此方法需 #include "MetaJson.h"（定义在该头文件中）。
		 */
		template <typename T>
		T readJson() const;

		/**
		 * @brief 获取 Content-Type 头
		 * @return Content-Type string_view
		 */
		std::string_view contentType() const;

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
		 * @return 参数值的常引用，未找到返回空字符串引用
		 */
		const std::string& param(std::string_view name) const;

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
		bool hasParam(std::string_view name) const;

		// ============ Cookie ============

		/**
		 * @brief 获取指定名称的 Cookie 值
		 * @param name Cookie 名称
		 * @return Cookie 值的常引用，不存在返回空字符串引用
		 */
		const std::string& cookie(std::string_view name) const;

		/**
		 * @brief 获取所有 Cookie
		 * @return Cookie 名值 map（首次调用时惰性解析 Cookie 头）
		 */
		const std::unordered_map<std::string, std::string, StringHash, StringEqual>& cookies() const;

		/**
		 * @brief 是否包含指定 Cookie
		 * @param name Cookie 名称
		 * @return true 如果存在
		 */
		bool hasCookie(std::string_view name) const;

		// ============ 查询参数 ============

		/**
		 * @brief 获取指定名称的查询参数值
		 * @param name 参数名
		 * @return 参数值，不存在返回 nullopt
		 */
		std::optional<std::string> queryParam(std::string_view name) const;

		/**
		 * @brief 获取所有查询参数
		 * @return 查询参数 multimap（首次调用时惰性解析）
		 */
		const std::unordered_multimap<std::string, std::string, StringHash, StringEqual>& queryParams() const;

		/**
		 * @brief 是否包含指定查询参数
		 * @param name 参数名
		 * @return true 如果存在
		 */
		bool hasQueryParam(std::string_view name) const;

		// ============ 表单参数（application/x-www-form-urlencoded） ============

		/**
		 * @brief 获取指定名称的表单参数值
		 * @param name 参数名
		 * @return 参数值，不存在返回 nullopt
		 */
		std::optional<std::string> formParam(std::string_view name) const;

		/**
		 * @brief 获取所有表单参数
		 * @return 表单参数 multimap（首次调用时惰性解析）
		 */
		const std::unordered_multimap<std::string, std::string, StringHash, StringEqual>& formParams() const;

		/**
		 * @brief 是否包含指定表单参数
		 * @param name 参数名
		 * @return true 如果存在
		 */
		bool hasFormParam(std::string_view name) const;

		// ============ 请求级属性（供中间件传递上下文数据） ============

		/**
		 * @brief 设置请求级属性
		 * @param key 属性键
		 * @param value 任意类型的值（使用 std::any 存储）
		 */
		void setAttribute(const std::string& key, std::any value);

		/**
		 * @brief 获取请求级属性
		 * @param key 属性键
		 * @return 找到返回 std::any，否则 nullopt
		 */
		std::optional<std::any> getAttribute(const std::string& key) const;

		/**
		 * @brief 获取请求级属性（类型安全版本）
		 * @tparam T 期望的值类型
		 * @param key 属性键
		 * @return 找到且类型匹配返回值，否则 nullopt
		 */
		template <typename T>
		std::optional<T> getAttribute(const std::string& key) const
		{
			auto it = attributes_.find(key);
			if (it == attributes_.end())
			{
				return std::nullopt;
			}
			try
			{
				return std::any_cast<T>(it->second);
			}
			catch (const std::bad_any_cast&)
			{
				return std::nullopt;
			}
		}

	private:
		/**
		 * @brief 惰性解析 Cookie 头，结果缓存到 cookies_
		 */
		void parseCookies() const;

		/**
		 * @brief URL 编码键值对解析（查询参数和表单参数共用）
		 * @param input 输入字符串（如 "key1=val1&key2=val2"）
		 * @param out 输出 multimap
		 */
		static void parseUrlEncoded(std::string_view input,
									std::unordered_multimap<std::string, std::string, StringHash, StringEqual>& out);

		void parseQueryParams() const;
		void parseFormParams() const;

		BeastRequest req_;
		std::vector<std::pair<std::string, std::string>> pathParams_;
		mutable std::optional<std::unordered_map<std::string, std::string, StringHash, StringEqual>> cookies_;
		mutable std::optional<boost::json::value> cachedJsonBody_;
		mutable std::optional<std::unordered_multimap<std::string, std::string, StringHash, StringEqual>> m_queryParams;
		mutable std::optional<std::unordered_multimap<std::string, std::string, StringHash, StringEqual>> m_formParams;
		std::unordered_map<std::string, std::any> attributes_;
	};

} // namespace hical
