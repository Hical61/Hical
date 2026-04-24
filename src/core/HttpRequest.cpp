#include "HttpRequest.h"
#include <boost/beast/http/verb.hpp>

namespace hical
{

	namespace
	{

		/// HTTP Header Injection 防护：检测字符串是否包含 CR/LF（单次遍历）
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
		req_.version(11); // HTTP/1.1
	}

	HttpRequest::HttpRequest(BeastRequest req) : req_(std::move(req))
	{
	}

	HttpMethod HttpRequest::method() const
	{
		switch (req_.method())
		{
			case boost::beast::http::verb::get:
				return HttpMethod::hGet;
			case boost::beast::http::verb::post:
				return HttpMethod::hPost;
			case boost::beast::http::verb::put:
				return HttpMethod::hPut;
			case boost::beast::http::verb::delete_:
				return HttpMethod::hDelete;
			case boost::beast::http::verb::patch:
				return HttpMethod::hPatch;
			case boost::beast::http::verb::head:
				return HttpMethod::hHead;
			case boost::beast::http::verb::options:
				return HttpMethod::hOptions;
			default:
				return HttpMethod::hUnknown;
		}
	}

	std::string_view HttpRequest::path() const
	{
		auto t = req_.target();
		auto pos = t.find('?');
		if (pos != std::string_view::npos)
		{
			return t.substr(0, pos);
		}
		return t;
	}

	std::string_view HttpRequest::target() const
	{
		return req_.target();
	}

	std::string_view HttpRequest::query() const
	{
		auto t = req_.target();
		auto pos = t.find('?');
		if (pos != std::string_view::npos)
		{
			return t.substr(pos + 1);
		}
		return {};
	}

	std::string_view HttpRequest::header(std::string_view name) const
	{
		auto it = req_.find(name);
		if (it != req_.end())
		{
			return it->value();
		}
		return {};
	}

	const std::string& HttpRequest::body() const
	{
		return req_.body();
	}

	const boost::json::value& HttpRequest::jsonBody() const
	{
		if (!cachedJsonBody_)
		{
			boost::system::error_code ec;
			auto val = boost::json::parse(req_.body(), ec);
			cachedJsonBody_.emplace(ec ? boost::json::value(nullptr) : std::move(val));
		}
		return *cachedJsonBody_;
	}

	std::string_view HttpRequest::contentType() const
	{
		return header("Content-Type");
	}

	HttpRequest::BeastRequest& HttpRequest::native()
	{
		return req_;
	}

	const HttpRequest::BeastRequest& HttpRequest::native() const
	{
		return req_;
	}

	void HttpRequest::setMethod(HttpMethod method)
	{
		switch (method)
		{
			case HttpMethod::hGet:
				req_.method(boost::beast::http::verb::get);
				break;
			case HttpMethod::hPost:
				req_.method(boost::beast::http::verb::post);
				break;
			case HttpMethod::hPut:
				req_.method(boost::beast::http::verb::put);
				break;
			case HttpMethod::hDelete:
				req_.method(boost::beast::http::verb::delete_);
				break;
			case HttpMethod::hPatch:
				req_.method(boost::beast::http::verb::patch);
				break;
			case HttpMethod::hHead:
				req_.method(boost::beast::http::verb::head);
				break;
			case HttpMethod::hOptions:
				req_.method(boost::beast::http::verb::options);
				break;
			default:
				break;
		}
	}

	void HttpRequest::setTarget(const std::string& target)
	{
		req_.target(target);
	}

	void HttpRequest::setHeader(const std::string& name, const std::string& value)
	{
		// HTTP Header Injection 防护：拒绝含 CR/LF 的头部
		if (containsCRLF(name) || containsCRLF(value))
		{
			return;
		}
		req_.set(name, value);
	}

	void HttpRequest::setBody(const std::string& body)
	{
		req_.body() = body;
		req_.prepare_payload();
	}

	// ============ 路径参数 ============

	const std::string& HttpRequest::param(const std::string& name) const
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

	bool HttpRequest::hasParam(const std::string& name) const
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

	const std::string& HttpRequest::cookie(const std::string& name) const
	{
		static const std::string empty;
		if (!cookies_)
		{
			parseCookies();
		}
		auto it = cookies_->find(name);
		if (it != cookies_->end())
		{
			return it->second;
		}
		return empty;
	}

	const std::unordered_map<std::string, std::string>& HttpRequest::cookies() const
	{
		if (!cookies_)
		{
			parseCookies();
		}
		return *cookies_;
	}

	bool HttpRequest::hasCookie(const std::string& name) const
	{
		if (!cookies_)
		{
			parseCookies();
		}
		return cookies_->count(name) > 0;
	}

	// ============ 请求级属性 ============

	void HttpRequest::setAttribute(const std::string& key, std::any value)
	{
		attributes_[key] = std::move(value);
	}

	std::optional<std::any> HttpRequest::getAttribute(const std::string& key) const
	{
		auto it = attributes_.find(key);
		if (it == attributes_.end())
		{
			return std::nullopt;
		}
		return it->second;
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
			case HttpStatusCode::hMovedPermanently:
				return "Moved Permanently";
			case HttpStatusCode::hFound:
				return "Found";
			case HttpStatusCode::hNotModified:
				return "Not Modified";
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
