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

		for (const auto& chunk : body.chunks())
		{
			result += serializeChunkFrame(chunk);
		}

		result += serializeTrailerFrame(body.trailers());
		return result;
	}

} // namespace hical
