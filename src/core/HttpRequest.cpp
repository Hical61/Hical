/**
 * @file HttpRequest.cpp
 * @brief HTTP 请求解析实现
 */

#include "HttpRequest.h"
#include "Router.h"

namespace hical
{

	namespace
	{

		/// 检测 CR/LF（防 Header Injection）
		bool containsCRLF(const std::string& s)
		{
			for (char c : s)
			{
				if (c == '\r' || c == '\n')
				{
					return true;
				}
			}
			return false;
		}

	} // namespace

	HttpRequest::HttpRequest()
	{
		req_.httpVersionMajor = 1;
		req_.httpVersionMinor = 1;
	}

	HttpRequest HttpRequest::fromParsed(NativeRequest&& req)
	{
		HttpRequest result;
		result.req_ = std::move(req);
		return result;
	}

	HttpMethod HttpRequest::method() const
	{
		return req_.method;
	}

	std::string_view HttpRequest::path() const
	{
		std::string_view t = req_.target;
		if (auto pos = t.find('?'); pos != std::string_view::npos)
		{
			return t.substr(0, pos);
		}
		return t;
	}

	std::string_view HttpRequest::target() const
	{
		return req_.target;
	}

	std::string_view HttpRequest::query() const
	{
		std::string_view t = req_.target;
		if (auto pos = t.find('?'); pos != std::string_view::npos)
		{
			return t.substr(pos + 1);
		}
		return {};
	}

	std::string_view HttpRequest::header(std::string_view name) const
	{
		return req_.headers.find(name);
	}

	const std::string& HttpRequest::body() const
	{
		return req_.body;
	}

	const boost::json::value& HttpRequest::jsonBody() const
	{
		if (!cachedJsonBody_)
		{
			boost::system::error_code ec;
			auto val = boost::json::parse(req_.body, ec);
			cachedJsonBody_.emplace(ec ? boost::json::value(nullptr) : std::move(val));
		}
		return *cachedJsonBody_;
	}

	std::string_view HttpRequest::contentType() const
	{
		return header("Content-Type");
	}

	NativeRequest& HttpRequest::native()
	{
		return req_;
	}

	const NativeRequest& HttpRequest::native() const
	{
		return req_;
	}

	void HttpRequest::setMethod(HttpMethod method)
	{
		req_.method = method;
	}

	void HttpRequest::setTarget(const std::string& target)
	{
		ownedTarget_ = target;
		req_.target = ownedTarget_;
	}

	void HttpRequest::setHeader(const std::string& name, const std::string& value)
	{
		// 防 Header Injection
		if (containsCRLF(name) || containsCRLF(value))
		{
			return;
		}
		// setter 走 owned HeaderMap（测试用）
		ownedHeaders_.set(name, value);
		// 同步到 NativeRequest.headers
		req_.headers.clear();
		for (const auto& [k, v] : ownedHeaders_)
		{
			req_.headers.add(k, v);
		}
	}

	void HttpRequest::setBody(const std::string& body)
	{
		req_.body = body;
		// 更新 Content-Length
		ownedHeaders_.set("Content-Length", std::to_string(body.size()));
		req_.headers.clear();
		for (const auto& [k, v] : ownedHeaders_)
		{
			req_.headers.add(k, v);
		}
	}

	// ============ 路径参数 ============

	const std::string& HttpRequest::param(std::string_view name) const
	{
		static const std::string empty;
		for (const auto& [key, value] : pathParams_)
		{
			if (key == name)
			{
				return value;
			}
		}
		return empty;
	}

	void HttpRequest::setParam(const std::string& name, const std::string& value)
	{
		for (auto& [key, val] : pathParams_)
		{
			if (key == name)
			{
				val = value;
				return;
			}
		}
		pathParams_.emplace_back(name, value);
	}

	bool HttpRequest::hasParam(std::string_view name) const
	{
		for (const auto& [key, value] : pathParams_)
		{
			if (key == name)
			{
				return true;
			}
		}
		return false;
	}

	// ============ Cookie ============

	void HttpRequest::parseCookies() const
	{
		cookies_.emplace();
		auto cookieHeader = header("Cookie");
		if (cookieHeader.empty())
		{
			return;
		}

		// 解析 "name1=value1; name2=value2; ..." 格式
		std::string_view sv(cookieHeader);
		while (!sv.empty())
		{
			// 跳过前导空格
			while (!sv.empty() && sv.front() == ' ')
			{
				sv.remove_prefix(1);
			}

			// 查找分隔符 ';'
			auto semi = sv.find(';');
			std::string_view pair = (semi != std::string_view::npos) ? sv.substr(0, semi) : sv;
			sv = (semi != std::string_view::npos) ? sv.substr(semi + 1) : std::string_view {};

			// 分割 name=value
			auto eq = pair.find('=');
			if (eq == std::string_view::npos)
			{
				continue;
			}
			std::string_view name = pair.substr(0, eq);
			std::string_view value = pair.substr(eq + 1);

			// 去除 name 首尾空格
			while (!name.empty() && name.front() == ' ')
			{
				name.remove_prefix(1);
			}
			while (!name.empty() && name.back() == ' ')
			{
				name.remove_suffix(1);
			}

			if (!name.empty())
			{
				// RFC 6265：同名 Cookie 以先出现的值为准（first-wins）
				(*cookies_).try_emplace(std::string(name), std::string(value));
			}
		}
	}

	const std::string& HttpRequest::cookie(std::string_view name) const
	{
		static const std::string empty;
		if (!cookies_)
		{
			parseCookies();
		}
		if (auto it = cookies_->find(name); it != cookies_->end())
		{
			return it->second;
		}
		return empty;
	}

	const std::unordered_map<std::string, std::string, StringHash, StringEqual>& HttpRequest::cookies() const
	{
		if (!cookies_)
		{
			parseCookies();
		}
		return *cookies_;
	}

	bool HttpRequest::hasCookie(std::string_view name) const
	{
		if (!cookies_)
		{
			parseCookies();
		}
		return cookies_->count(name) > 0;
	}

	// ============ 查询参数 ============

	void HttpRequest::parseUrlEncoded(std::string_view input,
									  std::unordered_multimap<std::string, std::string, StringHash, StringEqual>& out)
	{
		while (!input.empty())
		{
			auto amp = input.find('&');
			std::string_view pair = (amp != std::string_view::npos) ? input.substr(0, amp) : input;
			input = (amp != std::string_view::npos) ? input.substr(amp + 1) : std::string_view {};

			if (pair.empty())
			{
				continue;
			}

			auto eq = pair.find('=');
			std::string_view rawKey;
			std::string_view rawValue;
			if (eq != std::string_view::npos)
			{
				rawKey = pair.substr(0, eq);
				rawValue = pair.substr(eq + 1);
			}
			else
			{
				rawKey = pair;
			}

			if (rawKey.empty())
			{
				continue;
			}

			out.emplace(Router::urlDecode(rawKey), Router::urlDecode(rawValue));
		}
	}

	void HttpRequest::parseQueryParams() const
	{
		queryParams_.emplace();
		auto q = query();
		if (!q.empty())
		{
			parseUrlEncoded(q, *queryParams_);
		}
	}

	std::optional<std::string> HttpRequest::queryParam(std::string_view name) const
	{
		if (!queryParams_)
		{
			parseQueryParams();
		}
		if (auto it = queryParams_->find(name); it != queryParams_->end())
		{
			return it->second;
		}
		return std::nullopt;
	}

	const std::unordered_multimap<std::string, std::string, StringHash, StringEqual>& HttpRequest::queryParams() const
	{
		if (!queryParams_)
		{
			parseQueryParams();
		}
		return *queryParams_;
	}

	bool HttpRequest::hasQueryParam(std::string_view name) const
	{
		if (!queryParams_)
		{
			parseQueryParams();
		}
		return queryParams_->count(name) > 0;
	}

	// ============ 表单参数 ============

	void HttpRequest::parseFormParams() const
	{
		formParams_.emplace();
		auto ct = contentType();
		if (ct.find("application/x-www-form-urlencoded") != std::string_view::npos)
		{
			auto b = body();
			if (!b.empty())
			{
				parseUrlEncoded(b, *formParams_);
			}
		}
	}

	std::optional<std::string> HttpRequest::formParam(std::string_view name) const
	{
		if (!formParams_)
		{
			parseFormParams();
		}
		if (auto it = formParams_->find(name); it != formParams_->end())
		{
			return it->second;
		}
		return std::nullopt;
	}

	const std::unordered_multimap<std::string, std::string, StringHash, StringEqual>& HttpRequest::formParams() const
	{
		if (!formParams_)
		{
			parseFormParams();
		}
		return *formParams_;
	}

	bool HttpRequest::hasFormParam(std::string_view name) const
	{
		if (!formParams_)
		{
			parseFormParams();
		}
		return formParams_->count(name) > 0;
	}

	// ============ 请求级属性 ============

	void HttpRequest::setAttribute(std::string_view key, std::any value)
	{
		if (!attributes_)
		{
			attributes_ = std::make_unique<std::unordered_map<std::string, std::any, StringHash, StringEqual>>();
		}
		if (auto it = attributes_->find(key); it != attributes_->end())
		{
			it->second = std::move(value);
		}
		else
		{
			attributes_->emplace(std::string(key), std::move(value));
		}
	}

	std::optional<std::any> HttpRequest::getAttribute(std::string_view key) const
	{
		if (!attributes_)
		{
			return std::nullopt;
		}
		if (auto it = attributes_->find(key); it != attributes_->end())
		{
			return it->second;
		}
		return std::nullopt;
	}

	// ============ HttpTypes 实用函数 ============

	const char* httpMethodToString(HttpMethod method)
	{
		switch (method)
		{
			case HttpMethod::hGet:
				return "GET";
			case HttpMethod::hPost:
				return "POST";
			case HttpMethod::hPut:
				return "PUT";
			case HttpMethod::hDelete:
				return "DELETE";
			case HttpMethod::hPatch:
				return "PATCH";
			case HttpMethod::hHead:
				return "HEAD";
			case HttpMethod::hOptions:
				return "OPTIONS";
			case HttpMethod::hUnknown:
				return "UNKNOWN";
		}
		return "UNKNOWN";
	}

	HttpMethod stringToHttpMethod(const std::string& str)
	{
		if (str == "GET")
		{
			return HttpMethod::hGet;
		}
		if (str == "POST")
		{
			return HttpMethod::hPost;
		}
		if (str == "PUT")
		{
			return HttpMethod::hPut;
		}
		if (str == "DELETE")
		{
			return HttpMethod::hDelete;
		}
		if (str == "PATCH")
		{
			return HttpMethod::hPatch;
		}
		if (str == "HEAD")
		{
			return HttpMethod::hHead;
		}
		if (str == "OPTIONS")
		{
			return HttpMethod::hOptions;
		}
		return HttpMethod::hUnknown;
	}

	const char* httpStatusCodeToString(HttpStatusCode code)
	{
		switch (code)
		{
			case HttpStatusCode::hOk:
				return "OK";
			case HttpStatusCode::hCreated:
				return "Created";
			case HttpStatusCode::hAccepted:
				return "Accepted";
			case HttpStatusCode::hNoContent:
				return "No Content";
			case HttpStatusCode::hPartialContent:
				return "Partial Content";
			case HttpStatusCode::hMovedPermanently:
				return "Moved Permanently";
			case HttpStatusCode::hFound:
				return "Found";
			case HttpStatusCode::hNotModified:
				return "Not Modified";
			case HttpStatusCode::hTemporaryRedirect:
				return "Temporary Redirect";
			case HttpStatusCode::hPermanentRedirect:
				return "Permanent Redirect";
			case HttpStatusCode::hBadRequest:
				return "Bad Request";
			case HttpStatusCode::hUnauthorized:
				return "Unauthorized";
			case HttpStatusCode::hForbidden:
				return "Forbidden";
			case HttpStatusCode::hNotFound:
				return "Not Found";
			case HttpStatusCode::hMethodNotAllowed:
				return "Method Not Allowed";
			case HttpStatusCode::hConflict:
				return "Conflict";
			case HttpStatusCode::hTooManyRequests:
				return "Too Many Requests";
			case HttpStatusCode::hPayloadTooLarge:
				return "Payload Too Large";
			case HttpStatusCode::hRequestedRangeNotSatisfiable:
				return "Range Not Satisfiable";
			case HttpStatusCode::hInternalServerError:
				return "Internal Server Error";
			case HttpStatusCode::hNotImplemented:
				return "Not Implemented";
			case HttpStatusCode::hBadGateway:
				return "Bad Gateway";
			case HttpStatusCode::hServiceUnavailable:
				return "Service Unavailable";
		}
		return "Unknown";
	}

} // namespace hical
