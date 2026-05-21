#include "HttpResponse.h"

namespace hical
{

	namespace
	{

		/// HTTP Response Splitting 防护：检测字符串是否包含 CR/LF（单次遍历）
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

		/// RFC 6265 cookie-name 合法性校验：必须是 RFC 2616 token 字符
		/// token = 1*<any CHAR except CTLs or separators>
		/// separators = "(" | ")" | "<" | ">" | "@" | "," | ";" | ":" | "\" | <"> | "/" | "[" | "]" | "?" | "="
		///              | "{" | "}" | SP | HT
		bool isValidCookieName(const std::string& name)
		{
			if (name.empty())
			{
				return false;
			}
			for (unsigned char c : name)
			{
				// CTLs (0-31, 127) 和分隔符
				if (c <= 0x20 || c == 0x7F || c == '(' || c == ')' || c == '<' || c == '>' || c == '@' || c == ','
					|| c == ';' || c == ':' || c == '\\' || c == '"' || c == '/' || c == '[' || c == ']' || c == '?'
					|| c == '=' || c == '{' || c == '}')
				{
					return false;
				}
			}
			return true;
		}

	} // namespace

	HttpResponse::HttpResponse()
	{
		res_.httpVersionMinor = 1;
		res_.status = HttpStatusCode::hOk;
	}

	HttpStatusCode HttpResponse::statusCode() const
	{
		return res_.status;
	}

	void HttpResponse::setStatus(HttpStatusCode code)
	{
		res_.status = code;
	}

	std::string_view HttpResponse::header(std::string_view name) const
	{
		return res_.headers.find(name);
	}

	void HttpResponse::setHeader(const std::string& name, const std::string& value)
	{
		// HTTP Response Splitting 防护：拒绝含 CR/LF 的头部
		if (containsCRLF(name) || containsCRLF(value))
		{
			return;
		}
		res_.headers.set(name, value);
	}

	const std::string& HttpResponse::body() const
	{
		return res_.body;
	}

	void HttpResponse::setBody(const std::string& body, const std::string& contentType)
	{
		// HTTP Response Splitting 防护：contentType 也需检查 CR/LF
		if (containsCRLF(contentType))
		{
			return;
		}
		res_.body = body;
		res_.headers.set("Content-Type", contentType);
		res_.preparePayload();
	}

	void HttpResponse::setJsonBody(const boost::json::value& json)
	{
		res_.body = boost::json::serialize(json);
		res_.headers.set("Content-Type", "application/json");
		res_.preparePayload();
	}

	void HttpResponse::setCookie(const std::string& name, const std::string& value, const CookieOptions& options)
	{
		// RFC 6265 cookie-name 合法性校验：必须是 token 字符（含 CRLF 检测）
		if (!isValidCookieName(name))
		{
			return;
		}

		// HTTP Response Splitting 防护：value 不允许包含 CR/LF
		if (containsCRLF(value))
		{
			return;
		}

		// HTTP Response Splitting 防护：path/domain/sameSite 不允许包含 CR/LF
		if (containsCRLF(options.path) || containsCRLF(options.domain) || containsCRLF(options.sameSite))
		{
			return;
		}

		// RFC 6265 cookie-value 合法字符百分号编码
		// 合法: %x21 / %x23-2B / %x2D-3A / %x3C-5B / %x5D-7E
		static constexpr char kHexDigits[] = "0123456789ABCDEF";
		auto encodeCookieValue = [](const std::string& raw) -> std::string
		{
			std::string encoded;
			encoded.reserve(raw.size());
			for (unsigned char c : raw)
			{
				bool safe = (c == 0x21) || (c >= 0x23 && c <= 0x2B) || (c >= 0x2D && c <= 0x3A)
							|| (c >= 0x3C && c <= 0x5B) || (c >= 0x5D && c <= 0x7E);
				if (safe)
				{
					encoded += static_cast<char>(c);
				}
				else
				{
					encoded += '%';
					encoded += kHexDigits[c >> 4];
					encoded += kHexDigits[c & 0x0F];
				}
			}
			return encoded;
		};

		std::string cookie;
		cookie.reserve(name.size() + value.size() * 3 + 128);
		cookie += name;
		cookie += '=';
		cookie += encodeCookieValue(value);

		if (!options.path.empty())
		{
			cookie += "; Path=";
			cookie += options.path;
		}
		if (!options.domain.empty())
		{
			cookie += "; Domain=";
			cookie += options.domain;
		}
		if (options.maxAge >= 0)
		{
			cookie += "; Max-Age=";
			cookie += std::to_string(options.maxAge);
		}
		if (options.httpOnly)
		{
			cookie += "; HttpOnly";
		}
		if (options.secure)
		{
			cookie += "; Secure";
		}
		if (!options.sameSite.empty())
		{
			cookie += "; SameSite=";
			cookie += options.sameSite;
		}

		// insert 追加多个 Set-Cookie（不覆写）
		res_.headers.insert("Set-Cookie", cookie);
	}

	NativeResponse& HttpResponse::native()
	{
		return res_;
	}

	const NativeResponse& HttpResponse::native() const
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
		res.setHeader("X-Content-Type-Options", "nosniff");
		return res;
	}

	HttpResponse HttpResponse::badRequest(const std::string& message)
	{
		HttpResponse res;
		res.setStatus(HttpStatusCode::hBadRequest);
		res.setBody(message);
		res.setHeader("X-Content-Type-Options", "nosniff");
		return res;
	}

	HttpResponse HttpResponse::serverError()
	{
		HttpResponse res;
		res.setStatus(HttpStatusCode::hInternalServerError);
		res.setBody("Internal Server Error");
		res.setHeader("X-Content-Type-Options", "nosniff");
		return res;
	}

	HttpResponse HttpResponse::redirect(const std::string& location, HttpStatusCode code)
	{
		HttpResponse res;
		res.setStatus(code);
		// Location 头经过 CRLF 注入检查（setHeader 内部处理）
		res.setHeader("Location", location);
		res.setBody("");
		return res;
	}

} // namespace hical
