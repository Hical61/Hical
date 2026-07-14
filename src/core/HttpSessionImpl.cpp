/**
 * @file HttpSessionImpl.cpp
 * @brief HTTP 会话处理（编译防火墙）
 * picohttpparser 和 WebSocket 帧处理代码集中在这里，
 * 改 HttpServer 的配置逻辑不会触发这些重模板代码的重编。
 */

#include "HttpServer.h"
#include "FixedBuffer.h"
#include "MemoryPool.h"
#include "ReadBufferPool.h"
#include "SseSession.h"
#include "core/Version.h"
#include "WebSocket.h"
#include "WsHandshake.h"
#include <charconv>
#include <chrono>
#include <cstring>
#include <ctime>
#include <fstream>
#include <optional>

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
		struct TcpCorkGuard
		{
			tcp::socket& sock_;
			bool corked_ {false};

			explicit TcpCorkGuard(tcp::socket& s) : sock_(s)
			{
#if defined(__linux__)
				int flag = 1;
				if (::setsockopt(sock_.native_handle(), IPPROTO_TCP, TCP_CORK, &flag, sizeof(flag)) == 0)
				{
					corked_ = true;
				}
#elif defined(__APPLE__)
				int flag = 1;
				if (::setsockopt(sock_.native_handle(), IPPROTO_TCP, TCP_NOPUSH, &flag, sizeof(flag)) == 0)
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
					::setsockopt(sock_.native_handle(), IPPROTO_TCP, TCP_CORK, &flag, sizeof(flag));
#elif defined(__APPLE__)
					int flag = 0;
					::setsockopt(sock_.native_handle(), IPPROTO_TCP, TCP_NOPUSH, &flag, sizeof(flag));
#endif
				}
			}

			TcpCorkGuard(const TcpCorkGuard&) = delete;
			TcpCorkGuard& operator=(const TcpCorkGuard&) = delete;
		};

		/// 乐观同步写辅助：socket 在非阻塞模式下先试一把 write_some，
		/// 写完了直接返回 true（不挂协程、不进 reactor 完成队列），
		/// would_block / partial / 异常都返回 false，调用方回退 async_write。
		bool tryOptimisticWrite(tcp::socket& socket, const boost::asio::const_buffer& buf)
		{
			boost::system::error_code ec;
			size_t written = socket.write_some(buf, ec);
			// 完整写完且没出错——最佳情况，零完成队列开销
			if (!ec && written == buf.size())
			{
				return true;
			}
			// ec 为 would_block 说明内核发送缓冲区满了，或者只写了部分数据，
			// 都交回 async_write 处理。
			// 等 async_write 继续——虽然 async_write 会重写整个 buffer，但
			// write_some 已写入的数据不会在线上重复，TCP 流式语义兜底。
			return false;
		}

		/// 快速发送错误响应（栈缓冲区，零堆分配）
		Awaitable<void> sendRawResponse(tcp::socket& socket,
										unsigned statusCode,
										std::string_view reason,
										std::string_view body)
		{
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
			if (tryOptimisticWrite(socket, boost::asio::buffer(buf.data(), buf.size())))
			{
				co_return;
			}

			boost::system::error_code writeEc;
			co_await boost::asio::async_write(socket,
											  boost::asio::buffer(buf.data(), buf.size()),
											  boost::asio::redirect_error(boost::asio::use_awaitable, writeEc));
		}

		/// 发送 HttpResponse 对象（内部辅助）
		/// @param skipBody 为 true 时仅发送头部（HEAD 方法响应）
		Awaitable<void> writeResponse(tcp::socket& socket, NativeResponse& nativeRes, bool skipBody = false)
		{
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
						if (tryOptimisticWrite(socket, boost::asio::buffer(headBuf.data(), headBuf.size())))
						{
							co_return;
						}

						co_await boost::asio::async_write(socket,
														  boost::asio::buffer(headBuf.data(), headBuf.size()),
														  boost::asio::use_awaitable);
					}
				}
				else
				{
					if (tryOptimisticWrite(socket, boost::asio::buffer(headBuf.data(), headBuf.size())))
					{
						co_return;
					}

					co_await boost::asio::async_write(socket,
													  boost::asio::buffer(headBuf.data(), headBuf.size()),
													  boost::asio::use_awaitable);
				}
				co_return;
			}

			// 序列化头部到栈缓冲区（响应头通常 150-300 字节，512 足够）
			FixedBuffer<512> headBuf;
			nativeRes.serializeHeadTo(headBuf);

			if (skipBody || nativeRes.body.empty())
			{
				// HEAD 方法或空 body：仅发送头部
				if (tryOptimisticWrite(socket, boost::asio::buffer(headBuf.data(), headBuf.size())))
				{
					co_return;
				}

				co_await boost::asio::async_write(socket,
												  boost::asio::buffer(headBuf.data(), headBuf.size()),
												  boost::asio::use_awaitable);
			}
			else if (headBuf.size() + nativeRes.body.size() <= 512 && !headBuf.overflowed())
			{
				// 小响应（head + body <= 512）：合并到栈缓冲区，单次 write，零堆分配
				headBuf.append(nativeRes.body.data(), nativeRes.body.size());
				if (tryOptimisticWrite(socket, boost::asio::buffer(headBuf.data(), headBuf.size())))
				{
					co_return;
				}

				co_await boost::asio::async_write(socket,
												  boost::asio::buffer(headBuf.data(), headBuf.size()),
												  boost::asio::use_awaitable);
			}
			else
			{
				// 大响应：scatter-gather I/O，head 在栈上 + body 零拷贝引用
				// 一次 writev 系统调用，消除 body 的额外 memcpy
				std::array<boost::asio::const_buffer, 2> bufs = {boost::asio::buffer(headBuf.data(), headBuf.size()),
																 boost::asio::buffer(nativeRes.body)};
				co_await boost::asio::async_write(socket, bufs, boost::asio::use_awaitable);
			}
		}

		/// writeResponse 带前缀版本——通用头部（Server/Connection/Date）已预拼好，
		/// 直接 memcpy 进去，省掉 3 次 HeaderMap::insert + 序列化循环
		Awaitable<void> writeResponse(tcp::socket& socket,
									  NativeResponse& nativeRes,
									  const char* prefix,
									  size_t prefixLen,
									  bool skipBody = false)
		{
			nativeRes.preparePayload();

			// ChunkedBody 路径
			if (nativeRes.hasChunkedBody())
			{
				FixedBuffer<512> headBuf;
				nativeRes.serializeHeadTo(headBuf, prefix, prefixLen);

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
						if (tryOptimisticWrite(socket, boost::asio::buffer(headBuf.data(), headBuf.size())))
						{
							co_return;
						}

						co_await boost::asio::async_write(socket,
														  boost::asio::buffer(headBuf.data(), headBuf.size()),
														  boost::asio::use_awaitable);
					}
				}
				else
				{
					if (tryOptimisticWrite(socket, boost::asio::buffer(headBuf.data(), headBuf.size())))
					{
						co_return;
					}

					co_await boost::asio::async_write(socket,
													  boost::asio::buffer(headBuf.data(), headBuf.size()),
													  boost::asio::use_awaitable);
				}
				co_return;
			}

			FixedBuffer<512> headBuf;
			nativeRes.serializeHeadTo(headBuf, prefix, prefixLen);

			if (skipBody || nativeRes.body.empty())
			{
				if (tryOptimisticWrite(socket, boost::asio::buffer(headBuf.data(), headBuf.size())))
				{
					co_return;
				}

				co_await boost::asio::async_write(socket,
												  boost::asio::buffer(headBuf.data(), headBuf.size()),
												  boost::asio::use_awaitable);
			}
			else if (headBuf.size() + nativeRes.body.size() <= 512 && !headBuf.overflowed())
			{
				headBuf.append(nativeRes.body.data(), nativeRes.body.size());
				if (tryOptimisticWrite(socket, boost::asio::buffer(headBuf.data(), headBuf.size())))
				{
					co_return;
				}

				co_await boost::asio::async_write(socket,
												  boost::asio::buffer(headBuf.data(), headBuf.size()),
												  boost::asio::use_awaitable);
			}
			else
			{
				std::array<boost::asio::const_buffer, 2> bufs = {boost::asio::buffer(headBuf.data(), headBuf.size()),
																 boost::asio::buffer(nativeRes.body)};
				co_await boost::asio::async_write(socket, bufs, boost::asio::use_awaitable);
			}
		}

		/// 发送文件体响应（先发头部，再异步分块读文件发送）
		/// 用于 Range 请求等大文件场景，避免全量加载到内存。
		/// prefix 非空时用预构建前缀序列化头部，nullptr 走原始路径。
		Awaitable<void> writeFileResponse(tcp::socket& socket,
										  NativeResponse& nativeRes,
										  const char* prefix = nullptr,
										  size_t prefixLen = 0)
		{
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
			if (prefix)
			{
				nativeRes.serializeHeadTo(headBuf, prefix, prefixLen);
			}
			else
			{
				nativeRes.serializeHeadTo(headBuf);
			}
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
			if (prefix)
			{
				nativeRes.serializeHeadTo(headBuf, prefix, prefixLen);
			}
			else
			{
				nativeRes.serializeHeadTo(headBuf);
			}
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

	} // namespace

	Awaitable<void> HttpServer::handleSession(tcp::socket socket)
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
		struct SocketGuard
		{
			tcp::socket& sock;
			bool transferred {false};
			bool cleanExit {false}; // 正常结束才 shutdown，对端早断了就别白调了

			~SocketGuard()
			{
				if (!transferred && sock.is_open())
				{
					boost::system::error_code ec;
					if (cleanExit)
					{
						sock.shutdown(tcp::socket::shutdown_send, ec);
					}
					sock.close(ec);
				}
			}
		} guard {socket};

		// 显式设为非阻塞——tryOptimisticWrite 中的 write_some 在阻塞 fd
		// 上遇到 EAGAIN 会进 poll() 忙等、卡死 io 线程。Asio 异步操作
		// 期间设置的 O_NONBLOCK 是实现细节，不能依赖它保持生效。
		socket.non_blocking(true);

		// entry 在协程栈上，Guard 析构时自动注销
		// 声明在 SocketGuard 后面 → 先析构（先 unregister 再关 socket）
		IdleScanner::Entry idleEntry;
		idleEntry.socket = &socket;
		idleEntry.touch();

		IdleScanner::Guard idleGuard(currentThreadIdleScanner(), idleEntry);

		try
		{
			// 跨请求的粘包残留；大多数连接空着不额外占内存
			std::string pipelineSpill;

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
							co_await sendRawResponse(socket, 400, "Bad Request", "Malformed HTTP request");
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
							co_await sendRawResponse(socket,
													 431,
													 "Request Header Fields Too Large",
													 "Request header too large");
							co_return;
						}
						readBuf.resize(readBuf.size() * 2);
					}

					auto bytesRead = co_await socket.async_read_some(
						boost::asio::buffer(readBuf.data() + bufUsed, readBuf.size() - bufUsed),
						boost::asio::use_awaitable);
					bufUsed += bytesRead;

					// 头部大小检查
					if (bufUsed > maxHeaderSize_)
					{
						co_await sendRawResponse(socket,
												 431,
												 "Request Header Fields Too Large",
												 "Request header too large");
						co_return;
					}

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
						break; // 头部解析完成
					}
					if (parseResult == -1)
					{
						// 解析错误
						co_await sendRawResponse(socket, 400, "Bad Request", "Malformed HTTP request");
						co_return;
					}
					// parseResult == -2：数据不完整，继续读取
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
				bool connectionClose = false;     // 逮到 Connection: close 就记下来
				bool hasConnectionHeader = false; // 有 Connection 头就把这位置 1

				nativeReq.headers.clear();
				for (size_t i = 0; i < numHeaders; ++i)
				{
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
							auto [ptr, ec] =
								std::from_chars(hvalue.data(), hvalue.data() + hvalue.size(), contentLength);
							hasContentLength = (ec == std::errc {});
						}
					}
					else if (hname.size() == 10 && (hname[0] == 'C' || hname[0] == 'c'))
					{
						if (HeaderMap::iequals(hname, "Connection"))
						{
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
							hasTransferEncoding = true;
							// RFC 7230 ：chunked 必须是最后一个编码
							auto lastComma = hvalue.rfind(',');
							std::string_view lastToken =
								(lastComma != std::string_view::npos) ? hvalue.substr(lastComma + 1) : hvalue;
							while (!lastToken.empty() && lastToken.front() == ' ')
							{
								lastToken.remove_prefix(1);
							}
							while (!lastToken.empty() && lastToken.back() == ' ')
							{
								lastToken.remove_suffix(1);
							}
							if (HeaderMap::iequals(lastToken, "chunked"))
							{
								isChunked = true;
							}
						}
					}
				}

				// RFC 7230 ：不认识的 Transfer-Encoding 返回 501
				if (hasTransferEncoding && !isChunked)
				{
					co_await sendRawResponse(socket, 501, "Not Implemented", "Unsupported Transfer-Encoding");
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

				// ====== 阶段 C：读取 Body ======
				size_t headerBytes = static_cast<size_t>(parseResult);
				size_t remainingInBuf = bufUsed - headerBytes;

				// Expect: 100-continue 处理（仅 HTTP/1.1+，有 body 时才生效）
				// 只查路由存在性，404/413 提前拒绝；通过后发 100 Continue 再读 body。
				// 方法不匹配（405 语义）在此一律以 404 快速拒绝——预检目的是省带宽，
				// 不值得为收集 Allow 头做完整 resolveRoute；真正的 405 在客户端不发 Expect
				// 时由正常分发路径给出。
				if (nativeReq.expectContinue && (hasContentLength || isChunked) && minorVersion >= 1)
				{
					auto reqPath = nativeReq.target;
					auto qmark = reqPath.find('?');
					if (qmark != std::string_view::npos)
					{
						reqPath = reqPath.substr(0, qmark);
					}

					if (!router_.exists(nativeReq.method, reqPath))
					{
						co_await sendRawResponse(socket, 404, "Not Found", "Not Found");
						co_return;
					}

					// Content-Length 已知时，超限在发 100 前就拒——这才是 Expect 省带宽的意义所在。
					// chunked 长度未知，只能边读边查（维持现状）。
					if (hasContentLength && contentLength > maxBodySize_)
					{
						co_await sendRawResponse(socket, 413, "Payload Too Large", "Request body too large");
						co_return;
					}

					constexpr std::string_view k100 = "HTTP/1.1 100 Continue\r\n\r\n";
					co_await boost::asio::async_write(socket,
													  boost::asio::buffer(k100.data(), k100.size()),
													  boost::asio::use_awaitable);
				}

				// memmove 要等阶段 D 用完 string_view 后再做，否则覆盖了 readBuf 的头部数据
				size_t memmoveSrc = 0; // memmove 源偏移
				size_t memmoveLen = 0; // memmove 长度（0 表示无需 memmove）

				if (hasContentLength && contentLength > 0)
				{
					// Content-Length body 读取
					if (contentLength > maxBodySize_)
					{
						co_await sendRawResponse(socket, 413, "Payload Too Large", "Request body too large");
						co_return;
					}

					nativeReq.body.resize(contentLength);
					size_t bodyCopied = std::min(remainingInBuf, contentLength);
					if (bodyCopied > 0)
					{
						std::memcpy(nativeReq.body.data(), readBuf.data() + headerBytes, bodyCopied);
					}

					// 把 body 后面的残留数据位置记下来，后面再 memmove
					size_t tailLen = remainingInBuf - bodyCopied;
					if (tailLen > 0)
					{
						memmoveSrc = headerBytes + bodyCopied;
						memmoveLen = tailLen;
					}
					bufUsed = tailLen;

					size_t bodyRemaining = contentLength - bodyCopied;
					size_t offset = bodyCopied;
					while (bodyRemaining > 0)
					{
						auto bytesRead = co_await socket.async_read_some(
							boost::asio::buffer(nativeReq.body.data() + offset, bodyRemaining),
							boost::asio::use_awaitable);
						offset += bytesRead;
						bodyRemaining -= bytesRead;
					}
				}
				else if (isChunked)
				{
					// Chunked transfer-encoding 解码
					// phr_decode_chunked 是原地解码：将编码帧头剥离，解码数据覆写到同一缓冲区
					// 返回值：>= 0 表示完成（值为尾部长度），-2 表示需要更多数据，-1 表示错误
					// decodeBufLen 输入为待解码字节数，输出为本次解码产出的字节数
					std::string chunkBuf;
					if (remainingInBuf > 0)
					{
						chunkBuf.assign(readBuf.data() + headerBytes, remainingInBuf);
					}
					// chunked 路径消费了 readBuf 所有残留数据，清零
					bufUsed = 0;

					struct phr_chunked_decoder decoder = {};
					// encodeStart: 下一次 decode 的起始偏移
					// 解码产出的数据在 chunkBuf[encodeStart .. encodeStart+decodeBufLen)
					size_t encodeStart = 0;

					for (;;)
					{
						size_t available = chunkBuf.size() - encodeStart;
						if (available == 0)
						{
							// 缓冲区无数据，读取更多
							size_t oldSize = chunkBuf.size();
							chunkBuf.resize(oldSize + 4096);
							auto bytesRead =
								co_await socket.async_read_some(boost::asio::buffer(chunkBuf.data() + oldSize, 4096),
																boost::asio::use_awaitable);
							chunkBuf.resize(oldSize + bytesRead);
							continue;
						}

						size_t decodeBufLen = available;
						auto decodeRet = phr_decode_chunked(&decoder, chunkBuf.data() + encodeStart, &decodeBufLen);

						// decodeBufLen = 本次解码产出字节数，数据位于 chunkBuf[encodeStart..]
						nativeReq.body.append(chunkBuf.data() + encodeStart, decodeBufLen);
						encodeStart += decodeBufLen;

						if (nativeReq.body.size() > maxBodySize_)
						{
							co_await sendRawResponse(socket, 413, "Payload Too Large", "Request body too large");
							co_return;
						}

						if (decodeRet >= 0)
						{
							// 解码完成
							break;
						}
						if (decodeRet == -1)
						{
							// 解码错误
							co_await sendRawResponse(socket, 400, "Bad Request", "Malformed chunked encoding");
							co_return;
						}
						// decodeRet == -2：需要更多数据
						size_t oldSize = chunkBuf.size();
						chunkBuf.resize(oldSize + 4096);
						auto bytesRead =
							co_await socket.async_read_some(boost::asio::buffer(chunkBuf.data() + oldSize, 4096),
															boost::asio::use_awaitable);
						chunkBuf.resize(oldSize + bytesRead);
					}
				}
				else
				{
					// 无 body（GET/HEAD 等）：记录延迟 memmove 参数
					if (remainingInBuf > 0)
					{
						memmoveSrc = headerBytes;
						memmoveLen = remainingInBuf;
					}
					bufUsed = remainingInBuf;
				}

				// ====== 阶段 D：构建 HttpRequest 并分发 ======
				HttpRequest req = HttpRequest::fromParsed(std::move(nativeReq));

				// 检查 WebSocket 升级请求
				if (req.native().isUpgrade())
				{
					auto reqPath = req.path();

					auto wsMatch = router_.findWsRoute(reqPath);
					if (wsMatch.route)
					{
						const auto& wsRoute = *wsMatch.route;

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
								co_await writeResponse(socket, nativeRes);
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
								co_await writeResponse(socket, nativeRes);
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
							co_await writeResponse(socket, nativeRes);
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

				// 检查 SSE 路由（GET 请求匹配 SSE 路由时转换为 SSE 长连接）
				if (req.method() == HttpMethod::hGet)
				{
					auto reqPath = req.path();
					auto sseMatch = router_.findSseRoute(reqPath);
					if (sseMatch.route)
					{
						const auto& sseRoute = *sseMatch.route;

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
								co_await writeResponse(socket, nativeRes);
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

				// 通过中间件管道 + 路由器分发（带全局错误处理）
				HttpResponse res;
				try
				{
					if (middlewarePipeline_.size() > 0)
					{
						// 中间件链已经在 start() 里 build 好了
						res = co_await middlewarePipeline_.execute(req);
					}
					else
					{
						// 同步路由直接调就完了，不走协程
						auto syncResult = router_.dispatchSync(req);
						if (syncResult)
						{
							res = std::move(*syncResult);
						}
						else
						{
							res = co_await router_.dispatch(req);
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

				// 通用头部走预构建前缀，不再 insert 到 HeaderMap
				auto& nativeRes = res.native();
				nativeRes.httpVersionMinor = 1;
				bool shouldKeepAlive = req.native().keepAlive && !draining_.load();
				nativeRes.keepAlive = shouldKeepAlive;

				// 发送响应（scatter-gather I/O：状态行+头部在栈，body 零拷贝）
				// HEAD 方法：仅发送头部，不发送 body（RFC 7231 §4.3.2）
				bool isHead = (req.method() == HttpMethod::hHead);

				if (!shouldKeepAlive)
				{
					// 连接即将关闭——频率极低，走原始路径就好
					nativeRes.headers.insert("Server", HICAL_VERSION_STRING);
					nativeRes.headers.insert("Connection", "close");
					nativeRes.headers.insert("Date", cachedHttpDate());
					if (nativeRes.hasFileBody() && !isHead)
					{
						co_await writeFileResponse(socket, nativeRes);
					}
					else
					{
						co_await writeResponse(socket, nativeRes, isHead);
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

					if (nativeRes.hasFileBody() && !isHead)
					{
						co_await writeFileResponse(socket, nativeRes, responsePrefix, prefixLen);
					}
					else
					{
						co_await writeResponse(socket, nativeRes, responsePrefix, prefixLen, isHead);
					}
				}

				// 写完，刷新活跃时间
				idleEntry.touch();

				// 延迟 memmove：响应已发送或已暂存，nativeReq.target/headers 不再被引用，
				// 安全地将残留数据移到缓冲区开头（为下一个 pipelined 请求做准备）
				if (memmoveLen > 0)
				{
					std::memmove(readBuf.data(), readBuf.data() + memmoveSrc, memmoveLen);
				}

				// pipeline 残留已暂存，响应已发送，nativeReq.target/headers
				// 的 string_view 不再被引用，提前把缓冲区还回去。
				// 别等到协程末尾析构——万连接下能省几十 MB 在途内存。
				if (bufUsed > 0)
				{
					pipelineSpill.assign(readBuf.data(), bufUsed);
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

			// 使用 alive 标志防止 timer/ping 回调在 session 销毁后访问悬空引用
			auto wsAlive = std::make_shared<std::atomic<bool>>(true);

			// RAII：确保协程退出时标记 session 已失效
			struct WsAliveGuard
			{
				std::shared_ptr<std::atomic<bool>> alive;

				~WsAliveGuard()
				{
					alive->store(false);
				}
			} wsAliveGuard {wsAlive};

			// 7. 启动心跳 Ping 协程（Feature 1）
			if (wsRoute.pingInterval.count() > 0)
			{
				auto pingInterval = wsRoute.pingInterval;
				auto maxMissed = wsRoute.maxMissedPongs;
				auto pingPayload = wsRoute.pingPayload;

				coSpawn(session->socket().get_executor(),
						[session, wsAlive, pingInterval, maxMissed, pingPayload = std::move(pingPayload)]()
							-> Awaitable<void>
						{
							auto executor = co_await boost::asio::this_coro::executor;
							boost::asio::steady_timer timer(executor);

							while (wsAlive->load(std::memory_order_acquire) && session->isOpen())
							{
								timer.expires_after(pingInterval);
								boost::system::error_code ec;
								co_await timer.async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, ec));
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

			// 消息循环（根据是否有 onTypedMessage 选择接收方式）
			if (wsRoute.onTypedMessage)
			{
				// 类型感知消息循环（区分 Text/Binary）
				while (session->isOpen())
				{
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

					auto msg = co_await session->receiveMessage();

					if (wsDeadline)
					{
						wsDeadline->cancel();
					}

					if (!msg.has_value())
					{
						break;
					}

					co_await wsRoute.onTypedMessage(*msg, *session);
				}
			}
			else
			{
				// 原有文本消息循环（向后兼容）
				while (session->isOpen())
				{
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

					auto msg = co_await session->receive();

					if (wsDeadline)
					{
						wsDeadline->cancel();
					}

					if (!msg.has_value())
					{
						break;
					}

					if (wsRoute.onMessage)
					{
						co_await wsRoute.onMessage(*msg, *session);
					}
				}
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

			// 心跳保活，每 30 秒一条注释
			auto alive = std::make_shared<bool>(true);

			struct AliveGuard
			{
				std::shared_ptr<bool> a;

				~AliveGuard()
				{
					*a = false;
				}
			} aliveGuard {alive};

			coSpawn(sseSession->socket().get_executor(),
					[alive, sessionWeak = std::weak_ptr<SseSession>(sseSession)]() -> Awaitable<void>
					{
						auto executor = co_await boost::asio::this_coro::executor;
						boost::asio::steady_timer timer(executor);
						while (*alive)
						{
							timer.expires_after(std::chrono::seconds(30));
							boost::system::error_code ec;
							co_await timer.async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, ec));
							if (ec || !*alive)
							{
								break;
							}
							if (auto sp = sessionWeak.lock(); sp && sp->isOpen())
							{
								co_await sp->sendComment("heartbeat");
							}
						}
					});

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
} // namespace hical
