/**
 * @file GzipCompression.cpp
 * @brief 响应压缩中间件（gzip）实现
 */

#include "GzipCompression.h"
#include "ChunkedBody.h"
#include <boost/asio/bind_executor.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/thread_pool.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <zlib.h>
#include <array>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

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

		std::string_view trimOws(std::string_view text)
		{
			while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
			{
				text.remove_prefix(1);
			}
			while (!text.empty() && (text.back() == ' ' || text.back() == '\t'))
			{
				text.remove_suffix(1);
			}
			return text;
		}

		std::string_view takePart(std::string_view& text, char separator)
		{
			const auto position = text.find(separator);
			const auto part = trimOws(text.substr(0, position));
			text = position == std::string_view::npos ? std::string_view {} : text.substr(position + 1);
			return part;
		}

		bool positiveQuality(std::string_view quality)
		{
			constexpr size_t kMaxQualityLength = 5;
			if (quality.empty() || (quality.front() != '0' && quality.front() != '1'))
			{
				return false;
			}
			if (quality.size() == 1)
			{
				return quality.front() == '1';
			}
			if (quality.size() > kMaxQualityLength || quality[1] != '.')
			{
				return false;
			}
			bool positive = quality.front() == '1';
			for (char digit : quality.substr(2))
			{
				if (digit < '0' || digit > '9' || (quality.front() == '1' && digit != '0'))
				{
					return false;
				}
				positive = positive || digit != '0';
			}
			return positive;
		}

		/// 只协商 gzip；q=0 或非法 qvalue 都不压缩。
		bool acceptsGzip(const HttpRequest& req)
		{
			auto encodings = req.header("Accept-Encoding");
			while (!encodings.empty())
			{
				auto parameters = takePart(encodings, ',');
				if (!HeaderMap::iequals(takePart(parameters, ';'), "gzip"))
				{
					continue;
				}
				bool acceptable = true;
				while (!parameters.empty())
				{
					auto parameter = takePart(parameters, ';');
					auto equals = parameter.find('=');
					if (HeaderMap::iequals(trimOws(parameter.substr(0, equals)), "q"))
					{
						acceptable =
							equals != std::string_view::npos && positiveQuality(trimOws(parameter.substr(equals + 1)));
						if (!acceptable)
						{
							break;
						}
					}
				}
				if (acceptable)
				{
					return true;
				}
			}
			return false;
		}

		void varyAcceptEncoding(HeaderMap& headers)
		{
			for (const auto& [name, value] : headers)
			{
				if (!HeaderMap::iequals(name, "vary"))
				{
					continue;
				}
				std::string_view tokens = value;
				while (!tokens.empty())
				{
					const auto token = takePart(tokens, ',');
					if (token == "*" || HeaderMap::iequals(token, "accept-encoding"))
					{
						return;
					}
				}
			}
			auto vary = std::string(headers.find("Vary"));
			if (!vary.empty())
			{
				vary += ", ";
			}
			vary += "Accept-Encoding";
			headers.set("Vary", vary);
		}

		bool compressionEligible(const NativeResponse& response, const GzipCompressionOptions& options)
		{
			const auto status = static_cast<unsigned>(response.status);
			return !response.body.empty() && !response.hasChunkedBody() && !response.hasFileBody()
				   && !response.headers.contains("Content-Encoding") && response.body.size() >= options.minSize
				   && isCompressible(response.headers.find("Content-Type")) && status >= 200 && status != 204
				   && status != 304;
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
#ifndef __MINGW32__
					// MinGW 下的 thread_local 析构是在 DLL TLS 回调里跑的，那个时机
					// 不确定 CRT 堆还在不在，deflateEnd 内部要 free()，下去就是 0xc0000374。
					// 进程退出 OS 会帮我们收，一个线程就一份 z_stream，无所谓了。
					if (initialized)
					{
						deflateEnd(&strm);
					}
#endif
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
		 * @brief 将 gzip 输出包装为 HTTP/1 分块正文，完整缓冲后返回
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

		std::string compressBytes(std::string_view body, int level, bool chunked)
		{
			return chunked ? gzipCompressChunked(body, level) : gzipCompress(body, level);
		}

		void applyCompressed(NativeResponse& response, std::string bytes, bool chunked)
		{
			response.invalidatePayload();
			response.body = std::move(bytes);
			response.headers.set("Content-Encoding", "gzip");
			response.headers.erase("Transfer-Encoding");
			response.headers.erase("Content-Length");
			if (chunked)
			{
				response.headers.set("Transfer-Encoding", "chunked");
			}
			response.preparePayload();
		}

		Awaitable<std::string> compressOnWorker(std::string body, int level, bool chunked)
		{
			// 协程帧持有输入；压缩过程不挂起，thread_local z_stream 不跨 co_await 使用。
			co_return compressBytes(body, level, chunked);
		}

		Awaitable<HttpResponse> compressAfter(HttpRequest& request,
											  MiddlewareNext next,
											  GzipCompressionOptions options,
											  std::shared_ptr<boost::asio::thread_pool> pool)
		{
			auto response = co_await next(request);
			auto& native = response.native();
			if (!compressionEligible(native, options))
			{
				co_return response;
			}
			varyAcceptEncoding(native.headers);
			if (!acceptsGzip(request))
			{
				co_return response;
			}
			const auto bodySize = native.body.size();
			const bool chunked = request.protocol() != HttpProtocol::hHttp2 && bodySize >= options.streamingThreshold;
			if (!pool || bodySize < options.streamingThreshold)
			{
				applyCompressed(native, compressBytes(native.body, options.compressionLevel, chunked), chunked);
				co_return response;
			}
			auto owner = co_await boost::asio::this_coro::executor;
			try
			{
				auto compressed = co_await boost::asio::co_spawn(
					*pool,
					compressOnWorker(std::move(native.body), options.compressionLevel, chunked),
					boost::asio::bind_executor(owner, boost::asio::use_awaitable));
				applyCompressed(native, std::move(compressed), chunked);
			}
			catch (const std::exception&)
			{
				HttpResponse errorRes;
				errorRes.setStatus(HttpStatusCode::hInternalServerError);
				errorRes.setBody("gzip compression failed", "text/plain");
				co_return errorRes;
			}
			co_return response;
		}
	} // namespace

	SyncAfterHandler makeGzipCompressionMiddleware(GzipCompressionOptions opts)
	{
		return [opts](HttpRequest& request, HttpResponse& response)
		{
			auto& native = response.native();
			if (!compressionEligible(native, opts))
			{
				return;
			}
			varyAcceptEncoding(native.headers);
			if (!acceptsGzip(request))
			{
				return;
			}
			const bool chunked =
				request.protocol() != HttpProtocol::hHttp2 && native.body.size() >= opts.streamingThreshold;
			applyCompressed(native, compressBytes(native.body, opts.compressionLevel, chunked), chunked);
		};
	}

	MiddlewareHandler makeGzipCompressionMiddlewareAsync(GzipCompressionOptions opts)
	{
		std::shared_ptr<boost::asio::thread_pool> pool;
		if (opts.asyncThreads > 0)
		{
			pool = std::make_shared<boost::asio::thread_pool>(opts.asyncThreads);
		}
		// 返回 awaitable 前先复制配置和线程池引用，协程不再依赖这个闭包。
		return [opts, pool](HttpRequest& request, MiddlewareNext next) -> Awaitable<HttpResponse>
		{
			return compressAfter(request, std::move(next), opts, pool);
		};
	}

} // namespace hical
