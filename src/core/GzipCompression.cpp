/**
 * @file GzipCompression.cpp
 * @brief 响应压缩中间件（gzip）实现
 */

#include "GzipCompression.h"
#include "ChunkedBody.h"
#include <zlib.h>
#include <array>
#include <string>

#include <boost/asio/post.hpp>
#include <boost/asio/thread_pool.hpp>
#include <boost/asio/use_awaitable.hpp>

namespace hical
{

	namespace
	{

		/**
		 * @brief 判断 MIME 类型是否适合 gzip 压缩
		 * 只对文本和类文本格式压缩（text 类, json, js, xml, svg 等），
		 * 跳过 image/webp, font/woff2, video/mp4 等已压缩二进制格式，
		 * 避免无意义的 deflate 计算。
		 * @param contentType Content-Type 头部值
		 * @return true 表示适合压缩
		 */
		bool isCompressible(std::string_view contentType)
		{
			if (contentType.empty())
			{
				return false;
			}

			// 去掉 charset 等参数，只取 media type 部分
			auto semi = contentType.find(';');
			auto mediaType = (semi != std::string_view::npos) ? contentType.substr(0, semi) : contentType;

			if (mediaType.starts_with("text/"))
			{
				return true;
			}
			if (mediaType.starts_with("application/json"))
			{
				return true;
			}
			if (mediaType.starts_with("application/javascript"))
			{
				return true;
			}
			if (mediaType.starts_with("application/xml"))
			{
				return true;
			}
			if (mediaType.starts_with("image/svg+xml"))
			{
				return true;
			}
			if (mediaType.starts_with("application/xhtml+xml"))
			{
				return true;
			}

			return false;
		}

		/// 检查 Accept-Encoding 是否包含 gzip
		bool acceptsGzip(const HttpRequest& req)
		{
			auto ae = req.header("Accept-Encoding");
			if (ae.empty())
			{
				return false;
			}
			std::string_view sv(ae);
			while (!sv.empty())
			{
				auto comma = sv.find(',');
				auto token = (comma != std::string_view::npos) ? sv.substr(0, comma) : sv;
				sv = (comma != std::string_view::npos) ? sv.substr(comma + 1) : std::string_view {};

				while (!token.empty() && token.front() == ' ')
				{
					token.remove_prefix(1);
				}
				while (!token.empty() && token.back() == ' ')
				{
					token.remove_suffix(1);
				}

				auto semi = token.find(';');
				if (semi != std::string_view::npos)
				{
					token = token.substr(0, semi);
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
		 * @brief 获取 thread_local 缓存的 z_stream
		 * 首次调用时用指定 level 初始化，后续通过 deflateReset 复用。
		 * 避免高频压缩场景下频繁 deflateInit2/deflateEnd。
		 * @param level 压缩级别（1-9）
		 * @return 初始化好的 z_stream 引用
		 */
		z_stream& cachedZStream(int level)
		{
			struct CachedStream
			{
				z_stream strm = {};
				int cachedLevel = -1;
				bool initialized = false;

				~CachedStream()
				{
					if (initialized)
					{
						deflateEnd(&strm);
					}
				}
			};

			static thread_local CachedStream cs;

			if (cs.initialized && cs.cachedLevel == level)
			{
				deflateReset(&cs.strm);
				return cs.strm;
			}

			if (cs.initialized)
			{
				deflateEnd(&cs.strm);
				cs.initialized = false;
			}

			cs.strm = {};
			cs.strm.zalloc = Z_NULL;
			cs.strm.zfree = Z_NULL;
			cs.strm.opaque = Z_NULL;

			auto ret = deflateInit2(&cs.strm, level, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY);
			if (ret != Z_OK)
			{
				throw std::runtime_error("gzip deflateInit2 failed: " + std::to_string(ret));
			}

			cs.cachedLevel = level;
			cs.initialized = true;
			return cs.strm;
		}

		/**
		 * @brief 用 zlib deflate 压缩数据（gzip 格式）
		 */
		std::string gzipCompress(std::string_view input, int level)
		{
			if (input.empty())
			{
				return {};
			}

			auto& strm = cachedZStream(level);
			strm.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(input.data()));
			strm.avail_in = static_cast<uInt>(input.size());

			std::string output;
			output.reserve(input.size() + 1024);
			std::array<char, 16384> outBuf {};

			int ret;
			do
			{
				strm.next_out = reinterpret_cast<Bytef*>(outBuf.data());
				strm.avail_out = sizeof(outBuf);

				ret = deflate(&strm, Z_FINISH);
				if (ret == Z_STREAM_ERROR)
				{
					deflateReset(&strm);
					throw std::runtime_error("gzip deflate failed: Z_STREAM_ERROR");
				}

				output.append(outBuf.data(), sizeof(outBuf) - strm.avail_out);
			}
			while (ret != Z_STREAM_END);

			deflateReset(&strm);
			return output;
		}

		/**
		 * @brief 流式压缩，每次输出编码为 chunked frame
		 */
		std::string gzipCompressChunked(std::string_view input, int level)
		{
			if (input.empty())
			{
				return "0\r\n\r\n";
			}

			auto& strm = cachedZStream(level);
			strm.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(input.data()));
			strm.avail_in = static_cast<uInt>(input.size());

			std::string result;
			result.reserve(input.size() / 2);
			std::array<char, 16384> outBuf {};

			int ret;

			do
			{
				strm.next_out = reinterpret_cast<Bytef*>(outBuf.data());
				strm.avail_out = sizeof(outBuf);

				ret = deflate(&strm, Z_SYNC_FLUSH);
				if (ret == Z_STREAM_ERROR)
				{
					deflateReset(&strm);
					throw std::runtime_error("gzip deflate streaming failed: Z_STREAM_ERROR");
				}

				auto have = sizeof(outBuf) - strm.avail_out;
				if (have > 0)
				{
					result += serializeChunkFrame(std::string_view(outBuf.data(), have));
				}
			}
			while (strm.avail_in > 0);

			// flush 残留
			for (;;)
			{
				strm.next_out = reinterpret_cast<Bytef*>(outBuf.data());
				strm.avail_out = sizeof(outBuf);

				ret = deflate(&strm, Z_SYNC_FLUSH);
				if (ret == Z_STREAM_ERROR)
				{
					deflateReset(&strm);
					throw std::runtime_error("gzip deflate flush failed: Z_STREAM_ERROR");
				}

				auto have = sizeof(outBuf) - strm.avail_out;
				if (have == 0)
				{
					break;
				}
				result += serializeChunkFrame(std::string_view(outBuf.data(), have));
			}

			// finish
			for (;;)
			{
				strm.next_out = reinterpret_cast<Bytef*>(outBuf.data());
				strm.avail_out = sizeof(outBuf);

				ret = deflate(&strm, Z_FINISH);
				if (ret == Z_STREAM_ERROR)
				{
					deflateReset(&strm);
					throw std::runtime_error("gzip deflate finish failed: Z_STREAM_ERROR");
				}

				auto have = sizeof(outBuf) - strm.avail_out;
				if (have > 0)
				{
					result += serializeChunkFrame(std::string_view(outBuf.data(), have));
				}

				if (ret == Z_STREAM_END)
				{
					break;
				}
			}

			deflateReset(&strm);
			result += "0\r\n\r\n";
			return result;
		}

	} // namespace

	SyncAfterHandler makeGzipCompressionMiddleware(GzipCompressionOptions opts)
	{
		return [opts](HttpRequest& req, HttpResponse& res) -> void
		{
			if (!acceptsGzip(req))
			{
				return;
			}

			auto& native = res.native();

			if (!native.body.empty() && !native.hasChunkedBody() && !native.hasFileBody())
			{
				if (!isCompressible(native.headers.find("Content-Type")))
				{
					return;
				}

				auto bodySize = native.body.size();
				if (bodySize < opts.minSize)
				{
					return;
				}

				if (bodySize >= opts.streamingThreshold)
				{
					native.invalidatePayload();
					auto compressedWire = gzipCompressChunked(native.body, opts.compressionLevel);
					native.body = std::move(compressedWire);
					native.headers.set("Content-Encoding", "gzip");
					native.headers.set("Transfer-Encoding", "chunked");
					native.headers.erase("Content-Length");
				}
				else
				{
					native.invalidatePayload();
					auto compressed = gzipCompress(native.body, opts.compressionLevel);
					native.body = std::move(compressed);
					native.headers.set("Content-Encoding", "gzip");
					native.preparePayload();
				}
			}
		};
	}

	MiddlewareHandler makeGzipCompressionMiddlewareAsync(GzipCompressionOptions opts)
	{
		auto pool =
			std::make_shared<boost::asio::thread_pool>(opts.asyncThreads > 0 ? static_cast<int>(opts.asyncThreads) : 1);

		auto guard = std::make_shared<std::shared_ptr<boost::asio::thread_pool>>(pool);

		return [opts, pool, guard](HttpRequest& req, MiddlewareNext next) -> Awaitable<HttpResponse>
		{
			(void)guard;

			auto res = co_await next(req);

			if (!acceptsGzip(req))
			{
				co_return res;
			}

			auto& native = res.native();
			if (native.body.empty() || native.hasChunkedBody() || native.hasFileBody())
			{
				co_return res;
			}

			if (!isCompressible(native.headers.find("Content-Type")))
			{
				co_return res;
			}

			auto bodySize = native.body.size();
			if (bodySize < opts.minSize)
			{
				co_return res;
			}

			if (bodySize < opts.streamingThreshold)
			{
				native.invalidatePayload();
				auto compressed = gzipCompress(native.body, opts.compressionLevel);
				native.body = std::move(compressed);
				native.headers.set("Content-Encoding", "gzip");
				native.preparePayload();
				co_return res;
			}

			auto body = std::make_shared<std::string>(std::move(native.body));
			auto compressedWire = std::make_shared<std::string>();
			auto ioExecutor = co_await boost::asio::this_coro::executor;

			co_await boost::asio::post(*pool, boost::asio::use_awaitable);

			try
			{
				*compressedWire = gzipCompressChunked(*body, opts.compressionLevel);
			}
			catch (const std::exception&)
			{
				HttpResponse errorRes;
				errorRes.setStatus(HttpStatusCode::hInternalServerError);
				errorRes.setBody("gzip compression failed", "text/plain");
				co_return errorRes;
			}

			co_await boost::asio::post(boost::asio::bind_executor(ioExecutor, boost::asio::use_awaitable));

			native.invalidatePayload();
			native.body = std::move(*compressedWire);
			native.headers.set("Content-Encoding", "gzip");
			native.headers.set("Transfer-Encoding", "chunked");
			native.headers.erase("Content-Length");

			co_return res;
		};
	}

} // namespace hical
