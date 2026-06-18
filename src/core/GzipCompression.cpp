/**
 * @file GzipCompression.cpp
 * @brief 响应压缩中间件（gzip）实现
 */

#include "GzipCompression.h"
#include "ChunkedBody.h"
#include <zlib.h>
#include <array>
#include <string>

namespace hical
{

	namespace
	{

		/// 检查 Accept-Encoding 是否包含 gzip
		bool acceptsGzip(const HttpRequest& req)
		{
			auto ae = req.header("Accept-Encoding");
			if (ae.empty())
			{
				return false;
			}
			// 逗号分隔扫描
			std::string_view sv(ae);
			while (!sv.empty())
			{
				auto comma = sv.find(',');
				auto token = (comma != std::string_view::npos) ? sv.substr(0, comma) : sv;
				sv = (comma != std::string_view::npos) ? sv.substr(comma + 1) : std::string_view {};

				// 去掉首尾空格
				while (!token.empty() && token.front() == ' ')
				{
					token.remove_prefix(1);
				}
				while (!token.empty() && token.back() == ' ')
				{
					token.remove_suffix(1);
				}

				// Content negotiation 格式：gzip;q=1.0 — 去掉分号后的参数
				auto semi = token.find(';');
				if (semi != std::string_view::npos)
				{
					token = token.substr(0, semi);
					// 去掉尾部空格
					while (!token.empty() && token.back() == ' ')
					{
						token.remove_suffix(1);
					}
				}

				if (token == "gzip")
				{
					return true;
				}
			}
			return false;
		}

		/**
		 * @brief 用 zlib deflate 压缩数据（gzip 格式）
		 * @param input 原始数据
		 * @param level 压缩级别（1-9）
		 * @return 压缩后的 gzip 数据
		 * @throws std::runtime_error deflateInit/deflate 失败时抛出
		 */
		std::string gzipCompress(std::string_view input, int level)
		{
			if (input.empty())
			{
				return {};
			}

			z_stream strm = {};
			strm.zalloc = Z_NULL;
			strm.zfree = Z_NULL;
			strm.opaque = Z_NULL;

			// 15 + 16 = MAX_WBITS + GZIP 格式
			auto ret = deflateInit2(&strm, level, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY);
			if (ret != Z_OK)
			{
				throw std::runtime_error("gzip deflateInit2 failed: " + std::to_string(ret));
			}

			// z_stream::next_in 在旧版 zlib 中是非 const 指针，需要 const_cast
			strm.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(input.data()));
			strm.avail_in = static_cast<uInt>(input.size());

			// 初始输出缓冲区：input size + 1KB 余量（gzip 最小头尾）
			std::string output;
			output.reserve(input.size() + 1024);

			std::array<char, 16384> outBuf {};

			do
			{
				strm.next_out = reinterpret_cast<Bytef*>(outBuf.data());
				strm.avail_out = sizeof(outBuf);

				ret = deflate(&strm, Z_FINISH);
				if (ret == Z_STREAM_ERROR)
				{
					deflateEnd(&strm);
					throw std::runtime_error("gzip deflate failed: Z_STREAM_ERROR");
				}

				auto have = sizeof(outBuf) - strm.avail_out;
				output.append(outBuf.data(), have);
			}
			while (ret != Z_STREAM_END);

			deflateEnd(&strm);

			if (ret != Z_STREAM_END)
			{
				throw std::runtime_error("gzip deflate unexpected end: " + std::to_string(ret));
			}

			return output;
		}

		/**
		 * @brief 用 zlib deflate 进行流式压缩，每次压缩一个 chunk 并编码为 chunked frame
		 * @param input 原始数据
		 * @param level 压缩级别
		 * @return 完整的 chunked + gzip 编码 wire 字节流
		 */
		std::string gzipCompressChunked(std::string_view input, int level)
		{
			if (input.empty())
			{
				// 空 body：只发终止帧
				return "0\r\n\r\n";
			}

			z_stream strm = {};
			strm.zalloc = Z_NULL;
			strm.zfree = Z_NULL;
			strm.opaque = Z_NULL;

			auto ret = deflateInit2(&strm, level, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY);
			if (ret != Z_OK)
			{
				throw std::runtime_error("gzip deflateInit2 failed: " + std::to_string(ret));
			}

			strm.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(input.data()));
			strm.avail_in = static_cast<uInt>(input.size());

			std::string result;
			result.reserve(input.size() / 2); // 压缩后预期减半

			std::array<char, 16384> outBuf {};

			do
			{
				strm.next_out = reinterpret_cast<Bytef*>(outBuf.data());
				strm.avail_out = sizeof(outBuf);

				ret = deflate(&strm, Z_SYNC_FLUSH);
				if (ret == Z_STREAM_ERROR)
				{
					deflateEnd(&strm);
					throw std::runtime_error("gzip deflate streaming failed: Z_STREAM_ERROR");
				}

				auto have = sizeof(outBuf) - strm.avail_out;
				if (have > 0)
				{
					// 编码为 chunked frame
					std::string_view compressed(outBuf.data(), have);
					result += serializeChunkFrame(compressed);
				}
			}
			// 循环直到所有输入都被消费（Z_SYNC_FLUSH 下大部分输入一次输出完，
			// 但大 body 可能在多次循环中逐步输出）
			while (strm.avail_in > 0);

			// 清理残留在输出缓冲区中的数据
			for (;;)
			{
				strm.next_out = reinterpret_cast<Bytef*>(outBuf.data());
				strm.avail_out = sizeof(outBuf);

				ret = deflate(&strm, Z_SYNC_FLUSH);
				if (ret == Z_STREAM_ERROR)
				{
					deflateEnd(&strm);
					throw std::runtime_error("gzip deflate final flush failed: Z_STREAM_ERROR");
				}

				auto have = sizeof(outBuf) - strm.avail_out;
				if (have == 0)
				{
					break;
				}
				std::string_view compressed(outBuf.data(), have);
				result += serializeChunkFrame(compressed);
			}

			// 结束压缩流
			for (;;)
			{
				strm.next_out = reinterpret_cast<Bytef*>(outBuf.data());
				strm.avail_out = sizeof(outBuf);

				ret = deflate(&strm, Z_FINISH);
				if (ret == Z_STREAM_ERROR)
				{
					deflateEnd(&strm);
					throw std::runtime_error("gzip deflate finish failed: Z_STREAM_ERROR");
				}

				auto have = sizeof(outBuf) - strm.avail_out;
				if (have > 0)
				{
					std::string_view compressed(outBuf.data(), have);
					result += serializeChunkFrame(compressed);
				}

				if (ret == Z_STREAM_END)
				{
					break;
				}
			}

			deflateEnd(&strm);

			// 终止 chunked 帧
			result += "0\r\n\r\n";
			return result;
		}

	} // namespace

	SyncAfterHandler makeGzipCompressionMiddleware(GzipCompressionOptions opts)
	{
		return [opts](HttpRequest& req, HttpResponse& res) -> void
		{
			// 只压缩 200 OK 且有 body 的响应
			if (!acceptsGzip(req))
			{
				return;
			}

			auto& native = res.native();

			// 跳过：已压缩的、chunked body 路径（内容由 SSE/流式生成）、文件体路径
			if (!native.body.empty() && !native.hasChunkedBody() && !native.hasFileBody())
			{
				auto bodySize = native.body.size();
				if (bodySize < opts.minSize)
				{
					return; // 小 body 跳过，小于阈值没意义
				}

				if (bodySize >= opts.streamingThreshold)
				{
					// 大 body：走 chunked + gzip 流式压缩
					auto compressedWire = gzipCompressChunked(native.body, opts.compressionLevel);
					native.body = std::move(compressedWire);
					native.headers.set("Content-Encoding", "gzip");
					native.headers.set("Transfer-Encoding", "chunked");
					native.headers.erase("Content-Length");
				}
				else
				{
					// 小 body：整体压缩
					auto compressed = gzipCompress(native.body, opts.compressionLevel);
					native.body = std::move(compressed);
					native.headers.set("Content-Encoding", "gzip");
					native.preparePayload();
				}
			}
		};
	}

} // namespace hical
