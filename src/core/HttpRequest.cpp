#include "HttpRequest.h"
#include <boost/beast/http/verb.hpp>

namespace hical
{

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

	std::string HttpRequest::header(const std::string& name) const
	{
		auto it = req_.find(name);
		if (it != req_.end())
		{
			return std::string(it->value());
		}
		return "";
	}

	const std::string& HttpRequest::body() const
	{
		return req_.body();
	}

	boost::json::value HttpRequest::jsonBody() const
	{
		boost::system::error_code ec;
		auto val = boost::json::parse(req_.body(), ec);
		if (ec)
		{
			return nullptr;
		}
		return val;
	}

	std::string HttpRequest::contentType() const
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
		req_.set(name, value);
	}

	void HttpRequest::setBody(const std::string& body)
	{
		req_.body() = body;
		req_.prepare_payload();
	}

	// ============ 路径参数 ============

	std::string HttpRequest::param(const std::string& name) const
	{
		auto it = pathParams_.find(name);
		if (it != pathParams_.end())
		{
			return it->second;
		}
		return "";
	}

	void HttpRequest::setParam(const std::string& name, const std::string& value)
	{
		pathParams_[name] = value;
	}

	bool HttpRequest::hasParam(const std::string& name) const
	{
		return pathParams_.count(name) > 0;
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
