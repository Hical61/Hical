/**
 * @file ChunkedBody.cpp
 * @brief Chunked Transfer-Encoding 响应体实现
 */

#include "ChunkedBody.h"
#include <charconv>

namespace hical
{

	void ChunkedBody::write(std::string_view data)
	{
		if (finished_ || data.empty())
		{
			return;
		}
		chunks_.emplace_back(data);
	}

	void ChunkedBody::end(const std::vector<ChunkExtension>& trailers)
	{
		if (finished_)
		{
			return;
		}
		finished_ = true;
		trailers_ = trailers;
	}

	// ============ 自由函数实现 ============

	std::string serializeChunkFrame(std::string_view data)
	{
		std::string result;

		if (data.empty())
		{
			result = "0\r\n\r\n";
			return result;
		}

		// chunk-size 用十六进制
		char sizeBuf[20];
		auto [ptr, ec] = std::to_chars(sizeBuf, sizeBuf + sizeof(sizeBuf), data.size(), 16);
		auto sizeLen = static_cast<size_t>(ptr - sizeBuf);

		// 预分配：size + \r\n + data + \r\n
		result.reserve(sizeLen + 2 + data.size() + 2);
		result.append(sizeBuf, sizeLen);
		result.append("\r\n");
		result.append(data);
		result.append("\r\n");
		return result;
	}

	std::string serializeTrailerFrame(const std::vector<ChunkExtension>& trailers)
	{
		std::string result = "0\r\n";

		for (const auto& ext : trailers)
		{
			result += ext.name;
			result += ": ";
			result += ext.value;
			result += "\r\n";
		}

		result += "\r\n";
		return result;
	}

	std::string serializeChunkedBody(const ChunkedBody& body)
	{
		std::string result;
		serializeChunkedBodyTo(result, body);
		return result;
	}

	void serializeChunkedBodyTo(std::string& out, const ChunkedBody& body)
	{
		char sizeBuf[20];
		for (const auto& chunk : body.chunks())
		{
			auto [ptr, ec] = std::to_chars(sizeBuf, sizeBuf + sizeof(sizeBuf), chunk.size(), 16);
			auto sizeLen = static_cast<size_t>(ptr - sizeBuf);
			out.append(sizeBuf, sizeLen);
			out.append("\r\n");
			out.append(chunk);
			out.append("\r\n");
		}
		// trailer（含终止帧 0\r\n）
		out += "0\r\n";
		for (const auto& ext : body.trailers())
		{
			out += ext.name;
			out += ": ";
			out += ext.value;
			out += "\r\n";
		}
		out += "\r\n";
	}

	size_t chunkedBodyWireSize(const ChunkedBody& body) noexcept
	{
		size_t total = 0;
		char sizeBuf[20];
		for (const auto& chunk : body.chunks())
		{
			auto [ptr, ec] = std::to_chars(sizeBuf, sizeBuf + sizeof(sizeBuf), chunk.size(), 16);
			total += static_cast<size_t>(ptr - sizeBuf) + 2; // chunk-size + \r\n
			total += chunk.size() + 2;                       // data + \r\n
		}
		total += 3; // 终止帧 "0\r\n"
		for (const auto& ext : body.trailers())
		{
			total += ext.name.size() + 2 + ext.value.size() + 2; // name: value\r\n
		}
		total += 2; // 尾部 \r\n
		return total;
	}

} // namespace hical
