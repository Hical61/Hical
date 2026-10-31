/**
 * @file Http2Headers.cpp
 * @brief HTTP/2 字段校验、Cookie 合并和稳定头部视图。
 */

#include "Http2Headers.h"

#ifdef HICAL_HAS_HTTP2

	#include "core/HeaderMap.h"
	#include <boost/asio/ip/address_v6.hpp>
	#include <algorithm>
	#include <array>
	#include <charconv>
	#include <limits>
	#include <stdexcept>
	#include <string>

namespace hical::detail
{
	namespace
	{
		constexpr size_t kNotFound = Http2Options::kMaxHeaderFields;
		constexpr uint16_t kHttpPort = 80;
		constexpr uint16_t kHttpsPort = 443;
		constexpr uint32_t kMaxPort = 65535;
		constexpr std::string_view kConnectionFields[] {"connection",
														"keep-alive",
														"proxy-connection",
														"transfer-encoding",
														"upgrade"};
		constexpr std::string_view kForbiddenTrailers[] {"content-length",
														 "host",
														 "te",
														 "trailer",
														 "authorization",
														 "proxy-authorization",
														 "proxy-authenticate",
														 "www-authenticate",
														 "cookie",
														 "set-cookie",
														 "content-encoding",
														 "content-type",
														 "content-range",
														 "content-location",
														 "range",
														 "expect",
														 "max-forwards",
														 "cache-control",
														 "age",
														 "expires",
														 "date",
														 "location",
														 "retry-after",
														 "vary"};

		bool alpha(unsigned char value) noexcept
		{
			return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z');
		}

		bool digit(unsigned char value) noexcept
		{
			return value >= '0' && value <= '9';
		}

		bool token(std::string_view text, bool lowercaseOnly) noexcept
		{
			static constexpr std::string_view kSpecial = "!#$%&'*+-.^_`|~";
			return !text.empty()
				   && std::ranges::all_of(text,
										  [lowercaseOnly](unsigned char value)
										  {
											  return (alpha(value) && (!lowercaseOnly || value < 'A' || value > 'Z'))
													 || digit(value)
													 || kSpecial.find(static_cast<char>(value))
															!= std::string_view::npos;
										  });
		}

		bool validValue(std::string_view value) noexcept
		{
			if (!value.empty()
				&& (value.front() == ' ' || value.front() == '\t' || value.back() == ' ' || value.back() == '\t'))
			{
				return false;
			}
			return std::ranges::all_of(value,
									   [](unsigned char byte)
									   {
										   return (byte >= 0x20 && byte != 0x7f) || byte == '\t';
									   });
		}

		std::optional<uint64_t> unsignedDecimal(std::string_view value) noexcept
		{
			if (value.empty() || !std::ranges::all_of(value, digit))
			{
				return std::nullopt;
			}
			uint64_t number = 0;
			const auto result = std::from_chars(value.data(), value.data() + value.size(), number);
			if (result.ec != std::errc {} || result.ptr != value.data() + value.size()
				|| number > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
			{
				return std::nullopt;
			}
			return number;
		}

		bool validScheme(std::string_view scheme) noexcept
		{
			return !scheme.empty() && alpha(static_cast<unsigned char>(scheme.front()))
				   && std::ranges::all_of(scheme,
										  [](unsigned char value)
										  {
											  return alpha(value) || digit(value) || value == '+' || value == '-'
													 || value == '.';
										  });
		}

		struct Authority
		{
			std::string_view host;
			std::optional<uint16_t> port;
			std::optional<boost::asio::ip::address_v6::bytes_type> ipv6;
		};

		std::optional<Authority> parseAuthority(std::string_view value) noexcept
		{
			if (value.empty() || value.find_first_of("@/?#\\") != std::string_view::npos)
			{
				return std::nullopt;
			}
			Authority result;
			std::string_view port;
			bool hasPort = false;
			if (value.front() == '[')
			{
				const auto end = value.find(']');
				if (end == std::string_view::npos || end == 1
					|| value.substr(1, end - 1).find('%') != std::string_view::npos)
				{
					return std::nullopt;
				}
				result.host = value.substr(0, end + 1);
				boost::system::error_code error;
				const auto address = boost::asio::ip::make_address_v6(value.substr(1, end - 1), error);
				if (error)
				{
					return std::nullopt;
				}
				result.ipv6 = address.to_bytes();
				if (end + 1 != value.size())
				{
					if (value[end + 1] != ':')
					{
						return std::nullopt;
					}
					hasPort = true;
					port = value.substr(end + 2);
				}
			}
			else
			{
				const auto separator = value.find(':');
				if (separator != std::string_view::npos)
				{
					if (value.find(':', separator + 1) != std::string_view::npos)
					{
						return std::nullopt;
					}
					hasPort = true;
					port = value.substr(separator + 1);
				}
				result.host = value.substr(0, separator);
				static constexpr std::string_view kHostSpecial = "!$%&'()*+,-.;=_~";
				if (result.host.empty()
					|| !std::ranges::all_of(result.host,
											[](unsigned char byte)
											{
												return alpha(byte) || digit(byte)
													   || kHostSpecial.find(static_cast<char>(byte))
															  != std::string_view::npos;
											}))
				{
					return std::nullopt;
				}
			}
			if (hasPort)
			{
				const auto parsed = unsignedDecimal(port);
				if (!parsed || *parsed > kMaxPort)
				{
					return std::nullopt;
				}
				result.port = static_cast<uint16_t>(*parsed);
			}
			return result;
		}

		bool sameAuthority(Authority first, Authority second, std::string_view scheme) noexcept
		{
			if (first.ipv6 != second.ipv6 || (!first.ipv6 && !HeaderMap::iequals(first.host, second.host)))
			{
				return false;
			}
			const auto defaultPort = HeaderMap::iequals(scheme, "https")  ? std::optional<uint16_t> {kHttpsPort}
									 : HeaderMap::iequals(scheme, "http") ? std::optional<uint16_t> {kHttpPort}
																		  : std::nullopt;
			return (first.port ? first.port : defaultPort) == (second.port ? second.port : defaultPort);
		}
	} // namespace

	struct Http2Headers::Storage
	{
		struct Offset
		{
			size_t name;
			size_t nameSize;
			size_t value;
			size_t valueSize;
		};

		Kind kind_;
		int32_t streamId_;
		size_t maxFields_;
		size_t maxBytes_;
		size_t decodedFields_ {0};
		size_t decodedBytes_ {0};
		size_t entryCount_ {0};
		size_t viewCount_ {0};
		std::array<Offset, Http2Options::kMaxHeaderFields> entries_ {};
		std::array<Field, Http2Options::kMaxHeaderFields> views_ {};
		std::string bytes_;
		std::optional<uint64_t> contentLength_;
		uint16_t status_ {0};
		bool regularSeen_ {false};
		bool sealed_ {false};
		NetworkError error_;

		Storage(Kind kind, int32_t streamId, const Http2Options& options)
			: kind_(kind), streamId_(streamId), maxFields_(options.maxHeaderFields), maxBytes_(options.maxHeaderBytes)
		{
			options.validate();
			if (streamId <= 0 || (kind != Kind::hRequest && kind != Kind::hResponse && kind != Kind::hTrailers))
			{
				throw std::invalid_argument("HTTP/2 头部块角色或流 ID 无效");
			}
		}

		Field field(size_t index) const noexcept
		{
			const auto& entry = entries_[index];
			return {{bytes_.data() + entry.name, entry.nameSize}, {bytes_.data() + entry.value, entry.valueSize}};
		}

		size_t findEntry(std::string_view name) const noexcept
		{
			for (size_t index = 0; index < entryCount_; ++index)
			{
				if (field(index).name == name)
				{
					return index;
				}
			}
			return kNotFound;
		}

		std::string_view value(std::string_view name) const noexcept
		{
			const auto index = findEntry(name);
			return index == kNotFound ? std::string_view {} : field(index).value;
		}

		NetworkError fail(std::string_view message, bool resource = false)
		{
			if (!error_)
			{
				error_ = {resource ? ErrorCode::hResourceLimitExceeded : ErrorCode::hHttp2StreamError,
						  std::string(message),
						  Http2ErrorInfo {
							  static_cast<uint32_t>(resource ? Http2Code::hEnhanceYourCalm : Http2Code::hProtocolError),
							  streamId_}};
			}
			return error_;
		}

		NetworkError validatePseudo(std::string_view name, std::string_view value)
		{
			if (regularSeen_ || kind_ == Kind::hTrailers || value.empty() || findEntry(name) != kNotFound)
			{
				return fail("HTTP/2 伪头的顺序、重复或所属头部块无效");
			}
			if (kind_ == Kind::hResponse)
			{
				if (name != ":status" || value.size() != 3 || !std::ranges::all_of(value, digit) || value.front() == '0'
					|| value == "101")
				{
					return fail("HTTP/2 响应需要唯一的三位 :status，不能使用 101");
				}
				return {};
			}
			if (name == ":method" && token(value, false))
			{
				return {};
			}
			if (name == ":scheme" && validScheme(value))
			{
				return {};
			}
			if (name == ":authority" && parseAuthority(value))
			{
				return {};
			}
			if (name == ":path" && value.find('#') == std::string_view::npos
				&& std::ranges::all_of(value,
									   [](unsigned char byte)
									   {
										   return byte > 0x20 && byte != 0x7f;
									   }))
			{
				return {};
			}
			return fail("HTTP/2 请求包含未知或无效的伪头");
		}

		NetworkError validateRegular(std::string_view name, std::string_view value)
		{
			if (std::ranges::find(kConnectionFields, name) != std::end(kConnectionFields))
			{
				return fail("HTTP/2 不允许连接专用字段");
			}
			if (kind_ == Kind::hTrailers
				&& (name.starts_with("if-")
					|| std::ranges::find(kForbiddenTrailers, name) != std::end(kForbiddenTrailers)))
			{
				return fail("HTTP/2 尾头不能改变路由、消息分界、认证或内容处理");
			}
			if (name == "te" && (kind_ != Kind::hRequest || !HeaderMap::iequals(value, "trailers")))
			{
				return fail("HTTP/2 TE 仅允许初始请求中的 trailers");
			}
			if (name == "content-length" && (contentLength_ || !unsignedDecimal(value)))
			{
				return fail("HTTP/2 Content-Length 重复或不是有效的非负长度");
			}
			if (kind_ == Kind::hRequest && name == "host" && (findEntry(name) != kNotFound || !parseAuthority(value)))
			{
				return fail("HTTP/2 Host 重复或 authority 格式无效");
			}
			return {};
		}

		NetworkError append(std::string_view name, std::string_view value)
		{
			if (sealed_)
			{
				return {ErrorCode::hInvalidConfiguration, "HTTP/2 头部块封存后不能追加"};
			}
			if (error_)
			{
				return error_;
			}
			const auto remaining = maxBytes_ - decodedBytes_;
			if (decodedFields_ >= maxFields_ || Http2Options::kHeaderFieldOverheadBytes > remaining
				|| name.size() > remaining - Http2Options::kHeaderFieldOverheadBytes
				|| value.size() > remaining - Http2Options::kHeaderFieldOverheadBytes - name.size())
			{
				return fail("HTTP/2 解码字段数量或 header-list 字节数超过预算", true);
			}
			const bool pseudo = name.starts_with(':');
			if (!token(pseudo ? name.substr(1) : name, true) || !validValue(value))
			{
				return fail("HTTP/2 字段名或字段值格式无效");
			}
			if (auto error = pseudo ? validatePseudo(name, value) : validateRegular(name, value))
			{
				return error;
			}
			// 先 reserve，再更新偏移和额度，分配失败时不留下半个字段。
			bytes_.reserve(bytes_.size() + name.size() + value.size());
			const size_t nameOffset = bytes_.size();
			bytes_.append(name);
			const size_t valueOffset = bytes_.size();
			bytes_.append(value);
			entries_[entryCount_++] = {nameOffset, name.size(), valueOffset, value.size()};
			++decodedFields_;
			decodedBytes_ += Http2Options::kHeaderFieldOverheadBytes + name.size() + value.size();
			regularSeen_ = regularSeen_ || !pseudo;
			if (name == "content-length")
			{
				contentLength_ = unsignedDecimal(value);
			}
			return {};
		}

		NetworkError validateComplete()
		{
			if (kind_ == Kind::hResponse)
			{
				const auto code = unsignedDecimal(value(":status"));
				if (!code)
				{
					return fail("HTTP/2 响应缺少 :status");
				}
				status_ = static_cast<uint16_t>(*code);
				if (status_ / 100 == 1 && contentLength_)
				{
					return fail("HTTP/2 信息响应不能携带 Content-Length");
				}
				return {};
			}
			if (kind_ == Kind::hTrailers)
			{
				return {};
			}
			const auto method = value(":method");
			const auto scheme = value(":scheme");
			const auto path = value(":path");
			const auto authority = value(":authority");
			const auto host = value("host");
			const bool connect = method == "CONNECT";
			if (method.empty() || (connect && (authority.empty() || !scheme.empty() || !path.empty()))
				|| (!connect && (scheme.empty() || path.empty())))
			{
				return fail("HTTP/2 请求缺少必要伪头，或普通 CONNECT 携带了 scheme/path");
			}
			const bool httpScheme = HeaderMap::iequals(scheme, "http") || HeaderMap::iequals(scheme, "https");
			if (!connect && httpScheme
				&& ((authority.empty() && host.empty())
					|| (path.front() != '/' && !(method == "OPTIONS" && path == "*"))))
			{
				return fail("HTTP/2 HTTP 请求缺少 authority，或 :path 不是合法请求目标");
			}
			const auto parsedAuthority = parseAuthority(authority);
			if (connect && (!parsedAuthority || !parsedAuthority->port))
			{
				return fail("HTTP/2 普通 CONNECT 的 authority 必须包含端口");
			}
			if (!authority.empty() && !host.empty() && !sameAuthority(*parsedAuthority, *parseAuthority(host), scheme))
			{
				return fail("HTTP/2 :authority 与 Host 标识了不同目标");
			}
			return {};
		}

		void combineCookies()
		{
			if (kind_ != Kind::hRequest || findEntry("cookie") == kNotFound)
			{
				return;
			}
			const auto firstCookie = findEntry("cookie");
			bool multiple = false;
			for (size_t index = firstCookie + 1; index < entryCount_; ++index)
			{
				if (field(index).name == "cookie")
				{
					multiple = true;
					break;
				}
			}
			if (!multiple)
			{
				return;
			}
			std::array<Offset, Http2Options::kMaxHeaderFields> normalized {};
			std::string bytes;
			bytes.reserve(bytes_.size());
			size_t count = 0;
			bool combined = false;
			for (size_t index = 0; index < entryCount_; ++index)
			{
				const auto current = field(index);
				if (current.name == "cookie" && combined)
				{
					continue;
				}
				const auto nameOffset = bytes.size();
				bytes.append(current.name);
				const auto valueOffset = bytes.size();
				if (current.name == "cookie")
				{
					bool first = true;
					for (size_t cookieIndex = index; cookieIndex < entryCount_; ++cookieIndex)
					{
						const auto cookie = field(cookieIndex);
						if (cookie.name != "cookie")
						{
							continue;
						}
						if (!first)
						{
							bytes.append("; ");
						}
						bytes.append(cookie.value);
						first = false;
					}
					combined = true;
				}
				else
				{
					bytes.append(current.value);
				}
				normalized[count++] = {nameOffset, current.name.size(), valueOffset, bytes.size() - valueOffset};
			}
			bytes_ = std::move(bytes);
			entries_ = normalized;
			entryCount_ = count;
		}

		NetworkError seal()
		{
			if (error_ || sealed_)
			{
				return error_;
			}
			if (auto error = validateComplete())
			{
				return error;
			}
			combineCookies();
			const auto authority = value(":authority");
			bool hasHost = false;
			for (size_t index = 0; index < entryCount_; ++index)
			{
				auto current = field(index);
				if (current.name.starts_with(':'))
				{
					continue;
				}
				if (kind_ == Kind::hRequest && current.name == "host")
				{
					hasHost = true;
					if (!authority.empty())
					{
						current.value = authority;
					}
				}
				views_[viewCount_++] = current;
			}
			if (kind_ == Kind::hRequest && !hasHost && !authority.empty())
			{
				// 至少有 :authority 伪头占一个原始名额，合成 Host 不会突破 64 项视图。
				views_[viewCount_++] = {"host", authority};
			}
			sealed_ = true;
			return {};
		}
	};

	Http2Headers::Http2Headers(Kind kind, int32_t streamId, const Http2Options& options)
		: storage_(std::make_unique<Storage>(kind, streamId, options))
	{
	}

	Http2Headers::~Http2Headers() = default;
	Http2Headers::Http2Headers(Http2Headers&&) noexcept = default;
	Http2Headers& Http2Headers::operator=(Http2Headers&&) noexcept = default;

	NetworkError Http2Headers::append(std::string_view name, std::string_view value)
	{
		return storage_ ? storage_->append(name, value)
						: NetworkError {ErrorCode::hInvalidConfiguration, "HTTP/2 头部块所有权已经移走"};
	}

	NetworkError Http2Headers::seal()
	{
		return storage_ ? storage_->seal()
						: NetworkError {ErrorCode::hInvalidConfiguration, "HTTP/2 头部块所有权已经移走"};
	}

	bool Http2Headers::sealed() const noexcept
	{
		return storage_ && storage_->sealed_;
	}

	std::span<const Http2Headers::Field> Http2Headers::fields() const noexcept
	{
		return sealed() ? std::span<const Field> {storage_->views_.data(), storage_->viewCount_}
						: std::span<const Field> {};
	}

	std::string_view Http2Headers::find(std::string_view name) const noexcept
	{
		for (const auto& field : fields())
		{
			if (HeaderMap::iequals(name, field.name))
			{
				return field.value;
			}
		}
		return {};
	}

	std::string_view Http2Headers::rawMethod() const noexcept
	{
		return sealed() && storage_->kind_ == Kind::hRequest ? storage_->value(":method") : std::string_view {};
	}

	std::string_view Http2Headers::target() const noexcept
	{
		return sealed() && storage_->kind_ == Kind::hRequest ? (isConnect() ? authority() : storage_->value(":path"))
															 : std::string_view {};
	}

	std::string_view Http2Headers::scheme() const noexcept
	{
		return sealed() && storage_->kind_ == Kind::hRequest ? storage_->value(":scheme") : std::string_view {};
	}

	std::string_view Http2Headers::authority() const noexcept
	{
		if (!sealed() || storage_->kind_ != Kind::hRequest)
		{
			return {};
		}
		const auto authority = storage_->value(":authority");
		return authority.empty() ? storage_->value("host") : authority;
	}

	uint16_t Http2Headers::status() const noexcept
	{
		return sealed() ? storage_->status_ : 0;
	}

	std::optional<uint64_t> Http2Headers::contentLength() const noexcept
	{
		return sealed() ? storage_->contentLength_ : std::nullopt;
	}

	bool Http2Headers::isConnect() const noexcept
	{
		return rawMethod() == "CONNECT";
	}

	size_t Http2Headers::decodedFieldCount() const noexcept
	{
		return storage_ ? storage_->decodedFields_ : 0;
	}

	size_t Http2Headers::decodedBytes() const noexcept
	{
		return storage_ ? storage_->decodedBytes_ : 0;
	}
} // namespace hical::detail

#endif
