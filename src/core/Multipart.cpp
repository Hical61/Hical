#include "Multipart.h"
#include <algorithm>
#include <cctype>

namespace hical
{

	// ============ 内部工具函数 ============

	namespace
	{

		// 原地转小写
		inline std::string toLower(std::string s)
		{
			std::transform(s.begin(),
						   s.end(),
						   s.begin(),
						   [](unsigned char c)
						   {
							   return std::tolower(c);
						   });
			return s;
		}

		// 去除首尾空白
		inline std::string_view trim(std::string_view sv)
		{
			while (!sv.empty() && std::isspace(static_cast<unsigned char>(sv.front())))
			{
				sv.remove_prefix(1);
			}
			while (!sv.empty() && std::isspace(static_cast<unsigned char>(sv.back())))
			{
				sv.remove_suffix(1);
			}
			return sv;
		}

		// 去除首尾引号（"value" -> value）
		inline std::string_view unquote(std::string_view sv)
		{
			if (sv.size() >= 2 && sv.front() == '"' && sv.back() == '"')
			{
				return sv.substr(1, sv.size() - 2);
			}
			return sv;
		}

	} // namespace

	// ============ MultipartParser 实现 ============

	std::string MultipartParser::extractBoundary(const std::string& contentType)
	{
		// Content-Type: multipart/form-data; boundary=----WebKitFormBoundary
		auto pos = contentType.find("boundary=");
		if (pos == std::string::npos)
		{
			return "";
		}
		pos += 9; // 跳过 "boundary="
		std::string_view rest(contentType.c_str() + pos, contentType.size() - pos);

		// 跳过可能的引号
		rest = trim(rest);
		rest = unquote(rest);

		// boundary 到下一个 ';' 或末尾
		auto semi = rest.find(';');
		if (semi != std::string_view::npos)
		{
			rest = rest.substr(0, semi);
		}

		auto boundary = std::string(trim(rest));

		// RFC 2046: boundary 最长 70 字符
		if (boundary.size() > 70)
		{
			return "";
		}

		return boundary;
	}

	void MultipartParser::parseDispositionParams(std::string_view disposition, std::string& name, std::string& filename)
	{
		// 跳过 "form-data" 部分
		auto semi = disposition.find(';');
		if (semi == std::string_view::npos)
		{
			return;
		}
		disposition = disposition.substr(semi + 1);

		// 逐个解析参数
		while (!disposition.empty())
		{
			semi = disposition.find(';');
			std::string_view param = (semi != std::string_view::npos) ? disposition.substr(0, semi) : disposition;
			disposition = (semi != std::string_view::npos) ? disposition.substr(semi + 1) : std::string_view {};

			param = trim(param);
			auto eq = param.find('=');
			if (eq == std::string_view::npos)
			{
				continue;
			}
			auto key = trim(param.substr(0, eq));
			auto val = trim(param.substr(eq + 1));
			val = unquote(val);

			if (key == "name")
			{
				name = std::string(val);
			}
			else if (key == "filename")
			{
				filename = std::string(val);
			}
		}
	}

	void MultipartParser::parsePartHeaders(std::string_view headerBlock, MultipartPart& part)
	{
		// 按行解析头部
		while (!headerBlock.empty())
		{
			auto crlf = headerBlock.find("\r\n");
			std::string_view line = (crlf != std::string_view::npos) ? headerBlock.substr(0, crlf) : headerBlock;
			headerBlock = (crlf != std::string_view::npos) ? headerBlock.substr(crlf + 2) : std::string_view {};

			if (line.empty())
			{
				continue;
			}

			auto colon = line.find(':');
			if (colon == std::string_view::npos)
			{
				continue;
			}

			std::string key = toLower(std::string(trim(line.substr(0, colon))));
			std::string val = std::string(trim(line.substr(colon + 1)));
			part.headers[key] = val;

			if (key == "content-disposition")
			{
				parseDispositionParams(val, part.name, part.filename);
			}
			else if (key == "content-type")
			{
				part.contentType = val;
			}
		}
	}

	std::optional<std::vector<MultipartPart>> MultipartParser::parse(const HttpRequest& req)
	{
		// 检查 Content-Type
		auto ct = req.contentType();
		auto ctLower = toLower(ct);
		if (ctLower.find("multipart/form-data") == std::string::npos)
		{
			return std::nullopt;
		}

		// 提取 boundary
		auto boundary = extractBoundary(ct);
		if (boundary.empty())
		{
			return std::nullopt;
		}

		const std::string& body = req.body();
		// delimiter = "--" + boundary
		std::string delimiter = "--" + boundary;

		std::vector<MultipartPart> parts;
		std::string_view data(body);

		// 查找第一个 delimiter
		auto pos = data.find(delimiter);
		if (pos == std::string_view::npos)
		{
			return std::nullopt;
		}
		pos += delimiter.size();

		// 跳过 delimiter 后的 CRLF
		if (data.substr(pos, 2) == "\r\n")
		{
			pos += 2;
		}
		else if (data.substr(pos, 2) == "--")
		{
			// 空 multipart
			return parts;
		}
		else
		{
			return std::nullopt;
		}

		while (pos < data.size())
		{
			// 查找下一个 delimiter（可能是普通 delimiter 或 end delimiter）
			auto nextDelim = data.find(delimiter, pos);
			if (nextDelim == std::string_view::npos)
			{
				break;
			}

			// Part 内容：从 pos 到 nextDelim（去掉末尾 CRLF）
			std::string_view partData = data.substr(pos, nextDelim - pos);
			if (partData.size() >= 2 && partData.substr(partData.size() - 2) == "\r\n")
			{
				partData.remove_suffix(2);
			}

			// 找到空行分割头部和 body（"\r\n\r\n"）
			auto headerEnd = partData.find("\r\n\r\n");
			if (headerEnd == std::string_view::npos)
			{
				return std::nullopt;
			}

			MultipartPart part;
			parsePartHeaders(partData.substr(0, headerEnd), part);
			part.data = std::string(partData.substr(headerEnd + 4));
			parts.push_back(std::move(part));

			// Part 数量上限：防止在 maxBodySize 内构造大量小 Part 消耗 CPU/内存
			static constexpr std::size_t hMaxMultipartParts = 256;
			if (parts.size() >= hMaxMultipartParts)
			{
				return std::nullopt;
			}

			// 移动到下一个 delimiter 之后
			pos = nextDelim + delimiter.size();

			// 检查是否结束
			if (data.substr(pos, 2) == "--")
			{
				break;
			}
			// 跳过 CRLF
			if (data.substr(pos, 2) == "\r\n")
			{
				pos += 2;
			}
			else
			{
				break;
			}
		}

		return parts;
	}

	std::optional<MultipartPart> MultipartParser::getFile(const HttpRequest& req, const std::string& fieldName)
	{
		auto parts = parse(req);
		if (!parts)
		{
			return std::nullopt;
		}
		for (auto& part : *parts)
		{
			if (part.name == fieldName && part.isFile())
			{
				return part;
			}
		}
		return std::nullopt;
	}

	std::optional<std::string> MultipartParser::getField(const HttpRequest& req, const std::string& fieldName)
	{
		auto parts = parse(req);
		if (!parts)
		{
			return std::nullopt;
		}
		for (auto& part : *parts)
		{
			if (part.name == fieldName && !part.isFile())
			{
				return part.data;
			}
		}
		return std::nullopt;
	}

} // namespace hical
