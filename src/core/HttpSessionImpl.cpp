/**
 * @file HttpSessionImpl.cpp
 * @brief HTTP 会话处理（编译防火墙）
 * picohttpparser 和 WebSocket 帧处理代码集中在这里，
 * 改 HttpServer 的配置逻辑不会触发这些重模板代码的重编。
 */

#include "HttpServer.h"
#include "../asio/GenericConnection.h"
#include "FixedBuffer.h"
#include "HeaderMap.h"
#include "HttpTypes.h"
#include "InetAddress.h"
#include "MemoryPool.h"
#include "ReadBufferPool.h"
#include "RequestDispatch.h"
#include "SseSession.h"
#include "core/Version.h"
#include "WebSocket.h"
#include "WsHandshake.h"
#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstring>
#include <ctime>
#include <exception>
#include <fstream>
#include <optional>
#include <string_view>
#include <system_error>
#include <vector>

// TCP_CORK / TCP_NOPUSH（writeFileResponse 合并小包用）
#if defined(__linux__)
	#include <netinet/tcp.h>
#elif defined(__APPLE__)
	#include <netinet/tcp.h>
#endif

// picohttpparser（C 库）
extern "C"
{
#include "picohttpparser.h"
}

namespace hical
{

	using boost::asio::ip::tcp;

	namespace
	{

		/// RFC 9112 §6: requests must end their transfer-coding chain with chunked.
		/// Only plain chunked decoding is implemented; other preceding codings need a 501.
		HttpStatusCode validateTransferEncoding(std::string_view value)
		{
			bool unsupportedCoding = false;
			for (;;)
			{
				auto comma = value.find(',');
				auto token = value.substr(0, comma);
				auto first = token.find_first_not_of(" \t");
				if (first == std::string_view::npos)
				{
					return HttpStatusCode::hBadRequest;
				}
				token.remove_prefix(first);
				token = token.substr(0, token.find_last_not_of(" \t") + 1);
				auto codingEnd = token.find_first_of(" \t;");
				if (HeaderMap::iequals(token.substr(0, codingEnd), "chunked"))
				{
					// chunked must be last, unique, and have no parameters.
					if (codingEnd != std::string_view::npos || comma != std::string_view::npos)
					{
						return HttpStatusCode::hBadRequest;
					}
					return unsupportedCoding ? HttpStatusCode::hNotImplemented : HttpStatusCode::hOk;
				}
				if (comma == std::string_view::npos)
				{
					return HttpStatusCode::hBadRequest;
				}
				unsupportedCoding = true;
				value.remove_prefix(comma + 1);
			}
		}

		/// 将方法字符串映射到 HttpMethod 枚举
		HttpMethod methodFromStringView(std::string_view sv)
		{
			if (sv.size() < 3)
			{
				return HttpMethod::hUnknown;
			}
			switch (sv[0])
			{
				case 'G':
					if (sv == "GET")
					{
						return HttpMethod::hGet;
					}
					break;
				case 'P':
					if (sv == "POST")
					{
						return HttpMethod::hPost;
					}
					if (sv == "PUT")
					{
						return HttpMethod::hPut;
					}
					if (sv == "PATCH")
					{
						return HttpMethod::hPatch;
					}
					break;
				case 'D':
					if (sv == "DELETE")
					{
						return HttpMethod::hDelete;
					}
					break;
				case 'H':
					if (sv == "HEAD")
					{
						return HttpMethod::hHead;
					}
					break;
				case 'O':
					if (sv == "OPTIONS")
					{
						return HttpMethod::hOptions;
					}
					break;
			}
			return HttpMethod::hUnknown;
		}

		/// thread_local RFC 7231 Date 头缓存（每秒更新一次）
		/// 格式: "Thu, 13 May 2026 08:00:00 GMT"
		struct DateCache
		{
			time_t cachedSec {0};
			char buf[30] {}; // 29 chars + NUL
			size_t len {0};
		};

		// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)
		thread_local DateCache dateTlsCache; // NOLINT

		// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

		std::string_view cachedHttpDate() noexcept
		{
			auto& cache = dateTlsCache;
			auto now = std::time(nullptr);
			if (now != cache.cachedSec)
			{
				cache.cachedSec = now;
				struct tm gmt {};
#if defined(_WIN32)
				gmtime_s(&gmt, &now);
#else
				gmtime_r(&now, &gmt);
#endif
				cache.len = std::strftime(cache.buf, sizeof(cache.buf), "%a, %d %b %Y %H:%M:%S GMT", &gmt);
			}
			return {cache.buf, cache.len};
		}

		/// RAII TCP_CORK 守卫，让 writeFileResponse 的 head+首块合并成一个 TCP 段
		/// Linux 用 TCP_CORK，macOS 用 TCP_NOPUSH，Windows 下啥也不干（应用层已经 scatter-gather 了）
		template <typename SocketType>
		struct TcpCorkGuard
		{
			SocketType& sock_;
			bool corked_ {false};

			/// 取底层 fd：ssl::stream 原生 handle 包在 OpenSSL BIO 里，得下一层才能拿到真 socket
			auto fd()
			{
				if constexpr (hIsSslStream<SocketType>)
				{
					return sock_.next_layer().native_handle();
				}
				else
				{
					return sock_.native_handle();
				}
			}

			explicit TcpCorkGuard(SocketType& s) : sock_(s)
			{
#if defined(__linux__)
				int flag = 1;
				if (::setsockopt(fd(), IPPROTO_TCP, TCP_CORK, &flag, sizeof(flag)) == 0)
				{
					corked_ = true;
				}
#elif defined(__APPLE__)
				int flag = 1;
				if (::setsockopt(fd(), IPPROTO_TCP, TCP_NOPUSH, &flag, sizeof(flag)) == 0)
				{
					corked_ = true;
				}
#endif
			}

			~TcpCorkGuard()
			{
				if (corked_)
				{
#if defined(__linux__)
					int flag = 0;
					::setsockopt(fd(), IPPROTO_TCP, TCP_CORK, &flag, sizeof(flag));
#elif defined(__APPLE__)
					int flag = 0;
					::setsockopt(fd(), IPPROTO_TCP, TCP_NOPUSH, &flag, sizeof(flag));
#endif
				}
			}

			TcpCorkGuard(const TcpCorkGuard&) = delete;
			TcpCorkGuard& operator=(const TcpCorkGuard&) = delete;
		};

		/// 乐观同步写辅助：socket 在非阻塞模式下先试一把 write_some，
		/// 写完了直接返回 true（不挂协程、不进 reactor 完成队列）。
		/// 没写完时返回 false，并把已经写出去的字节数带回来（一个字节都没出去就是 0）——
		/// 调用方必须用 writeRemaining 从那个偏移往后接着发。别想着直接 async_write 整段：
		/// TCP 是有序字节流，重发的前缀不会被对端吞掉，只会被当成新数据再收一遍。
		template <typename SocketType>
		bool tryOptimisticWrite([[maybe_unused]] SocketType& socket,
								[[maybe_unused]] const boost::asio::const_buffer& buf,
								[[maybe_unused]] size_t& written)
		{
			if constexpr (hIsSslStream<SocketType>)
			{
				// SSL 流不能走乐观同步写：ssl::stream 内部 BIO 状态机要求走 async_write，
				// 直接底层 write_some 会绕过 OpenSSL 的记录层，数据都不加密。直接禁用。
				written = 0;
				return false;
			}
			else
			{
				boost::system::error_code ec;
				size_t n = socket.write_some(buf, ec);
				// 出错就当作一个字节都没写出去，别拿 write_some 的返回值去推偏移
				written = ec ? 0 : n;
				return !ec && n == buf.size();
			}
		}

		/// 两个 buffer 的版本：一次 writev 把头部和 body 一起推出去
		template <typename SocketType>
		bool tryOptimisticWrite([[maybe_unused]] SocketType& socket,
								[[maybe_unused]] const std::array<boost::asio::const_buffer, 2>& bufs,
								[[maybe_unused]] size_t& written)
		{
			if constexpr (hIsSslStream<SocketType>)
			{
				// 同上：ssl::stream 一律走 async_write，不碰底层 write_some
				written = 0;
				return false;
			}
			else
			{
				boost::system::error_code ec;
				size_t n = socket.write_some(bufs, ec);
				written = ec ? 0 : n;
				return !ec && n == boost::asio::buffer_size(bufs);
			}
		}

		/// 从 written 偏移处把剩下没写的字节发完（乐观写只写出去一部分时用）
		template <typename SocketType>
		Awaitable<void> writeRemaining(SocketType& socket, boost::asio::const_buffer buf, size_t written)
		{
			co_await boost::asio::async_write(
				socket,
				boost::asio::buffer(static_cast<const char*>(buf.data()) + written, buf.size() - written),
				boost::asio::use_awaitable);
		}

		/// 两个 buffer 的续写版本：written 可能落在头部里，也可能已经进了 body
		template <typename SocketType>
		Awaitable<void> writeRemaining(SocketType& socket,
									   std::array<boost::asio::const_buffer, 2> bufs,
									   size_t written)
		{
			const size_t headSize = bufs[0].size();
			if (written < headSize)
			{
				std::array<boost::asio::const_buffer, 2> rest = {
					boost::asio::buffer(static_cast<const char*>(bufs[0].data()) + written, headSize - written),
					bufs[1]};
				co_await boost::asio::async_write(socket, rest, boost::asio::use_awaitable);
			}
			else
			{
				const size_t bodyWritten = written - headSize;
				co_await boost::asio::async_write(
					socket,
					boost::asio::buffer(static_cast<const char*>(bufs[1].data()) + bodyWritten,
										bufs[1].size() - bodyWritten),
					boost::asio::use_awaitable);
			}
		}

		/// 序列化响应头：prefix 非空时把预拼好的 Server/Connection/Date 直接拼进去
		inline void serializeHeadWithPrefix(FixedBuffer<512>& buf,
											const NativeResponse& nativeRes,
											std::string_view prefix)
		{
			if (prefix.empty())
			{
				nativeRes.serializeHeadTo(buf);
			}
			else
			{
				nativeRes.serializeHeadTo(buf, prefix.data(), prefix.size());
			}
		}

		/// 记一笔「往 socket 发了一趟响应」。计数只给诊断和测试看，没接计数器就是空操作
		inline void recordResponseWrite(std::atomic<size_t>* writeCount)
		{
			if (writeCount != nullptr)
			{
				writeCount->fetch_add(1, std::memory_order_relaxed);
			}
		}

		/// 把攒着的响应一次性发出去。空批直接返回——单请求路径上这里只是一次分支，
		/// 不进内核，也不会让协程挂起。
		template <typename SocketType>
		Awaitable<void> flushResponseBatch(SocketType& socket, std::string& batch, std::atomic<size_t>* writeCount)
		{
			if (batch.empty())
			{
				co_return;
			}
			recordResponseWrite(writeCount);
			auto wire = boost::asio::buffer(batch.data(), batch.size());
			size_t sent = 0;
			if (!tryOptimisticWrite(socket, wire, sent))
			{
				co_await writeRemaining(socket, wire, sent);
			}
			// 只在写成功后清：中途抛异常的话连接已经在往死里走，上一批必然发不出去，
			// 脏数据跟着协程帧一起销毁，别留着下个请求接着用
			batch.clear();
		}

		/// 快速发送错误响应（栈缓冲区，零堆分配）
		template <typename SocketType>
		Awaitable<void> sendRawResponse(SocketType& socket,
										unsigned statusCode,
										std::string_view reason,
										std::string_view body,
										std::atomic<size_t>* writeCount)
		{
			recordResponseWrite(writeCount);
			FixedBuffer<512> buf;
			buf << "HTTP/1.1 ";

			char codeBuf[4];
			auto [ptr, ec] = std::to_chars(codeBuf, codeBuf + 4, statusCode);
			buf.append(codeBuf, static_cast<size_t>(ptr - codeBuf));
			buf << ' ';
			buf << reason;
			buf << "\r\nContent-Length: ";

			char lenBuf[16];
			auto [ptr2, ec2] = std::to_chars(lenBuf, lenBuf + 16, body.size());
			buf.append(lenBuf, static_cast<size_t>(ptr2 - lenBuf));
			buf << "\r\nConnection: close\r\n\r\n";
			buf << body;

			// 错误响应通常很小（几十到几百字节），乐观同步写几乎必中
			auto wire = boost::asio::buffer(buf.data(), buf.size());
			size_t sent = 0;
			if (tryOptimisticWrite(socket, wire, sent))
			{
				co_return;
			}

			co_await writeRemaining(socket, wire, sent);
		}

		/// 发送 HttpResponse 对象（内部辅助）
		/// @param skipBody 为 true 时仅发送头部（HEAD 方法响应）
		template <typename SocketType>
		Awaitable<void> writeResponse(SocketType& socket,
									  NativeResponse& nativeRes,
									  bool skipBody,
									  std::atomic<size_t>* writeCount)
		{
			recordResponseWrite(writeCount);
			nativeRes.preparePayload();

			// ChunkedBody 路径：scatter-gather 头部 + 全部 chunk 帧，一次 async_write
			if (nativeRes.hasChunkedBody())
			{
				FixedBuffer<512> headBuf;
				nativeRes.serializeHeadTo(headBuf);

				if (nativeRes.chunkedBody && !skipBody)
				{
					auto wire = std::make_shared<std::string>();
					auto totalSize = chunkedBodyWireSize(*nativeRes.chunkedBody);
					wire->reserve(totalSize);
					serializeChunkedBodyTo(*wire, *nativeRes.chunkedBody);

					if (!wire->empty())
					{
						// scatter-gather: head + body 一次 async_write
						std::array<boost::asio::const_buffer, 2> bufs = {
							boost::asio::buffer(headBuf.data(), headBuf.size()),
							boost::asio::buffer(*wire)};
						co_await boost::asio::async_write(socket, bufs, boost::asio::use_awaitable);
					}
					else
					{
						auto head = boost::asio::buffer(headBuf.data(), headBuf.size());
						size_t sent = 0;
						if (tryOptimisticWrite(socket, head, sent))
						{
							co_return;
						}

						co_await writeRemaining(socket, head, sent);
					}
				}
				else
				{
					auto head = boost::asio::buffer(headBuf.data(), headBuf.size());
					size_t sent = 0;
					if (tryOptimisticWrite(socket, head, sent))
					{
						co_return;
					}

					co_await writeRemaining(socket, head, sent);
				}
				co_return;
			}

			// 序列化头部到栈缓冲区（响应头通常 150-300 字节，512 足够）
			FixedBuffer<512> headBuf;
			nativeRes.serializeHeadTo(headBuf);

			if (skipBody || nativeRes.body.empty())
			{
				// HEAD 方法或空 body：仅发送头部
				auto head = boost::asio::buffer(headBuf.data(), headBuf.size());
				size_t sent = 0;
				if (tryOptimisticWrite(socket, head, sent))
				{
					co_return;
				}

				co_await writeRemaining(socket, head, sent);
			}
			else if (headBuf.size() + nativeRes.body.size() <= 512 && !headBuf.overflowed())
			{
				// 小响应（head + body <= 512）：合并到栈缓冲区，单次 write，零堆分配
				headBuf.append(nativeRes.body.data(), nativeRes.body.size());
				auto wire = boost::asio::buffer(headBuf.data(), headBuf.size());
				size_t sent = 0;
				if (tryOptimisticWrite(socket, wire, sent))
				{
					co_return;
				}

				co_await writeRemaining(socket, wire, sent);
			}
			else
			{
				// 大响应：scatter-gather I/O，head 在栈上 + body 零拷贝引用
				// 一次 writev 系统调用，消除 body 的额外 memcpy。
				// 乐观同步写对 JSON 这类 body 几百字节到几十 KB 的响应命中率很高，
				// 命不中也只是回到原来那次 async_write
				std::array<boost::asio::const_buffer, 2> bufs = {boost::asio::buffer(headBuf.data(), headBuf.size()),
																 boost::asio::buffer(nativeRes.body)};
				size_t sent = 0;
				if (tryOptimisticWrite(socket, bufs, sent))
				{
					co_return;
				}

				co_await writeRemaining(socket, bufs, sent);
			}
		}

		/// writeResponse 带前缀版本——通用头部（Server/Connection/Date）已预拼好，
		/// 直接 memcpy 进去，省掉 3 次 HeaderMap::insert + 序列化循环
		template <typename SocketType>
		Awaitable<void> writeResponse(SocketType& socket,
									  NativeResponse& nativeRes,
									  std::string_view prefix,
									  bool skipBody,
									  std::atomic<size_t>* writeCount)
		{
			recordResponseWrite(writeCount);
			nativeRes.preparePayload();

			// ChunkedBody 路径
			if (nativeRes.hasChunkedBody())
			{
				FixedBuffer<512> headBuf;
				serializeHeadWithPrefix(headBuf, nativeRes, prefix);

				if (nativeRes.chunkedBody && !skipBody)
				{
					auto wire = std::make_shared<std::string>();
					auto totalSize = chunkedBodyWireSize(*nativeRes.chunkedBody);
					wire->reserve(totalSize);
					serializeChunkedBodyTo(*wire, *nativeRes.chunkedBody);

					if (!wire->empty())
					{
						std::array<boost::asio::const_buffer, 2> bufs = {
							boost::asio::buffer(headBuf.data(), headBuf.size()),
							boost::asio::buffer(*wire)};
						co_await boost::asio::async_write(socket, bufs, boost::asio::use_awaitable);
					}
					else
					{
						auto head = boost::asio::buffer(headBuf.data(), headBuf.size());
						size_t sent = 0;
						if (tryOptimisticWrite(socket, head, sent))
						{
							co_return;
						}

						co_await writeRemaining(socket, head, sent);
					}
				}
				else
				{
					auto head = boost::asio::buffer(headBuf.data(), headBuf.size());
					size_t sent = 0;
					if (tryOptimisticWrite(socket, head, sent))
					{
						co_return;
					}

					co_await writeRemaining(socket, head, sent);
				}
				co_return;
			}

			FixedBuffer<512> headBuf;
			serializeHeadWithPrefix(headBuf, nativeRes, prefix);

			if (skipBody || nativeRes.body.empty())
			{
				auto head = boost::asio::buffer(headBuf.data(), headBuf.size());
				size_t sent = 0;
				if (tryOptimisticWrite(socket, head, sent))
				{
					co_return;
				}

				co_await writeRemaining(socket, head, sent);
			}
			else if (headBuf.size() + nativeRes.body.size() <= 512 && !headBuf.overflowed())
			{
				headBuf.append(nativeRes.body.data(), nativeRes.body.size());
				auto wire = boost::asio::buffer(headBuf.data(), headBuf.size());
				size_t sent = 0;
				if (tryOptimisticWrite(socket, wire, sent))
				{
					co_return;
				}

				co_await writeRemaining(socket, wire, sent);
			}
			else
			{
				// 大响应（JSON 基本都落这儿）：head + body 两段一起乐观写，命中就省掉一次
				// composed operation；没命中也只是回到原来那次 writev
				std::array<boost::asio::const_buffer, 2> bufs = {boost::asio::buffer(headBuf.data(), headBuf.size()),
																 boost::asio::buffer(nativeRes.body)};
				size_t sent = 0;
				if (tryOptimisticWrite(socket, bufs, sent))
				{
					co_return;
				}

				co_await writeRemaining(socket, bufs, sent);
			}
		}

		/// 把一个响应序列化进批缓冲，不碰 socket。
		/// 只收「head + 整块 body」这种能直接拼的响应；文件体和 chunked 自己会写，
		/// 调用方得先 flush 再单发，不能混进来。
		inline void appendResponseToBatch(std::string& batch,
										  NativeResponse& nativeRes,
										  std::string_view prefix,
										  bool skipBody)
		{
			nativeRes.preparePayload();
			FixedBuffer<512> headBuf;
			serializeHeadWithPrefix(headBuf, nativeRes, prefix);

			const size_t bodyLen = skipBody ? 0 : nativeRes.body.size();
			const size_t needed = batch.size() + headBuf.size() + bodyLen;
			if (needed > batch.capacity())
			{
				// 按几何倍数涨，别固定一上来就占几 KB：只回一条响应就走的连接（绝大多数）
				// 只占「一条响应」那么大。批稳定下来之后这里不再分配。
				batch.reserve((std::max)(needed, batch.capacity() * 2));
			}

			batch.append(headBuf.data(), headBuf.size());
			if (bodyLen > 0)
			{
				batch.append(nativeRes.body.data(), bodyLen);
			}
		}

		/// 发送文件体响应（先发头部，再异步分块读文件发送）
		/// 用于 Range 请求等大文件场景，避免全量加载到内存。
		/// prefix 为空时用原始路径序列化头部。
		template <typename SocketType>
		Awaitable<void> writeFileResponse(SocketType& socket,
										  NativeResponse& nativeRes,
										  std::string_view prefix,
										  std::atomic<size_t>* writeCount)
		{
			// 按「一个文件响应」记一笔，不按 64KB 分块数记
			recordResponseWrite(writeCount);
			nativeRes.preparePayload();

			auto& fb = *nativeRes.fileBody;
			static constexpr size_t kChunkSize = 65536; // 64KB

			// cork 住 socket，让 head + 首个 chunk 合并成一个大 TCP 段再发出去
			TcpCorkGuard cork(socket);

#ifdef BOOST_ASIO_HAS_FILE
			// 先打开文件确认可访问，失败时尚未发送头部，可安全抛异常
			auto executor = co_await boost::asio::this_coro::executor;
			boost::asio::random_access_file file(executor,
												 fb.path.string(),
												 boost::asio::random_access_file::read_only);

			// 文件打开成功，发送头部
			FixedBuffer<512> headBuf;
			serializeHeadWithPrefix(headBuf, nativeRes, prefix);
			co_await boost::asio::async_write(socket,
											  boost::asio::buffer(headBuf.data(), headBuf.size()),
											  boost::asio::use_awaitable);

			// 异步分块发送文件内容
			int64_t remaining = fb.length;
			uint64_t offset = static_cast<uint64_t>(fb.offset);
			std::string chunk((std::min)(static_cast<int64_t>(kChunkSize), remaining), '\0');

			while (remaining > 0)
			{
				auto toRead = (std::min)(static_cast<int64_t>(kChunkSize), remaining);
				auto bytesRead =
					co_await file.async_read_some_at(offset,
													 boost::asio::buffer(chunk.data(), static_cast<size_t>(toRead)),
													 boost::asio::use_awaitable);
				if (bytesRead == 0)
				{
					break;
				}

				co_await boost::asio::async_write(socket,
												  boost::asio::buffer(chunk.data(), bytesRead),
												  boost::asio::use_awaitable);
				offset += bytesRead;
				remaining -= static_cast<int64_t>(bytesRead);
			}
#else
			// ifstream 回退（macOS 等不支持 BOOST_ASIO_HAS_FILE 的平台）
			std::ifstream ifs(fb.path, std::ios::binary);
			if (!ifs)
			{
				throw boost::system::system_error(
					boost::system::errc::make_error_code(boost::system::errc::no_such_file_or_directory));
			}

			FixedBuffer<512> headBuf;
			serializeHeadWithPrefix(headBuf, nativeRes, prefix);
			co_await boost::asio::async_write(socket,
											  boost::asio::buffer(headBuf.data(), headBuf.size()),
											  boost::asio::use_awaitable);

			ifs.seekg(fb.offset);
			int64_t remaining = fb.length;
			std::string chunk((std::min)(static_cast<int64_t>(kChunkSize), remaining), '\0');
			while (remaining > 0 && ifs)
			{
				auto toRead = (std::min)(static_cast<int64_t>(kChunkSize), remaining);
				ifs.read(chunk.data(), toRead);
				auto bytesRead = static_cast<size_t>(ifs.gcount());
				if (bytesRead == 0)
				{
					break;
				}

				co_await boost::asio::async_write(socket,
												  boost::asio::buffer(chunk.data(), bytesRead),
												  boost::asio::use_awaitable);
				remaining -= static_cast<int64_t>(bytesRead);
			}
#endif
			// TcpCorkGuard 析构时 uncork，内核把剩余积压数据一口气发出去
		}

		/// socket 析构守卫（handleSession 用），WS 升级时 transferred=true 就跳过
		template <typename SocketType>
		struct SocketGuard
		{
			SocketType& sock;
			bool transferred {false};
			bool cleanExit {false}; // 正常结束才 shutdown，对端早断了就别白调了

			~SocketGuard()
			{
				if (!transferred && sock.lowest_layer().is_open())
				{
					boost::system::error_code ec;
					if (cleanExit)
					{
						// ssl::stream 的 shutdown 会发 close_notify 握手（慢），对裸 socket 用法也不同；
						// 统一走 lowest_layer() 拿底层 tcp::socket，shutdown/close 语义一致
						sock.lowest_layer().shutdown(tcp::socket::shutdown_send, ec);
					}
					sock.lowest_layer().close(ec);
				}
			}
		};

	} // namespace

	template <typename SocketType>
	Awaitable<void> HttpServer::handleSession(SocketType socket)
	{
		// 连接计数：+1 已在 acceptLoop accept 处占位完成（先占位再校验才能做硬上限，
		// 不能在这里 fetch_add——本协程是 coSpawn 异步投递的，burst 建连时会严重滞后）。
		// 这里只接管计数所有权，由 ConnectionCounter 析构时唯一负责 -1。
		struct ConnectionCounter
		{
			std::atomic<size_t>& count;
			std::atomic<bool>& draining;
			HttpServer& server;

			~ConnectionCounter()
			{
				// fetch_sub 返回旧值；旧值为 1 表示减到 0（最后一个连接）
				if (count.fetch_sub(1) == 1 && draining.load())
				{
					server.stop();
				}
			}
		} connCounter {activeConnections_, draining_, *this};

		// socket 析构守卫，WS 升级时 transferred=true 就跳过
		SocketGuard<SocketType> guard {socket};

		// entry 在协程栈上，Guard 析构时自动注销
		// 声明在 SocketGuard 后面 → 先析构（先 unregister 再关 socket）
		// 注册要赶在 SSL 握手之前：握手期间连接同样处于「已建立但对端沉默」的状态，
		// 不挂进扫描链的话，一个 TCP 连上却不发 ClientHello 的客户端能把连接和 fd 一直占住。
		IdleScanner::Entry idleEntry;
		// SSL 流要存 next_layer() 的底层 tcp::socket*（Entry::socket 就是这个类型），
		// 关闭底层 socket 就能让挂起的 SSL 操作报错退出，不用发 close_notify。
		if constexpr (hIsSslStream<SocketType>)
		{
			idleEntry.socket = &socket.next_layer();
		}
		else
		{
			idleEntry.socket = &socket;
		}
		idleEntry.touch();

		IdleScanner::Guard idleGuard(currentThreadIdleScanner(), idleEntry);

		// SSL 流先握手：底层 socket 由 OpenSSL BIO 状态机管理，不能直接调 non_blocking
		// 去碰 fd（那会破坏 OpenSSL 的非阻塞读写状态），握手失败直接 co_return，SocketGuard 负责关掉。
		if constexpr (hIsSslStream<SocketType>)
		{
			boost::system::error_code handshakeEc;
			co_await socket.async_handshake(boost::asio::ssl::stream_base::server,
											boost::asio::redirect_error(boost::asio::use_awaitable, handshakeEc));
			if (handshakeEc)
			{
				co_return;
			}
		}

		if constexpr (!hIsSslStream<SocketType>)
		{
			// 显式设为非阻塞——tryOptimisticWrite 中的 write_some 在阻塞 fd
			// 上遇到 EAGAIN 会进 poll() 忙等、卡死 io 线程。Asio 异步操作
			// 期间设置的 O_NONBLOCK 是实现细节，不能依赖它保持生效。
			socket.non_blocking(true);
		}

		// 对端地址是连接级信息，取一次供本连接所有请求复用。
		// socket 刚 accept 必然已连接，getpeername() 不会失败，与 TcpServer::acceptLoop 同款写法。
		// ssl::stream 没有 remote_endpoint，走 lowest_layer() 拿底层 tcp::socket。
		auto remoteEp = socket.lowest_layer().remote_endpoint();
		InetAddress peerAddr(remoteEp.address().to_string(), remoteEp.port());

		try
		{
			// 写次数计数器：连接建起来时解析一次，没开统计就是 nullptr。
			// 之后就传这个局部指针，别每个响应去读一次开关——计数器是所有 worker
			// 共用的一条 cache line，生产路径上少一次跨核 fetch_add 就少一次抖动。
			// 关着的时候下游只剩 recordResponseWrite 里那句 if (writeCount != nullptr)，
			// 空指针判断完美预测，跟没这块代码基本一样。
			std::atomic<size_t>* writeCounter =
				writeCountEnabled_.load(std::memory_order_relaxed) ? &responseWriteCount_ : nullptr;

			// 跨请求的粘包残留；大多数连接空着不额外占内存
			std::string pipelineSpill;

			// 响应合批缓冲：同一次读里能解出几个请求，响应就先按顺序拼进来，攒完一次写出去。
			// 跨迭代复用，容量按需涨（见 appendResponseToBatch），稳态零分配。
			// 单请求路径上它最多装一条，flush 时就是原来那一次写。
			std::string responseBatch;
			static constexpr size_t kMaxResponseBatchBytes = 64 * 1024;

			// ── 连接级响应前缀模板 ──
			// 把 Server / Connection / Date 三个通用头部预拼成 wire bytes，
			// 每个请求只做一次 memcpy（~90B），不再走 HeaderMap::insert + 逐字段序列化。
			// Date 每秒最多更新一次（29B memcpy），Connection 在连接断开前不变。
			static constexpr auto kServerHeader = "Server: " HICAL_VERSION_STRING "\r\n";
			static constexpr size_t kServerHeaderLen = std::char_traits<char>::length(kServerHeader);
			static constexpr std::string_view kConnKeepAlive = "Connection: keep-alive\r\n";
			static constexpr std::string_view kConnClose = "Connection: close\r\n";
			static constexpr std::string_view kDatePrefix = "Date: ";
			static constexpr std::string_view kCRLF = "\r\n";
			static constexpr size_t kMaxDateValueLen = 29;

			// 编译期确保版本号再长也不会炸栈
			static constexpr size_t kMaxPrefixLen =
				kServerHeaderLen + kConnKeepAlive.size() + kDatePrefix.size() + kMaxDateValueLen + kCRLF.size();
			static_assert(kMaxPrefixLen <= 128, "responsePrefix buffer too small, bump the array size");

			char responsePrefix[128];
			size_t prefixLen = 0;
			size_t dateValueOffset = 0; // Date 值在 responsePrefix 中的字节偏移
			time_t lastPrefixDateSec = 0;

			// 构建（或重建）前缀的 lambda
			auto rebuildPrefix = [&](bool keepAlive)
			{
				prefixLen = 0;
				auto appendBytes = [&](const char* s, size_t n)
				{
					std::memcpy(responsePrefix + prefixLen, s, n);
					prefixLen += n;
				};

				appendBytes(kServerHeader, kServerHeaderLen);
				auto conn = keepAlive ? kConnKeepAlive : kConnClose;
				appendBytes(conn.data(), conn.size());
				appendBytes(kDatePrefix.data(), kDatePrefix.size());
				dateValueOffset = prefixLen;
				auto date = cachedHttpDate();
				appendBytes(date.data(), date.size());
				appendBytes(kCRLF.data(), kCRLF.size());
				lastPrefixDateSec = std::time(nullptr);
			};

			rebuildPrefix(true); // 默认 keep-alive

			// 所有「要离开当前处理顺序」的地方都得先调它：阻塞读、直接写（错误响应/文件/chunked）、
			// 把 socket 交给 WS/SSE、以及任何 co_return/break。攒着不发就是响应卡在服务端。
			auto flushPendingResponses = [&]() -> Awaitable<void>
			{
				if (!responseBatch.empty())
				{
					co_await flushResponseBatch(socket, responseBatch, writeCounter);
				}
			};

			for (;;)
			{
				// pipeline 有残留时数据已在用户态，直接借缓冲区处理；
				// 无残留时用 256B 栈缓冲做 speculative read（不走 async_wait），
				// async_read_some 走 Asio 投机路径，不触发 epoll_ctl(MOD)，
				// 空闲连接不持有堆缓冲区，保持之前百万连接的内存优化
				size_t bufUsed = pipelineSpill.size();
				ReadBufferPool::BufferHandle readBufHandle;

				if (bufUsed == 0)
				{
					// 要阻塞等下一批数据了：攒着没发出去的响应先落地。
					// 不在这里发的话，客户端等响应、我们等数据，两边干等到超时。这是合批的主出口。
					co_await flushPendingResponses();

					// 路径 A：空闲连接 → 栈缓冲 speculative read，零 epoll_ctl(MOD)
					char smallBuf[256];
					size_t bytesRead = co_await socket.async_read_some(boost::asio::buffer(smallBuf, sizeof(smallBuf)),
																	   boost::asio::use_awaitable);
					if (bytesRead == 0)
					{
						break;
					}

					readBufHandle = ReadBufferPool::acquire();
					{
						auto& buf = readBufHandle.get();
						size_t initSize = std::max(bytesRead, ReadBufferPool::kBufferSize);
						buf.resize(initSize);
						std::memcpy(buf.data(), smallBuf, bytesRead);
					}
					bufUsed = bytesRead;
				}
				else
				{
					// 路径 B：有 pipeline 残留 → 直接借 8KB readBuf
					readBufHandle = ReadBufferPool::acquire();
					{
						auto& buf = readBufHandle.get();
						size_t initSize = std::max(bufUsed, ReadBufferPool::kBufferSize);
						if (initSize > buf.capacity())
						{
							buf.reserve(initSize);
						}
						buf.resize(initSize);
						std::memcpy(buf.data(), pipelineSpill.data(), bufUsed);
					}
					pipelineSpill.clear();
				}

				std::string& readBuf = readBufHandle.get();

				// picohttpparser 输出
				const char* method = nullptr;
				size_t methodLen = 0;
				const char* path = nullptr;
				size_t pathLen = 0;
				int minorVersion = 0;
				struct phr_header headers[64];
				size_t numHeaders = 64;
				int parseResult = -2; // -2 = 不完整

				// ====== 阶段 A：读取并解析 HTTP 头部 ======
				size_t prevBufLen = 0;
				for (;;)
				{
					// Pipeline：buf 里有残留数据就先试着解析，不用再 read
					if (bufUsed > prevBufLen)
					{
						numHeaders = 64;
						parseResult = phr_parse_request(readBuf.data(),
														bufUsed,
														&method,
														&methodLen,
														&path,
														&pathLen,
														&minorVersion,
														headers,
														&numHeaders,
														prevBufLen);
						prevBufLen = bufUsed;

						if (parseResult > 0)
						{
							break; // 完整请求已在缓冲区，零 syscall
						}
						if (parseResult == -1)
						{
							co_await flushPendingResponses();
							co_await sendRawResponse(socket,
													 400,
													 "Bad Request",
													 "Malformed HTTP request",
													 writeCounter);
							co_return;
						}
						// parseResult == -2：数据不完整，fall through 到 async_read_some
					}

					// 确保缓冲区有空间
					if (bufUsed >= readBuf.size())
					{
						if (readBuf.size() >= maxHeaderSize_)
						{
							// 头部过大
							co_await flushPendingResponses();
							co_await sendRawResponse(socket,
													 431,
													 "Request Header Fields Too Large",
													 "Request header too large",
													 writeCounter);
							co_return;
						}
						readBuf.resize(readBuf.size() * 2);
					}

					// 头部还没收全，这里要阻塞等数据——批里攒着的响应先放出去，
					// 否则客户端拿着半截请求等响应，我们等剩下的头部，谁都动不了
					co_await flushPendingResponses();

					auto bytesRead = co_await socket.async_read_some(
						boost::asio::buffer(readBuf.data() + bufUsed, readBuf.size() - bufUsed),
						boost::asio::use_awaitable);
					bufUsed += bytesRead;

					numHeaders = 64;
					parseResult = phr_parse_request(readBuf.data(),
													bufUsed,
													&method,
													&methodLen,
													&path,
													&pathLen,
													&minorVersion,
													headers,
													&numHeaders,
													prevBufLen);
					prevBufLen = bufUsed;

					if (parseResult > 0)
					{
						// 头部解析完成。TCP 是流式，buf 里可能还粘连着 body 残留，
						// 此时 bufUsed 是「header+body」的总长，判断超限只能看 picohttpparser
						// 给出的 header 字节数（parseResult），不能看 bufUsed。
						if (static_cast<size_t>(parseResult) > maxHeaderSize_)
						{
							co_await flushPendingResponses();
							co_await sendRawResponse(socket,
													 431,
													 "Request Header Fields Too Large",
													 "Request header too large",
													 writeCounter);
							co_return;
						}
						break;
					}
					if (parseResult == -1)
					{
						// 解析错误
						co_await flushPendingResponses();
						co_await sendRawResponse(socket, 400, "Bad Request", "Malformed HTTP request", writeCounter);
						co_return;
					}

					// parseResult == -2：头部不完整。这里 buf 里的字节都是还没解析完的 header
					// 前缀（header 一旦完整 picohttpparser 就返回 >0，不会带着 body 返回 -2），
					// 所以用 bufUsed 判断超限是准确的。
					if (bufUsed > maxHeaderSize_)
					{
						co_await flushPendingResponses();
						co_await sendRawResponse(socket,
												 431,
												 "Request Header Fields Too Large",
												 "Request header too large",
												 writeCounter);
						co_return;
					}
					// 数据不完整，继续读取
				}

				// 读完头部，刷新活跃时间
				idleEntry.touch();

				// ====== 阶段 B：构建 NativeRequest ======
				NativeRequest nativeReq;
				nativeReq.method = methodFromStringView(std::string_view(method, methodLen));
				nativeReq.target = std::string_view(path, pathLen);
				nativeReq.httpVersionMajor = 1;
				nativeReq.httpVersionMinor = minorVersion;

				// 复制头部，同时检测关键头部
				size_t contentLength = 0;
				bool hasContentLength = false;
				bool isChunked = false;
				bool hasTransferEncoding = false;
				bool invalidFraming = false;
				auto transferEncodingStatus = HttpStatusCode::hOk;
				bool connectionClose = false;     // 逮到 Connection: close 就记下来
				bool hasConnectionHeader = false; // 有 Connection 头就把这位置 1

				nativeReq.headers.clear();
				for (size_t i = 0; i < numHeaders; ++i)
				{
					// picohttpparser represents obsolete folded lines with a null name.
					// Reject them rather than silently ignoring a continuation of a framing field.
					if (headers[i].name == nullptr)
					{
						invalidFraming = true;
						continue;
					}
					std::string_view hname(headers[i].name, headers[i].name_len);
					std::string_view hvalue(headers[i].value, headers[i].value_len);

					nativeReq.headers.add(hname, hvalue);

					// 按长度+首字符快速过滤，将 20 次 iequals 降到 ~2 次
					// Content-Length: 长度 14，首字符 C/c
					// Transfer-Encoding: 长度 17，首字符 T/t
					// Connection: 长度 10，首字符 C/c
					// Expect: 长度 6，首字符 E/e
					if (hname.size() == 14 && (hname[0] == 'C' || hname[0] == 'c'))
					{
						if (HeaderMap::iequals(hname, "Content-Length"))
						{
							size_t parsedLength = 0;
							auto [ptr, ec] =
								std::from_chars(hvalue.data(), hvalue.data() + hvalue.size(), parsedLength);
							invalidFraming |= ec != std::errc {} || ptr != hvalue.data() + hvalue.size()
											  || (hasContentLength && parsedLength != contentLength);
							hasContentLength = true;
							contentLength = parsedLength;
						}
					}
					else if (hname.size() == 10 && (hname[0] == 'C' || hname[0] == 'c'))
					{
						if (HeaderMap::iequals(hname, "Connection"))
						{
							// 只认首个 Connection 头，和 RequestHeaders::find 的语义对齐
							if (!hasConnectionHeader)
							{
								// 顺手判掉 upgrade，isUpgrade() 就不用再扫一遍头部
								nativeReq.upgradePreScanned = detail::connectionHasUpgradeToken(hvalue);
							}
							hasConnectionHeader = true;
							connectionClose = HeaderMap::iequals(hvalue, "close");
						}
					}
					else if (hname.size() == 6 && (hname[0] == 'E' || hname[0] == 'e'))
					{
						if (HeaderMap::iequals(hname, "Expect"))
						{
							nativeReq.expectContinue = HeaderMap::iequals(hvalue, "100-continue");
						}
					}
					else if (hname.size() == 17 && (hname[0] == 'T' || hname[0] == 't'))
					{
						if (HeaderMap::iequals(hname, "Transfer-Encoding"))
						{
							// Conservatively reject multiple TE fields instead of merging coding chains.
							invalidFraming |= hasTransferEncoding;
							hasTransferEncoding = true;
							transferEncodingStatus = validateTransferEncoding(hvalue);
							isChunked = transferEncodingStatus == HttpStatusCode::hOk;
						}
					}
				}

				// Validate framing before routing, middleware, 100 Continue, or reading any body.
				// An error closes the connection so buffered bytes cannot become another request.
				if (invalidFraming || transferEncodingStatus == HttpStatusCode::hBadRequest
					|| (hasTransferEncoding && (hasContentLength || minorVersion == 0)))
				{
					co_await flushPendingResponses();
					co_await sendRawResponse(socket, 400, "Bad Request", "Invalid request framing", writeCounter);
					co_return;
				}
				if (transferEncodingStatus == HttpStatusCode::hNotImplemented)
				{
					co_await flushPendingResponses();
					co_await sendRawResponse(socket,
											 501,
											 "Not Implemented",
											 "Unsupported Transfer-Encoding",
											 writeCounter);
					co_return;
				}

				// 算 keep-alive：header 解析时已经顺手抓了 Connection 头的值，
				// 不用再跑一遍 O(N) find。
				// 有 Connection 头就用预扫结果；没有就按 HTTP 版本走默认。
				if (hasConnectionHeader)
				{
					nativeReq.keepAlive = !connectionClose;
				}
				else
				{
					// HTTP/1.1 默认 keep-alive，HTTP/1.0 默认 close
					nativeReq.keepAlive = (minorVersion >= 1);
				}

				// ====== 前置路由匹配（阶段 B.5）：读 body 之前先 resolveRoute ======
				// 从这往下的时序两个不变量：① 中间件在 body 之前执行（req.body() 那时是空的），
				// ② 无效请求（404/405/超深路径）在读 body 前就拒绝，body 一字不读。
				// body 现在还是空的，但路由匹配只看 method+path，不受影响。
				// 提前构建 HttpRequest，后续 body 读取通过 req.native() 写入（阶段 C 填充）。
				HttpRequest req = HttpRequest::fromParsed(std::move(nativeReq));
				req.setPeerAddr(peerAddr);

				// 一次性 resolveRoute，结果缓存到 resolveResult，dispatch 阶段复用，绝不二次匹配。
				// 有效请求命中后照常读 body；无效请求（404/405/超深）在读 body 之前就直接拒绝，
				// 避免把整个 body 白读进内存。
				auto resolveResult = router_.resolveRoute(req);
				bool hasRequestBody = (hasContentLength && contentLength > 0) || isChunked;

				if (resolveResult.pathTooDeep)
				{
					// 超深路径：resolveRoute 直接判定，400 语义与 dispatch 内一致，body 不用读
					co_await flushPendingResponses();
					co_await sendRawResponse(socket, 400, "Bad Request", "Path too deep", writeCounter);
					co_return;
				}

				if (!resolveResult.isMatch() && hasRequestBody)
				{
					// 未命中且带 body：带 body 的请求绝不可能是 WS 升级（GET）或 SSE（GET），
					// 这里可以安全地提前拒绝，不必读 body。
					if (resolveResult.isMethodNotAllowed())
					{
						const std::string& allowed = resolveResult.allowedMethods;
						HttpResponse res;
						res.setStatus(HttpStatusCode::hMethodNotAllowed);
						res.setHeader("Allow", allowed);
						res.setBody("Method Not Allowed", "text/plain");
						auto& nativeRes = res.native();
						nativeRes.httpVersionMinor = 1;
						// body 还没读，连接状态已脏，必须 close，避免下个请求把残留 body 当请求头解析
						nativeRes.keepAlive = false;
						nativeRes.headers.set("Connection", "close");
						co_await flushPendingResponses();
						co_await writeResponse(socket, nativeRes, false, writeCounter);
						co_return;
					}
					co_await flushPendingResponses();
					co_await sendRawResponse(socket, 404, "Not Found", "Not Found", writeCounter);
					co_return;
				}
				// 未命中且无 body 的（GET 打不存在的路径之类）走原路，留给阶段 D 的 WS/SSE/dispatch 再定
				// ——反正没 body 可读，「提前拒绝省内存」在这里没啥便宜可占。

				// ====== 阶段 C：读取 Body（抽成 lambda，供中间件 final next 与无中间件路径共用） ======
				// 中间件前置后 body 在中间件之后才读，这个 lambda 只在「中间件已通过、或无中间件」时被调用。
				// 返回 nullopt = body 读成功（继续 dispatch）；返回 HttpResponse = 读 body 阶段出错（413/400 等），
				// 调用方直接写回该响应并关闭连接（body 没读完，连接已脏）。
				size_t headerBytes = static_cast<size_t>(parseResult);
				size_t remainingInBuf = bufUsed - headerBytes;

				// 残留数据（下一个 pipelined 请求的字节）在 readBuf 里的起始偏移。
				// 要等阶段 D 用完 string_view 才能挪，所以这里只记位置，响应写完再存进 pipelineSpill
				size_t spillSrc = 0;

				// body 读没读过的标记。链里有 needsBody 声明时会插一道门，门和 tail 共用同一个
				// readRequestBody：谁先读就把这块置上，另一个看到就跳过，保证 body 只读一遍、
				// spillSrc/bufUsed 这些副作用也只落一次。链里没门时它恒为 false，tail 照旧兜底读。
				bool bodyRead = false;

				auto readRequestBody = [&]() -> Awaitable<std::optional<HttpResponse>>
				{
					// 读过就直接返回成功，保证 body 只读一遍。调用方不止一个：全局链上的门、
					// tail 兜底、还有组级 useBody 挂在路由 handler 上的局部链门——组级那道门
					// 在 dispatchResolved 里才跑到，那时 body 早读过了，靠这个判断挡掉二次读。
					// 少了它，组级 useBody + 超出一个 readBuf（8KB）的 body 会在第二次读时挂死。
					if (bodyRead)
					{
						co_return std::nullopt;
					}
					// 成功失败都算读过：失败时 body 没读完、连接已脏，调用方会短路写回错误响应，
					// 谁都不该再读第二遍。
					bodyRead = true;

					// 读 body 阶段出错：body 未读完，连接已脏，必须 close
					auto bodyError = [](HttpStatusCode code, const std::string& msg)
					{
						HttpResponse res;
						res.setStatus(code);
						res.setBody(msg, "text/plain");
						res.native().keepAlive = false;
						return res;
					};

					// Expect: 100-continue 处理（仅 HTTP/1.1+，有 body 时才生效）
					// 路由是否存在已经在阶段 B.5 的 resolveRoute 里判过：带 body 且走到这里，
					// 必然是 resolveResult.isMatch() 为 true（未命中带 body 的早在 964 行就 co_return 了）。
					// 所以这里不能再调 router_.exists() 二次匹配——exists 只查 static + 同 method 的 param 路由，
					// 不查 wildcard，范围比 resolveRoute 窄，会漏掉 wildcard 路由，把正常请求误判成 404。
					// 真需要提前拒绝的只有超限这一种，直接交给下面的 Content-Length 预检。
					if (req.native().expectContinue && (hasContentLength || isChunked) && minorVersion >= 1)
					{
						// Content-Length 已知时，超限在发 100 前就拒——这才是 Expect 省带宽的意义所在。
						// chunked 长度未知，只能边读边查（维持现状）。
						if (hasContentLength && contentLength > maxBodySize_)
						{
							co_return bodyError(HttpStatusCode::hPayloadTooLarge, "Request body too large");
						}

						constexpr std::string_view k100 = "HTTP/1.1 100 Continue\r\n\r\n";
						// 100-continue 也是往 socket 上写：前面攒着的响应必须先出去，不能让 100 插队
						co_await flushPendingResponses();
						co_await boost::asio::async_write(socket,
														  boost::asio::buffer(k100.data(), k100.size()),
														  boost::asio::use_awaitable);
					}

					if (hasContentLength && contentLength > 0)
					{
						// Content-Length body 读取
						if (contentLength > maxBodySize_)
						{
							co_return bodyError(HttpStatusCode::hPayloadTooLarge, "Request body too large");
						}

						req.native().body.resize(contentLength);
						size_t bodyCopied = std::min(remainingInBuf, contentLength);
						if (bodyCopied > 0)
						{
							std::memcpy(req.native().body.data(), readBuf.data() + headerBytes, bodyCopied);
						}

						// 把 body 后面的残留数据位置记下来，响应写完再存进 pipelineSpill
						size_t tailLen = remainingInBuf - bodyCopied;
						if (tailLen > 0)
						{
							spillSrc = headerBytes + bodyCopied;
						}
						bufUsed = tailLen;

						size_t bodyRemaining = contentLength - bodyCopied;
						size_t offset = bodyCopied;
						if (bodyRemaining > 0)
						{
							// 要阻塞等 body 了：攒着的响应先落地。客户端多半是收完前一个响应才发 body 的，
							// 攒着不发就是两边互相等
							co_await flushPendingResponses();
						}
						while (bodyRemaining > 0)
						{
							auto bytesRead = co_await socket.async_read_some(
								boost::asio::buffer(req.native().body.data() + offset, bodyRemaining),
								boost::asio::use_awaitable);
							offset += bytesRead;
							bodyRemaining -= bytesRead;
						}
					}
					else if (isChunked)
					{
						// Chunked transfer-encoding 解码
						// phr_decode_chunked 是原地解码：将 chunk 帧头剥离，解码数据覆写到同一缓冲区。
						// 契约：输入 chunkBuf[0..size) 是待解码编码数据，*bufsz 返回产出解码字节数 dst。
						//   ret == -2：不完整，且此时编码数据已被全部消费（内部 src 推到 bufsz），剩余编码 = 0；
						//   ret == -1：错误；
						//   ret >= 0：完成，返回值 = 未解码尾部字节数（chunked 之后的 pipeline 残留），
						//             位于 chunkBuf[dst .. dst+ret)。
						// 因此每次循环必须保证 chunkBuf 从头就是「未消费的编码数据」，ret == -2 时
						// 清空缓冲重新累积，而不是推进一个错误的编码消费偏移。
						std::string chunkBuf;
						if (remainingInBuf > 0)
						{
							chunkBuf.assign(readBuf.data() + headerBytes, remainingInBuf);
						}
						// chunked 路径消费了 readBuf 所有残留数据，清零
						bufUsed = 0;

						struct phr_chunked_decoder decoder = {};

						for (;;)
						{
							size_t decodeBufLen = chunkBuf.size();
							auto decodeRet = phr_decode_chunked(&decoder, chunkBuf.data(), &decodeBufLen);

							// decodeBufLen = 本次产出解码字节数，数据位于 chunkBuf[0..decodeBufLen)
							req.native().body.append(chunkBuf.data(), decodeBufLen);

							if (req.native().body.size() > maxBodySize_)
							{
								co_return bodyError(HttpStatusCode::hPayloadTooLarge, "Request body too large");
							}

							if (decodeRet >= 0)
							{
								// 解码完成，把 chunked 之后的残留（keep-alive pipeline）写回 pipelineSpill，
								// 否则下一个请求会被丢掉
								size_t tailLen = static_cast<size_t>(decodeRet);
								if (tailLen > 0)
								{
									pipelineSpill.assign(chunkBuf.data() + decodeBufLen, tailLen);
								}
								break;
							}
							if (decodeRet == -1)
							{
								// 解码错误
								co_return bodyError(HttpStatusCode::hBadRequest, "Malformed chunked encoding");
							}

							// decodeRet == -2：编码数据已全部消费，清空后继续读
							chunkBuf.clear();

							// 阻塞读之前把攒着的响应放出去。第一次读之前批里才可能有东西，
							// 后面几轮读完就接着解码、不会攒新的，调了也是空转
							co_await flushPendingResponses();
							chunkBuf.resize(4096);
							auto bytesRead = co_await socket.async_read_some(boost::asio::buffer(chunkBuf.data(), 4096),
																			 boost::asio::use_awaitable);
							if (bytesRead == 0)
							{
								// 对端在不完整 chunked body 时断开，避免死循环
								co_return bodyError(HttpStatusCode::hBadRequest, "Incomplete chunked body");
							}
							chunkBuf.resize(bytesRead);
						}
					}
					else
					{
						// 无 body（GET/HEAD 等）：残留数据就在头部后面
						spillSrc = headerBytes;
						bufUsed = remainingInBuf;
					}

					co_return std::nullopt;
				};

				// ====== 阶段 D：分发（中间件已前置到读 body 之前） ======
				// WS/SSE 有独立的 wsMiddlewareChain_，在各自分支单独执行，这里跳过全局中间件，
				// 避免同一组中间件的链被跑两遍（认证/日志会执行两次）。
				bool isWsOrSse = false;
				if constexpr (!hIsSslStream<SocketType>)
				{
					if (req.native().isUpgrade() && router_.findWsRoute(req.path()).route)
					{
						isWsOrSse = true;
					}
					else if (req.method() == HttpMethod::hGet && router_.findSseRoute(req.path()).route)
					{
						isWsOrSse = true;
					}
				}

				HttpResponse res;
				if (!isWsOrSse)
				{
					if (middlewarePipeline_.size() > 0)
					{
						// 中间件在 body 之前执行。最终 next 的 per-request 状态（读 body + 前置 resolveRoute 缓存
						// 结果 dispatch）装进栈上 ctx，挂到 req 内部槽，走 start() 预构建的 cachedChain_。
						// 链只 build 一次，每请求零重建；中间件后置拿真实 handler 响应，
						// 短路（认证失败等）时不触发终端骨架，body 一字不读。
						auto tail = [&](HttpRequest& r) -> Awaitable<HttpResponse>
						{
							// 门（链里有 needsBody 声明才插）已经替我读过 body 时 bodyRead 为真，
							// 这次跳过兜底读；没有门（普通请求）就照旧读一遍。两条路都没多出协程帧：
							// 没门时这次就是原来那次，有门时省下的正是尾巴上多读一遍的代价。
							if (!bodyRead)
							{
								if (auto err = co_await readRequestBody(); err)
								{
									co_return std::move(*err);
								}
							}
							if (auto sync = router_.dispatchSyncResolved(r, resolveResult); sync)
							{
								co_return std::move(*sync);
							}
							co_return co_await router_.dispatchResolved(r, resolveResult);
						};
						detail::RequestDispatchContext ctx;
						ctx.obj = &tail;
						ctx.invoke = &detail::tailInvoke<decltype(tail)>;
						// 把「读 body」入口也交给链。声明了 needsBody 的管线里有一道门，门取这两个
						// 字段在声明者之前把 body 读掉；没声明时门不存在，这两个字段没人调，body 由
						// 上面的 tail 兜底读。obj/invoke 与 bodyObj/readBody 都指向本 for 循环栈上的
						// 闭包，链执行完即失效，attach(nullptr) 会一并解绑。
						ctx.bodyObj = &readRequestBody;
						ctx.readBody = &detail::bodyInvoke<decltype(readRequestBody)>;
						detail::RequestDispatchContext::attach(req, &ctx);
						try
						{
							res = co_await middlewarePipeline_.execute(req);
						}
						catch (const std::exception& e)
						{
							if (errorHandler_)
							{
								try
								{
									res = errorHandler_(e, req);
								}
								catch (...)
								{
									// errorHandler 自身抛异常时 fallback
									res = HttpResponse::serverError();
								}
							}
							else
							{
								res = HttpResponse::serverError();
							}
						}
						catch (...)
						{
							res = HttpResponse::serverError();
						}
						// 链执行完显式解绑：ctx 是栈上的裸指针槽，光靠「每请求新建 req 默认归零」
						// 这条隐式不变量防悬空太脆弱。趁 ctx 还活着把槽清空，以后就算复用 req 也
						// 不会拿到已析构的 ctx 指针。
						detail::RequestDispatchContext::attach(req, nullptr);
					}
					else
					{
						// 无中间件：读 body 后走同步快速路径 dispatchSyncResolved，
						// 异步/编译期链 handler 再回退到 dispatchResolved。
						try
						{
							if (auto err = co_await readRequestBody(); err)
							{
								res = std::move(*err);
							}
							else
							{
								auto syncResult = router_.dispatchSyncResolved(req, resolveResult);
								if (syncResult)
								{
									res = std::move(*syncResult);
								}
								else
								{
									res = co_await router_.dispatchResolved(req, resolveResult);
								}
							}
						}
						catch (const std::exception& e)
						{
							if (errorHandler_)
							{
								try
								{
									res = errorHandler_(e, req);
								}
								catch (...)
								{
									res = HttpResponse::serverError();
								}
							}
							else
							{
								res = HttpResponse::serverError();
							}
						}
						catch (...)
						{
							res = HttpResponse::serverError();
						}
					}
				}

				// 检查 WebSocket 升级请求
				// SSL 下 WS/SseSession 按值收 tcp::socket，无法接收 ssl::stream，
				// 编译期排除掉，Upgrade 请求自然落到后面 404 的普通 HTTP 分发路径。
				if constexpr (!hIsSslStream<SocketType>)
				{
					if (req.native().isUpgrade())
					{
						auto reqPath = req.path();

						auto wsMatch = router_.findWsRoute(reqPath);
						if (wsMatch.route)
						{
							const auto& wsRoute = *wsMatch.route;

							// socket 很快要整个交给 WS 会话（拒绝升级的分支也会自己写响应），
							// 批里攒着的响应先发出去，不能留到所有权转走之后
							co_await flushPendingResponses();

							// 注入 WebSocket 参数路由捕获的参数
							for (const auto& [name, value] : wsMatch.params)
							{
								req.setParam(name, value);
							}

							// Origin 白名单校验（CSWSH 防护）
							if (!wsRoute.allowedOrigins.empty())
							{
								// 透明哈希：string_view 直接查找，零临时 string 堆分配
								auto origin = req.header("Origin");
								if (wsRoute.allowedOrigins.find(origin) == wsRoute.allowedOrigins.end())
								{
									HttpResponse forbiddenRes;
									forbiddenRes.setStatus(HttpStatusCode::hForbidden);
									forbiddenRes.setBody("403 Forbidden: Origin not allowed", "text/plain");
									auto& nativeRes = forbiddenRes.native();
									nativeRes.httpVersionMinor = 1;
									nativeRes.headers.set("Connection", "close");
									co_await writeResponse(socket, nativeRes, false, writeCounter);
									co_return;
								}
							}

							// WebSocket 升级也走中间件管道（认证/限流/日志等）
							if (wsMiddlewareChain_)
							{
								auto wsAuthRes = co_await wsMiddlewareChain_(req);

								auto wsAuthCode = wsAuthRes.statusCode();
								if (wsAuthCode != HttpStatusCode::hOk)
								{
									// 中间件拦截了（401/403 之类），拒绝升级
									auto& nativeRes = wsAuthRes.native();
									nativeRes.httpVersionMinor = 1;
									nativeRes.headers.set("Connection", "close");
									co_await writeResponse(socket, nativeRes, false, writeCounter);
									co_return;
								}
							}

							// 提前拷贝 WS 握手头部（string_view 引用 readBuf，release 后悬挂）
							std::string wsKey(req.native().headers.find("Sec-WebSocket-Key"));
							std::string wsExtensions(req.native().headers.find("Sec-WebSocket-Extensions"));
							std::string wsProtocol(req.native().headers.find("Sec-WebSocket-Protocol"));

							// 验证必须在 readBuf 归还前完成（validateWsUpgrade 读 headers 的 string_view）
							if (validateWsUpgrade(req.native()).empty())
							{
								HttpResponse badRes;
								badRes.setStatus(HttpStatusCode::hBadRequest);
								badRes.setBody("400 Bad Request: invalid WebSocket upgrade", "text/plain");
								auto& nativeRes = badRes.native();
								nativeRes.httpVersionMinor = 1;
								nativeRes.headers.set("Connection", "close");
								co_await writeResponse(socket, nativeRes, false, writeCounter);
								co_return;
							}

							// socket 所有权转移给 WebSocket 会话，标记 guard 跳过析构
							guard.transferred = true;
							// readBuf 已不再需要（握手字段已拷贝），提前归还，
							// 避免 WS 长连接期间占用 + 解耦 tlsPool 的析构顺序
							readBufHandle.release();
							// socket 即将 move 走，handleSession 的 idleEntry 指针立刻失效，
							// 必须在 move 前注销，防止悬空指针残留在 scanner 链表
							idleGuard.release();
							co_await handleWebSocket(std::move(socket),
													 std::move(wsKey),
													 std::move(wsExtensions),
													 std::move(wsProtocol),
													 wsRoute);
							co_return;
						}
					}
				}

				// 检查 SSE 路由（GET 请求匹配 SSE 路由时转换为 SSE 长连接）
				// 同样编译期排除：SSL 下 SSE 不支持升级，落到普通分发。
				if constexpr (!hIsSslStream<SocketType>)
				{
					if (req.method() == HttpMethod::hGet)
					{
						auto reqPath = req.path();
						auto sseMatch = router_.findSseRoute(reqPath);
						if (sseMatch.route)
						{
							const auto& sseRoute = *sseMatch.route;

							// 同上：SSE 接管 socket 之前，批里的响应先落地
							co_await flushPendingResponses();

							// 注入 SSE 参数路由捕获的参数
							for (const auto& [name, value] : sseMatch.params)
							{
								req.setParam(name, value);
							}

							// SSE 路由也走中间件管道（认证/限流/日志等）
							if (wsMiddlewareChain_)
							{
								auto sseAuthRes = co_await wsMiddlewareChain_(req);
								auto sseAuthCode = sseAuthRes.statusCode();
								if (sseAuthCode != HttpStatusCode::hOk)
								{
									auto& nativeRes = sseAuthRes.native();
									nativeRes.httpVersionMinor = 1;
									nativeRes.headers.set("Connection", "close");
									co_await writeResponse(socket, nativeRes, false, writeCounter);
									co_return;
								}
							}

							// readBuf 归还（SseSession 不依赖它）
							readBufHandle.release();

							// socket 所有权转移给 SSE 会话，标记 guard 跳过析构
							guard.transferred = true;
							// handleSession 的 idleEntry 指针失效，在 move 前注销
							idleGuard.release();

							co_await handleSseSession(std::move(socket), sseRoute);
							co_return;
						}
					}
				}

				// 通用头部走预构建前缀，不再 insert 到 HeaderMap
				auto& nativeRes = res.native();
				nativeRes.httpVersionMinor = 1;
				bool shouldKeepAlive = req.native().keepAlive && !draining_.load();
				nativeRes.keepAlive = shouldKeepAlive;

				// 发送响应（scatter-gather I/O：状态行+头部在栈，body 零拷贝）
				// HEAD 方法：仅发送头部，不发送 body（RFC 7231 §4.3.2）
				bool isHead = (req.method() == HttpMethod::hHead);
				std::string_view prefix {responsePrefix, prefixLen};

				if (!shouldKeepAlive)
				{
					// 连接即将关闭——先把批里攒着的前几个响应落地，再走原始路径
					co_await flushPendingResponses();
					nativeRes.headers.insert("Server", HICAL_VERSION_STRING);
					nativeRes.headers.insert("Connection", "close");
					nativeRes.headers.insert("Date", cachedHttpDate());
					if (nativeRes.hasFileBody() && !isHead)
					{
						co_await writeFileResponse(socket, nativeRes, {}, writeCounter);
					}
					else
					{
						co_await writeResponse(socket, nativeRes, isHead, writeCounter);
					}
				}
				else
				{
					// 快速路径：Date 过期才更新（每秒最多一次 29B memcpy）
					auto now = std::time(nullptr);
					if (now != lastPrefixDateSec)
					{
						lastPrefixDateSec = now;
						auto date = cachedHttpDate();
						std::memcpy(responsePrefix + dateValueOffset, date.data(), date.size());
					}

					if (nativeRes.hasFileBody() || nativeRes.hasChunkedBody())
					{
						// 这两种响应自己往 socket 上写（文件分块读、chunked 自己凑帧），
						// 混进批里顺序就乱了：先把前面的响应发出去，再单独发它
						co_await flushPendingResponses();
						if (nativeRes.hasFileBody() && !isHead)
						{
							co_await writeFileResponse(socket, nativeRes, prefix, writeCounter);
						}
						else
						{
							co_await writeResponse(socket, nativeRes, prefix, isHead, writeCounter);
						}
					}
					else if (nativeRes.body.size() >= kMaxResponseBatchBytes)
					{
						// 大响应：拼进批里只是白抄一遍（批里还有东西就先落地，别乱序）
						co_await flushPendingResponses();
						co_await writeResponse(socket, nativeRes, prefix, isHead, writeCounter);
					}
					else if (responseBatch.empty() && bufUsed == 0)
					{
						// 批里空着、缓冲区里也没有下一个请求的字节——这次就是单独一条响应。
						// 直接发，跟合批之前一模一样：不多一次拷贝，也不多一次分配。
						co_await writeResponse(socket, nativeRes, prefix, isHead, writeCounter);
					}
					else
					{
						appendResponseToBatch(responseBatch, nativeRes, prefix, isHead);
						if (responseBatch.size() >= kMaxResponseBatchBytes)
						{
							// 攒到上限就先落一盘，别让一个贪心客户端把内存顶起来
							co_await flushResponseBatch(socket, responseBatch, writeCounter);
						}
					}
				}

				// 写完，刷新活跃时间
				idleEntry.touch();

				// 残留数据直接在原位置暂存，不再先 memmove 到缓冲区开头——反正下一轮
				// 也是 memcpy 进另一个 readBuf，中间那次搬没有意义。
				// bufUsed 就是残留长度：Content-Length 路径 bufUsed = tailLen，无 body 路径
				// bufUsed = remainingInBuf，两种情况下 spillSrc 都正好指向残留的起点；
				// 没读过 body 的路径 spillSrc 还是 0，从头存，跟以前一样。
				// pipelineSpill 已暂存、响应已发送，req.native().target/headers 的 string_view
				// 不再被引用，提前把缓冲区还回去。别等到协程末尾析构——万连接下能省几十 MB 在途内存。
				if (bufUsed > 0)
				{
					pipelineSpill.assign(readBuf.data() + spillSrc, bufUsed);
				}
				readBufHandle.release();

				if (!shouldKeepAlive)
				{
					guard.cleanExit = true;
					break;
				}
			}
		}
		catch (const boost::system::system_error& e)
		{
			if (e.code() != boost::asio::error::eof && e.code() != boost::asio::error::connection_reset
				&& e.code() != boost::asio::error::operation_aborted)
			{
				// 忽略正常的连接关闭
			}
		}

		// idleGuard 先析构注销 entry，然后 SocketGuard 关 socket
	}

	Awaitable<void> HttpServer::handleWebSocket(tcp::socket socket,
												std::string wsKey,
												std::string wsExtensions,
												std::string wsProtocol,
												const Router::WsRoute& wsRoute)
	{
		std::shared_ptr<WebSocketSession> session;

		// WebSocket 空闲超时 timer 在 try 外声明，catch 块需要访问以取消 timer 防竞态
		std::optional<boost::asio::steady_timer> wsDeadline;
		auto wsTimeoutDuration = std::chrono::milliseconds(static_cast<int64_t>(idleTimeout_ * 1000));
		if (idleTimeout_ > 0)
		{
			wsDeadline.emplace(socket.get_executor());
		}

		try
		{
			// 入参 wsKey/wsExtensions/wsProtocol 已是 owned string，readBuf 已归还

			// 1. 计算 Sec-WebSocket-Accept（validateWsUpgrade 已在调用方完成）
			auto acceptKey = computeWsAcceptKey(wsKey);

			// 3. 协商 permessage-deflate
			WsDeflateNegotiation deflateNeg;
			WsCompressionConfig compressionCfg;
			compressionCfg.enabled = wsRoute.enableCompression;
			compressionCfg.serverMaxWindowBits = wsRoute.serverMaxWindowBits;
			compressionCfg.clientMaxWindowBits = wsRoute.clientMaxWindowBits;
			compressionCfg.serverNoContextTakeover = wsRoute.serverNoContextTakeover;

			if (wsRoute.enableCompression && !wsExtensions.empty())
			{
				deflateNeg = negotiateDeflate(wsExtensions, compressionCfg);
			}

			// 4. 协商子协议（Feature 5）
			std::string negotiatedProtocol;
			if (!wsRoute.subprotocols.empty() && !wsProtocol.empty())
			{
				negotiatedProtocol = negotiateSubprotocol(wsProtocol, wsRoute.subprotocols);
			}

			// 5. 发送 101 Switching Protocols（FixedBuffer<512> 栈上零堆分配）
			FixedBuffer<512> responseBuf;
			buildWsAcceptResponse(responseBuf,
								  acceptKey,
								  deflateNeg.accepted ? &deflateNeg : nullptr,
								  negotiatedProtocol);
			co_await boost::asio::async_write(socket,
											  boost::asio::buffer(responseBuf.data(), responseBuf.size()),
											  boost::asio::use_awaitable);

			// 6. 创建 WebSocketSession（shared_ptr 以支持 WsHub）
			session = std::make_shared<WebSocketSession>(std::move(socket),
														 WebSocketSession::hDefaultMaxMessageSize,
														 compressionCfg,
														 deflateNeg.accepted ? &deflateNeg : nullptr);

			// 设置协商的子协议
			if (!negotiatedProtocol.empty())
			{
				session->setSubprotocol(std::move(negotiatedProtocol));
			}

			// WS 升级后 handleSession 的 Entry 已经注销了，得重新注册一个，
			// 不然 stop() 的 closeAll() 管不到这条连接，run() 就退不了
			IdleScanner::Entry wsIdleEntry;
			wsIdleEntry.socket = &session->socket();
			wsIdleEntry.touch();
			auto* wsScanner = currentThreadIdleScanner();
			IdleScanner::Guard wsIdleGuard(wsScanner, wsIdleEntry);

			// 使用 alive 标志防止 timer/ping 回调在 session 销毁后访问悬空引用。
			// timer 提到协程外面，会话结束时 guard 才拿得到句柄去 cancel；不开 ping 的就不建（nullptr）。
			auto wsAlive = std::make_shared<std::atomic<bool>>(true);
			std::shared_ptr<boost::asio::steady_timer> wsPingTimer;
			if (wsRoute.pingInterval.count() > 0)
			{
				wsPingTimer = std::make_shared<boost::asio::steady_timer>(session->socket().get_executor());
			}

			// RAII：确保协程退出时标记 session 已失效，并把还在 wait 的 ping timer 取消掉。
			// 只置标志不行——ping 协程多半正挂在 async_wait 上，收不到这个通知，只能等 timer 自然到期。
			struct WsAliveGuard
			{
				std::shared_ptr<std::atomic<bool>> alive;
				std::shared_ptr<boost::asio::steady_timer> timer; // 可空（未开 ping）

				~WsAliveGuard()
				{
					alive->store(false);
					if (timer)
					{
						timer->cancel();
					}
				}
			} wsAliveGuard {wsAlive, wsPingTimer};

			// 7. 启动心跳 Ping 协程（Feature 1）
			if (wsPingTimer)
			{
				auto pingInterval = wsRoute.pingInterval;
				auto maxMissed = wsRoute.maxMissedPongs;
				auto pingPayload = wsRoute.pingPayload;

				coSpawn(session->socket().get_executor(),
						[session, wsAlive, wsPingTimer, pingInterval, maxMissed, pingPayload = std::move(pingPayload)]()
							-> Awaitable<void>
						{
							while (wsAlive->load(std::memory_order_acquire) && session->isOpen())
							{
								wsPingTimer->expires_after(pingInterval);
								boost::system::error_code ec;
								co_await wsPingTimer->async_wait(
									boost::asio::redirect_error(boost::asio::use_awaitable, ec));
								if (ec || !wsAlive->load(std::memory_order_acquire))
								{
									break;
								}

								// 检查是否超过最大未响应次数
								auto elapsed = std::chrono::steady_clock::now() - session->lastPongTime();
								if (elapsed > pingInterval * maxMissed)
								{
									// 死连接，关闭 socket
									boost::system::error_code closeEc;
									session->socket().close(closeEc);
									break;
								}

								try
								{
									co_await session->sendPing(pingPayload);
								}
								catch (...)
								{
									break;
								}
							}
						});
			}

			// 调用连接回调
			if (wsRoute.onConnect)
			{
				co_await wsRoute.onConnect(*session);
			}

			// 消息循环：一批读入的数据能解出几条消息就解几条，回调跑完再统一落一次盘。
			// 以前是一帧一个 receive/write 往返，16 帧进来就是 16 次系统调用。
			auto dispatchMessage = [&wsRoute](const WsMessage& msg, WebSocketSession& session) -> Awaitable<void>
			{
				if (wsRoute.onTypedMessage)
				{
					co_await wsRoute.onTypedMessage(msg, session);
				}
				else if (wsRoute.onMessage)
				{
					co_await wsRoute.onMessage(msg.data, session);
				}
			};

			// 一批里通常也就十几条消息，先按这个数预留，省掉反复扩容
			static constexpr size_t kInitialWsBatchMessages = 16;
			std::vector<WsMessage> batch;
			batch.reserve(kInitialWsBatchMessages);

			while (session->isOpen())
			{
				bool alive = false;
				// 回调或读出错时不能就地 co_await（catch 里不许挂起），
				// 先把异常存下来，等出站缓冲刷完再抛出去
				std::exception_ptr error;

				try
				{
					// 空闲超时只覆盖「等下一批数据」这一段，回调执行期间不设防
					if (wsDeadline)
					{
						wsDeadline->expires_after(wsTimeoutDuration);
						wsDeadline->async_wait(
							[sessionWeak = std::weak_ptr(session),
							 aliveFlag = wsAlive](const boost::system::error_code& ec)
							{
								if (!ec && aliveFlag->load())
								{
									if (auto sp = sessionWeak.lock(); sp && sp->isOpen())
									{
										boost::system::error_code closeEc;
										sp->socket().close(closeEc);
									}
								}
							});
					}

					alive = co_await session->receiveBatch(batch);

					if (wsDeadline)
					{
						wsDeadline->cancel();
					}

					// 合批窗口只包住回调这一段：回调里的 send() 先入队，跑完统一落一次盘。
					// 挂起等数据那段不合批，心跳 Ping 和 WsHub 广播才能立刻发出去
					session->beginBatch();
					for (const auto& msg : batch)
					{
						co_await dispatchMessage(msg, *session);
					}
				}
				catch (...)
				{
					error = std::current_exception();
				}

				// 正常收尾还是出错收尾，攒着的响应都得刷出去
				co_await session->endBatch();

				if (error)
				{
					std::rethrow_exception(error);
				}

				if (!alive)
				{
					break;
				}
				// 只有 ping/pong 的一批解不出消息，但连接还在，回去接着读
			}
		}
		catch (const boost::system::system_error& e)
		{
			// 异常路径先取消 timer，防止 timer 回调在 session 析构后访问悬空引用
			if (wsDeadline)
			{
				wsDeadline->cancel();
			}

			if (e.code() != boost::asio::error::eof && e.code() != boost::asio::error::connection_reset
				&& e.code() != boost::asio::error::operation_aborted)
			{
				// 忽略正常关闭
			}
		}

		// 连接断开回调（正常退出和异常退出都会触发）
		if (session && wsRoute.onDisconnect)
		{
			try
			{
				co_await wsRoute.onDisconnect(*session);
			}
			catch (...)
			{
				// 忽略断开回调中的异常
			}
		}
	}

	Awaitable<void> HttpServer::handleSseSession(boost::asio::ip::tcp::socket socket, const Router::SseRoute& sseRoute)
	{
		// SSE 使用自定义超时（30 分钟）
		static constexpr int64_t kSseTimeoutMs = 30 * 60 * 1000;

		// 注册 IdleScanner entry，30 分钟超时
		IdleScanner::Entry sseIdleEntry;
		sseIdleEntry.customTimeoutMs = kSseTimeoutMs;
		sseIdleEntry.touch();
		auto* scanner = currentThreadIdleScanner();
		IdleScanner::Guard idleGuard(scanner, sseIdleEntry);

		// SSE 会话（将 socket move 进去）
		auto sseSession = std::make_shared<SseSession>(std::move(socket));
		// idle entry 引用 SseSession 内部的 socket
		sseIdleEntry.socket = &sseSession->socket();
		// 保活引用：onConnect 结束后回调的 shared_ptr 释放会销毁 SseSession，
		// sessionRef 在 guard 之后析构，保证 guard 析构时 socket 仍然存活
		auto sessionRef = sseSession;

		// socket 析构守卫，引用 SseSession 内部的 socket
		struct SseSocketGuard
		{
			boost::asio::ip::tcp::socket& sock;

			~SseSocketGuard()
			{
				if (sock.is_open())
				{
					boost::system::error_code ec;
					sock.shutdown(boost::asio::ip::tcp::socket::shutdown_send, ec);
					sock.close(ec);
				}
			}
		} guard {sseSession->socket()};

		try
		{
			// 发送响应头
			co_await sseSession->sendResponseHead();
			sseIdleEntry.touch();

			// 心跳 timer 提到协程外面，会话结束时 guard 才拿得到句柄去 cancel。
			// 另外留着 alive 标志：协程可能正挂在 sendComment 上（timer 没在 wait），
			// 这时 cancel 打不到 wait，得靠标志让它醒来后别再重挂 timer。
			auto heartbeatAlive = std::make_shared<bool>(true);
			auto heartbeatTimer = std::make_shared<boost::asio::steady_timer>(sseSession->socket().get_executor());

			coSpawn(sseSession->socket().get_executor(),
					[heartbeatAlive,
					 heartbeatTimer,
					 sessionWeak = std::weak_ptr<SseSession>(sseSession)]() -> Awaitable<void>
					{
						while (*heartbeatAlive)
						{
							heartbeatTimer->expires_after(std::chrono::seconds(30));
							boost::system::error_code ec;
							co_await heartbeatTimer->async_wait(
								boost::asio::redirect_error(boost::asio::use_awaitable, ec));
							// ec 来自 cancel，标志兜住停在 sendComment 时被结束的情况
							if (ec || !*heartbeatAlive)
							{
								break;
							}
							if (auto sp = sessionWeak.lock(); sp && sp->isOpen())
							{
								co_await sp->sendComment("heartbeat");
							}
						}
					});

			// 会话结束（正常退出或异常）时停心跳：置标志 + cancel timer，两个都不能少——
			// 只置标志，协程挂在 async_wait 上要等 timer 自然到期（30 秒）；只 cancel，会漏掉停在
			// sendComment 时那次。timer 和它的 wait 在同一个 executor，同线程调用，直接 cancel 即可
			struct HeartbeatGuard
			{
				std::shared_ptr<bool> alive;
				std::shared_ptr<boost::asio::steady_timer> timer;

				~HeartbeatGuard()
				{
					*alive = false;
					if (timer)
					{
						timer->cancel();
					}
				}
			} heartbeatGuard {heartbeatAlive, heartbeatTimer};

			// 调用 onConnect 回调
			co_await sseRoute.onConnect(std::move(sseSession));
		}
		catch (const boost::system::system_error& e)
		{
			if (e.code() != boost::asio::error::eof && e.code() != boost::asio::error::connection_reset
				&& e.code() != boost::asio::error::operation_aborted)
			{
				// 忽略正常关闭
			}
		}
	}

	// handleSession 模板定义在本编译防火墙 .cpp，用显式实例化限制实例化类型，
	// 避免每个 TU 重编模板体，也让其他 TU 能引用到这两个实例。
	template Awaitable<void> HttpServer::handleSession<tcp::socket>(tcp::socket);
	template Awaitable<void> HttpServer::handleSession<boost::asio::ssl::stream<tcp::socket>>(
		boost::asio::ssl::stream<tcp::socket>);
} // namespace hical
