/**
 * @file HttpRequest.h
 * @brief 零拷贝 HTTP 请求解析与访问
 */

#pragma once

#include "HeaderMap.h"
#include "HttpTypes.h"
#include <any>
#include <array>
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

	class MultipartParser; // 前向声明，用于友元

	/**
	 * @brief 零拷贝请求头部容器
	 * 存储 string_view 对，引用外部缓冲区（如连接级 readBuf）。
	 * 栈分配，无堆分配。最多 64 个头部（HTTP 请求极少超过此数量）。
	 */
	class RequestHeaders
	{
	public:
		struct Entry
		{
			std::string_view name;
			std::string_view value;
		};

		static constexpr size_t kMaxHeaders = 64;

		void add(std::string_view name, std::string_view value) noexcept
		{
			if (size_ < kMaxHeaders)
			{
				entries_[size_++] = {name, value};
			}
		}

		std::string_view find(std::string_view name) const noexcept
		{
			for (size_t i = 0; i < size_; ++i)
			{
				if (HeaderMap::iequals(entries_[i].name, name))
				{
					return entries_[i].value;
				}
			}
			return {};
		}

		bool contains(std::string_view name) const noexcept
		{
			for (size_t i = 0; i < size_; ++i)
			{
				if (HeaderMap::iequals(entries_[i].name, name))
				{
					return true;
				}
			}
			return false;
		}

		size_t size() const noexcept
		{
			return size_;
		}

		const Entry& operator[](size_t i) const noexcept
		{
			return entries_[i];
		}

		const Entry* begin() const noexcept
		{
			return entries_.data();
		}

		const Entry* end() const noexcept
		{
			return entries_.data() + size_;
		}

		void clear() noexcept
		{
			size_ = 0;
		}

	private:
		std::array<Entry, kMaxHeaders> entries_;
		size_t size_ = 0;
	};

	/**
	 * @brief HTTP 请求的原生内部表示（零拷贝）
	 * target 和 headers 中的 string_view 引用连接级 readBuf，
	 * 生命周期由 handleSession 的 for 循环保证。
	 * body 拥有独立内存（从 socket 读取时直接写入）。
	 */
	struct NativeRequest
	{
		HttpMethod method = HttpMethod::hUnknown;
		std::string_view target; // 零拷贝，引用 readBuf
		int httpVersionMajor = 1;
		int httpVersionMinor = 1;
		RequestHeaders headers; // 零拷贝，引用 readBuf
		std::string body;       // 拥有所有权（从 socket 读取）
		bool keepAlive = true;
		bool expectContinue = false; // 请求携带了 Expect: 100-continue

		/**
		 * @brief 是否为 WebSocket 升级请求
		 */
		bool isUpgrade() const
		{
			auto conn = headers.find("Connection");
			if (conn.empty())
			{
				return false;
			}
			// Connection 可能是 "keep-alive, Upgrade" 形式
			bool hasUpgrade = false;
			std::string_view sv(conn);
			while (!sv.empty())
			{
				auto comma = sv.find(',');
				auto token = (comma != std::string_view::npos) ? sv.substr(0, comma) : sv;
				sv = (comma != std::string_view::npos) ? sv.substr(comma + 1) : std::string_view {};
				while (!token.empty() && token.front() == ' ')
				{
					token.remove_prefix(1);
				}
				while (!token.empty() && token.back() == ' ')
				{
					token.remove_suffix(1);
				}
				if (HeaderMap::iequals(token, "upgrade"))
				{
					hasUpgrade = true;
					break;
				}
			}
			if (!hasUpgrade)
			{
				return false;
			}
			return HeaderMap::iequals(headers.find("Upgrade"), "websocket");
		}
	};

	/**
	 * @brief HTTP 请求封装
	 * 对自研 NativeRequest 的 hical 风格封装。
	 * 提供简洁的接口访问请求方法、路径、头部和消息体。
	 */
	class HttpRequest
	{
		friend class MultipartParser;

	public:
		HttpRequest();

		/**
		 * @brief 从已解析的 NativeRequest 构造（内部使用，跳过 CRLF 检查）
		 * @param req 已解析的原生请求
		 * @return HttpRequest
		 */
		[[nodiscard]] static HttpRequest fromParsed(NativeRequest&& req);

		[[nodiscard]] HttpMethod method() const;
		[[nodiscard]] std::string_view path() const;
		[[nodiscard]] std::string_view target() const;
		[[nodiscard]] std::string_view query() const;
		[[nodiscard]] std::string_view header(std::string_view name) const;
		[[nodiscard]] const std::string& body() const;
		[[nodiscard]] const boost::json::value& jsonBody() const;

		/**
		 * @brief 反序列化请求体 JSON 为指定类型（需要 HICAL_JSON 标注）
		 */
		template <typename T>
		[[nodiscard]] T readJson() const;

		/**
		 * @brief 获取 Content-Type 头部值
		 */
		[[nodiscard]] std::string_view contentType() const;

		/**
		 * @brief 获取底层 NativeRequest 引用（可修改）
		 */
		[[nodiscard]] NativeRequest& native();
		/**
		 * @brief 获取底层 NativeRequest 常量引用
		 */
		[[nodiscard]] const NativeRequest& native() const;

		void setMethod(HttpMethod method);
		void setTarget(const std::string& target);
		void setHeader(const std::string& name, const std::string& value);
		void setBody(const std::string& body);

		// ============ 路径参数 ============

		/**
		 * @brief 获取路径参数值（如 /users/{id} 中的 id），不存在则返回空字符串引用
		 */
		[[nodiscard]] const std::string& param(std::string_view name) const;
		void setParam(const std::string& name, const std::string& value);
		[[nodiscard]] bool hasParam(std::string_view name) const;

		// ============ Cookie ============

		/**
		 * @brief 获取指定名称的 Cookie 值，不存在则返回空字符串引用
		 */
		[[nodiscard]] const std::string& cookie(std::string_view name) const;
		/**
		 * @brief 获取所有 Cookie（惰性解析，首次调用时解析 Cookie 头）
		 */
		[[nodiscard]] const std::unordered_map<std::string, std::string, StringHash, StringEqual>& cookies() const;
		[[nodiscard]] bool hasCookie(std::string_view name) const;

		// ============ 查询参数 ============

		/**
		 * @brief 获取指定名称的查询参数值，不存在则返回 nullopt
		 */
		[[nodiscard]] std::optional<std::string> queryParam(std::string_view name) const;
		/**
		 * @brief 获取所有查询参数（惰性解析，支持同名多值）
		 */
		[[nodiscard]] const std::unordered_multimap<std::string, std::string, StringHash, StringEqual>& queryParams()
			const;
		[[nodiscard]] bool hasQueryParam(std::string_view name) const;

		// ============ 表单参数 ============

		/**
		 * @brief 获取指定名称的表单参数值（application/x-www-form-urlencoded），不存在则返回 nullopt
		 */
		[[nodiscard]] std::optional<std::string> formParam(std::string_view name) const;
		/**
		 * @brief 获取所有表单参数（惰性解析，支持同名多值）
		 */
		[[nodiscard]] const std::unordered_multimap<std::string, std::string, StringHash, StringEqual>& formParams()
			const;
		[[nodiscard]] bool hasFormParam(std::string_view name) const;

		// ============ 请求级属性 ============

		/**
		 * @brief 设置请求级属性（中间件间数据传递，如 trace_id、db connection 等）
		 */
		void setAttribute(std::string_view key, std::any value);
		/**
		 * @brief 获取请求级属性，不存在则返回 nullopt
		 */
		[[nodiscard]] std::optional<std::any> getAttribute(std::string_view key) const;

		template <typename T>
		[[nodiscard]] std::optional<T> getAttribute(const std::string& key) const
		{
			if (!attributes_)
			{
				return std::nullopt;
			}
			auto it = attributes_->find(key);
			if (it == attributes_->end())
			{
				return std::nullopt;
			}
			if (it->second.type() != typeid(T))
			{
				return std::nullopt;
			}
			return std::any_cast<T>(it->second);
		}

	private:
		void parseCookies() const;
		static void parseUrlEncoded(std::string_view input,
									std::unordered_multimap<std::string, std::string, StringHash, StringEqual>& out);
		void parseQueryParams() const;
		void parseFormParams() const;

		NativeRequest req_;
		// setter 专用拥有存储（测试/构建请求场景，不在热路径）
		std::string ownedTarget_;
		HeaderMap ownedHeaders_;
		std::vector<std::pair<std::string, std::string>> pathParams_;
		mutable std::optional<std::unordered_map<std::string, std::string, StringHash, StringEqual>> cookies_;
		mutable std::optional<boost::json::value> cachedJsonBody_;
		mutable std::optional<std::unordered_multimap<std::string, std::string, StringHash, StringEqual>> queryParams_;
		mutable std::optional<std::unordered_multimap<std::string, std::string, StringHash, StringEqual>> formParams_;
		mutable std::any cachedMultipartParts_;
		std::unique_ptr<std::unordered_map<std::string, std::any, StringHash, StringEqual>> attributes_;
	};

} // namespace hical
