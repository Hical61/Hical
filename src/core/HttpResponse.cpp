#include "HttpResponse.h"
#include <iomanip>
#include <sstream>

namespace hical
{

	HttpResponse::HttpResponse()
	{
		res_.version(11); // HTTP/1.1
		res_.result(boost::beast::http::status::ok);
	}

	HttpResponse::HttpResponse(BeastResponse res) : res_(std::move(res))
	{
	}

	HttpStatusCode HttpResponse::statusCode() const
	{
		return static_cast<HttpStatusCode>(res_.result_int());
	}

	void HttpResponse::setStatus(HttpStatusCode code)
	{
		res_.result(static_cast<unsigned int>(code));
	}

	std::string HttpResponse::header(const std::string& name) const
	{
		auto it = res_.find(name);
		if (it != res_.end())
		{
			return std::string(it->value());
		}
		return "";
	}

	void HttpResponse::setHeader(const std::string& name, const std::string& value)
	{
		res_.set(name, value);
	}

	const std::string& HttpResponse::body() const
	{
		return res_.body();
	}

	void HttpResponse::setBody(const std::string& body, const std::string& contentType)
	{
		res_.body() = body;
		res_.set(boost::beast::http::field::content_type, contentType);
		res_.prepare_payload();
	}

	void HttpResponse::setJsonBody(const boost::json::value& json)
	{
		res_.body() = boost::json::serialize(json);
		res_.set(boost::beast::http::field::content_type, "application/json");
		res_.prepare_payload();
	}

	void HttpResponse::setCookie(const std::string& name, const std::string& value, const CookieOptions& options)
	{
		// HTTP Response Splitting 防护：name/value 不允许包含 CR/LF
		auto containsCRLF = [](const std::string& s) -> bool
		{
			return s.find('\r') != std::string::npos || s.find('\n') != std::string::npos;
		};
		if (containsCRLF(name) || containsCRLF(value))
		{
			// 拒绝含控制字符的 Cookie，静默忽略（生产环境应记录告警日志）
			return;
		}

		// RFC 6265 cookie-value 合法字符百分号编码
		// 合法: %x21 / %x23-2B / %x2D-3A / %x3C-5B / %x5D-7E
		auto encodeCookieValue = [](const std::string& raw) -> std::string
		{
			std::ostringstream encoded;
			encoded << std::hex << std::uppercase;
			for (unsigned char c : raw)
			{
				bool safe = (c == 0x21) || (c >= 0x23 && c <= 0x2B) || (c >= 0x2D && c <= 0x3A)
							|| (c >= 0x3C && c <= 0x5B) || (c >= 0x5D && c <= 0x7E);
				if (safe)
				{
					encoded << static_cast<char>(c);
				}
				else
				{
					encoded << '%' << std::setw(2) << std::setfill('0') << static_cast<int>(c);
				}
			}
			return encoded.str();
		};

		std::ostringstream oss;
		oss << name << "=" << encodeCookieValue(value);

		if (!options.path.empty())
		{
			oss << "; Path=" << options.path;
		}
		if (!options.domain.empty())
		{
			oss << "; Domain=" << options.domain;
		}
		if (options.maxAge >= 0)
		{
			oss << "; Max-Age=" << options.maxAge;
		}
		if (options.httpOnly)
		{
			oss << "; HttpOnly";
		}
		if (options.secure)
		{
			oss << "; Secure";
		}
		if (!options.sameSite.empty())
		{
			oss << "; SameSite=" << options.sameSite;
		}

		// Beast 不支持同名字段多值直接 set，使用 insert 追加多个 Set-Cookie
		res_.insert(boost::beast::http::field::set_cookie, oss.str());
	}

	HttpResponse::BeastResponse& HttpResponse::native()
	{
		return res_;
	}

	const HttpResponse::BeastResponse& HttpResponse::native() const
	{
		return res_;
	}

	// ============ 快捷工厂方法 ============

	HttpResponse HttpResponse::ok(const std::string& body)
	{
		HttpResponse res;
		res.setStatus(HttpStatusCode::hOk);
		if (!body.empty())
		{
			res.setBody(body);
		}
		return res;
	}

	HttpResponse HttpResponse::json(const boost::json::value& json)
	{
		HttpResponse res;
		res.setStatus(HttpStatusCode::hOk);
		res.setJsonBody(json);
		return res;
	}

	HttpResponse HttpResponse::notFound()
	{
		HttpResponse res;
		res.setStatus(HttpStatusCode::hNotFound);
		res.setBody("Not Found");
		return res;
	}

	HttpResponse HttpResponse::badRequest(const std::string& message)
	{
		HttpResponse res;
		res.setStatus(HttpStatusCode::hBadRequest);
		res.setBody(message);
		return res;
	}

	HttpResponse HttpResponse::serverError()
	{
		HttpResponse res;
		res.setStatus(HttpStatusCode::hInternalServerError);
		res.setBody("Internal Server Error");
		return res;
	}

} // namespace hical
