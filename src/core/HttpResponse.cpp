/**
 * @file HttpResponse.cpp
 * @brief HTTP 响应序列化实现
 */

#include "HttpResponse.h"

namespace hical
{

	namespace
	{

		/// 检测 CR/LF（防 Response Splitting）
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

		bool containsCRLF(std::string_view s)
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

		/// cookie-name 只能包含 token 字符 (RFC 2616)
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
		res_.invalidatePayload();
		res_.status = code;
	}

	std::string_view HttpResponse::header(std::string_view name) const
	{
		return res_.headers.find(name);
	}

	void HttpResponse::setHeader(std::string_view name, std::string_view value)
	{
		// 防 Response Splitting
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

	void HttpResponse::setBody(const std::string& body)
	{
		res_.invalidatePayload();
		res_.body = body;
		res_.preparePayload();
	}

	void HttpResponse::setBody(std::string&& body)
	{
		res_.invalidatePayload();
		res_.body = std::move(body);
		res_.preparePayload();
	}

	void HttpResponse::setBody(const std::string& body, const std::string& contentType)
	{
		// contentType 也查 CR/LF
		if (containsCRLF(contentType))
		{
			return;
		}
		res_.invalidatePayload();
		res_.body = body;
		res_.headers.set("Content-Type", contentType);
		res_.preparePayload();
	}

	void HttpResponse::setBody(std::string&& body, const std::string& contentType)
	{
		if (containsCRLF(contentType))
		{
			return;
		}
		res_.invalidatePayload();
		res_.body = std::move(body);
		res_.headers.set("Content-Type", contentType);
		res_.preparePayload();
	}

	void HttpResponse::setJsonBody(const boost::json::value& json)
	{
		res_.invalidatePayload();
		res_.body = boost::json::serialize(json);
		res_.headers.set("Content-Type", "application/json");
		res_.preparePayload();
	}

	void HttpResponse::setCookie(const std::string& name, const std::string& value, const CookieOptions& options)
	{
		// cookie name 必须是 token 字符
		if (!isValidCookieName(name))
		{
			return;
		}

		// value 不能有 CR/LF
		if (containsCRLF(value))
		{
			return;
		}

		// path/domain/sameSite 也查
		if (containsCRLF(options.path) || containsCRLF(options.domain) || containsCRLF(options.sameSite))
		{
			return;
		}

		// cookie value 中非法字符做百分号编码
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
			res.setBody(body, "text/plain; charset=utf-8");
		}
		return res;
	}

	HttpResponse HttpResponse::ok(const std::string& body, const std::string& contentType)
	{
		HttpResponse res;
		res.setStatus(HttpStatusCode::hOk);
		res.setBody(body, contentType);
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
		res.setBody("Not Found", "text/plain");
		res.setHeader("X-Content-Type-Options", "nosniff");
		return res;
	}

	HttpResponse HttpResponse::badRequest(const std::string& message)
	{
		HttpResponse res;
		res.setStatus(HttpStatusCode::hBadRequest);
		res.setBody(message, "text/plain");
		res.setHeader("X-Content-Type-Options", "nosniff");
		return res;
	}

	HttpResponse HttpResponse::serverError()
	{
		HttpResponse res;
		res.setStatus(HttpStatusCode::hInternalServerError);
		res.setBody("Internal Server Error", "text/plain");
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

	void HttpResponse::setFileBody(const std::filesystem::path& path,
								   int64_t offset,
								   int64_t length,
								   const std::string& contentType)
	{
		if (containsCRLF(contentType))
		{
			return;
		}
		res_.invalidatePayload();
		res_.fileBody = FileBody {path, offset, length};
		res_.body.clear();
		res_.headers.set("Content-Type", contentType);
	}

	bool HttpResponse::hasFileBody() const
	{
		return res_.hasFileBody();
	}

	HttpResponse HttpResponse::rangeNotSatisfiable(std::uintmax_t fileSize)
	{
		HttpResponse res;
		res.setStatus(HttpStatusCode::hRequestedRangeNotSatisfiable);
		res.setHeader("Content-Range", "bytes */" + std::to_string(fileSize));
		res.setBody("416 Range Not Satisfiable", "text/plain");
		return res;
	}

	HttpResponse HttpResponse::chunked()
	{
		HttpResponse res;
		res.setStatus(HttpStatusCode::hOk);
		res.res_.chunkedBody.emplace();
		res.res_.headers.set("Content-Type", "text/plain; charset=utf-8");
		return res;
	}

	ChunkedBody& HttpResponse::chunkedBody()
	{
		return res_.chunkedBody.value();
	}

} // namespace hical
