/**
 * @file HttpSessionImpl.cpp
 * @brief HttpServer 的会话处理实现（编译防火墙）
 * 将 picohttpparser HTTP 解析器和 WebSocket 帧处理代码隔离在此翻译单元，
 * 修改 HttpServer 配置逻辑时不会触发重模板编译。
 */

#include "HttpServer.h"
#include "FixedBuffer.h"
#include "MemoryPool.h"
#include "core/Version.h"
#include "WebSocket.h"
#include "WsHandshake.h"
#include <charconv>
#include <chrono>
#include <ctime>
#include <fstream>
#include <optional>

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

			// 序列化头部到栈缓冲区（响应头通常 150-300 字节，512 足够）
			FixedBuffer<512> headBuf;
			nativeRes.serializeHeadTo(headBuf);

			if (skipBody || nativeRes.body.empty())
			{
				// HEAD 方法或空 body：仅发送头部
				co_await boost::asio::async_write(socket,
												  boost::asio::buffer(headBuf.data(), headBuf.size()),
												  boost::asio::use_awaitable);
			}
			else if (headBuf.size() + nativeRes.body.size() <= 512 && !headBuf.overflowed())
			{
				// 小响应（head + body <= 512）：合并到栈缓冲区，单次 write，零堆分配
				headBuf.append(nativeRes.body.data(), nativeRes.body.size());
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

		/// 发送文件体响应（先发头部，再异步分块读文件发送）
		/// 用于 Range 请求等大文件场景，避免全量加载到内存
		Awaitable<void> writeFileResponse(tcp::socket& socket, NativeResponse& nativeRes)
		{
			nativeRes.preparePayload();

			auto& fb = *nativeRes.fileBody;
			static constexpr size_t kChunkSize = 65536; // 64KB

#ifdef BOOST_ASIO_HAS_FILE
			// 先打开文件确认可访问，失败时尚未发送头部，可安全抛异常
			auto executor = co_await boost::asio::this_coro::executor;
			boost::asio::random_access_file file(executor,
												 fb.path.string(),
												 boost::asio::random_access_file::read_only);

			// 文件打开成功，发送头部
			FixedBuffer<512> headBuf;
			nativeRes.serializeHeadTo(headBuf);
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
			// 先打开文件确认可访问
			std::ifstream ifs(fb.path, std::ios::binary);
			if (!ifs)
			{
				throw boost::system::system_error(
					boost::system::errc::make_error_code(boost::system::errc::no_such_file_or_directory));
			}

			// 文件打开成功，发送头部
			FixedBuffer<512> headBuf;
			nativeRes.serializeHeadTo(headBuf);
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
		}

	} // namespace

	/// 空闲超时协程：循环检查时间戳，超时就关 socket
	static Awaitable<void> idleTimerLoop(std::shared_ptr<boost::asio::steady_timer> pTimer,
										 tcp::socket& socket,
										 std::shared_ptr<std::atomic<bool>> alive,
										 std::shared_ptr<std::atomic<int64_t>> lastActive,
										 int64_t timeoutMs)
	{
		auto& timer = *pTimer;
		while (alive->load())
		{
			timer.expires_after(std::chrono::milliseconds(timeoutMs));
			boost::system::error_code ec;
			co_await timer.async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, ec));
			if (ec || !alive->load())
			{
				break;
			}

			auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
						   std::chrono::steady_clock::now().time_since_epoch())
						   .count();
			auto elapsed = now - lastActive->load(std::memory_order_relaxed);

			if (elapsed >= timeoutMs)
			{
				// 真正超时：关闭 socket 中断 async_read
				boost::asio::dispatch(socket.get_executor(),
									  [&socket, alive]()
									  {
										  if (alive->load())
										  {
											  boost::system::error_code closeEc;
											  socket.close(closeEc);
										  }
									  });
				break;
			}
			// 还没超时，继续等
		}
	}

	Awaitable<void> HttpServer::handleSession(tcp::socket socket)
	{
		// 连接计数 RAII 守卫
		activeConnections_.fetch_add(1);

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
					server.stopAllLoops();
				}
			}
		} connCounter {activeConnections_, draining_, *this};

		// socket 析构守卫，WS 升级时 transferred=true 就跳过
		struct SocketGuard
		{
			tcp::socket& sock;
			bool transferred {false};

			~SocketGuard()
			{
				if (!transferred && sock.is_open())
				{
					boost::system::error_code ec;
					sock.shutdown(tcp::socket::shutdown_send, ec);
					sock.close(ec);
				}
			}
		} guard {socket};

		// timer 放 try 外面，catch 里也要能 cancel 它
		std::shared_ptr<boost::asio::steady_timer> deadline;
		if (idleTimeout_ > 0)
		{
			deadline = std::make_shared<boost::asio::steady_timer>(socket.get_executor());
		}

		try
		{
			// alive 标志，socket 没了之后 timer 回调别再碰
			auto socketAlive = std::make_shared<std::atomic<bool>>(true);

			// 协程退出时 alive=false
			struct AliveGuard
			{
				std::shared_ptr<std::atomic<bool>> alive;

				~AliveGuard()
				{
					alive->store(false);
				}
			} aliveGuard {socketAlive};

			// 连接级活跃时间戳，不用每请求 reset timer 了
			auto lastActiveMs =
				std::make_shared<std::atomic<int64_t>>(std::chrono::duration_cast<std::chrono::milliseconds>(
														   std::chrono::steady_clock::now().time_since_epoch())
														   .count());
			auto timeoutMs = static_cast<int64_t>(idleTimeout_ * 1000);

			// 超时协程，一个 loop 搞定，不用回调链
			if (deadline)
			{
				boost::asio::co_spawn(socket.get_executor(),
									  idleTimerLoop(deadline, socket, socketAlive, lastActiveMs, timeoutMs),
									  boost::asio::detached);
			}

			// 读缓冲区，keep-alive 复用
			std::string readBuf;
			readBuf.resize(8192);
			size_t bufUsed = 0; // 跨请求保留：TCP 粘包时残留下一请求的数据

			for (;;)
			{
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

				// 读完头部后更新活跃时间戳
				lastActiveMs->store(std::chrono::duration_cast<std::chrono::milliseconds>(
										std::chrono::steady_clock::now().time_since_epoch())
										.count(),
									std::memory_order_relaxed);

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

				nativeReq.headers.clear();
				for (size_t i = 0; i < numHeaders; ++i)
				{
					std::string_view hname(headers[i].name, headers[i].name_len);
					std::string_view hvalue(headers[i].value, headers[i].value_len);

					nativeReq.headers.add(hname, hvalue);

					// 按长度+首字符快速过滤，将 20 次 iequals 降到 ~2 次
					// Content-Length: 长度 14，首字符 C/c
					// Transfer-Encoding: 长度 17，首字符 T/t
					if (hname.size() == 14 && (hname[0] == 'C' || hname[0] == 'c'))
					{
						if (HeaderMap::iequals(hname, "Content-Length"))
						{
							auto [ptr, ec] =
								std::from_chars(hvalue.data(), hvalue.data() + hvalue.size(), contentLength);
							hasContentLength = (ec == std::errc {});
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

				// 计算 keep-alive
				auto connHeader = nativeReq.headers.find("Connection");
				if (!connHeader.empty())
				{
					nativeReq.keepAlive = !HeaderMap::iequals(connHeader, "close");
				}
				else
				{
					// HTTP/1.1 默认 keep-alive，HTTP/1.0 默认 close
					nativeReq.keepAlive = (minorVersion >= 1);
				}

				// ====== 阶段 C：读取 Body ======
				size_t headerBytes = static_cast<size_t>(parseResult);
				size_t remainingInBuf = bufUsed - headerBytes;

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
								forbiddenRes.setBody("403 Forbidden: Origin not allowed");
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

						// socket 所有权转移给 WebSocket 会话，标记 guard 跳过析构
						guard.transferred = true;
						co_await handleWebSocket(std::move(socket), req.native(), wsRoute);
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

				// 设置通用头部（insert 替代 set：用户 handler 不会预设 Server/Connection，
				// 直接 push_back O(1)，省去线性扫描）
				auto& nativeRes = res.native();
				nativeRes.httpVersionMinor = 1;
				nativeRes.headers.insert("Server", HICAL_VERSION_STRING);
				bool shouldKeepAlive = req.native().keepAlive && !draining_.load();
				nativeRes.keepAlive = shouldKeepAlive;
				nativeRes.headers.insert("Connection", shouldKeepAlive ? "keep-alive" : "close");
				nativeRes.headers.insert("Date", cachedHttpDate());

				// 发送响应（scatter-gather I/O：状态行+头部在栈，body 零拷贝）
				// HEAD 方法：仅发送头部，不发送 body（RFC 7231 §4.3.2）
				bool isHead = (req.method() == HttpMethod::hHead);
				if (nativeRes.hasFileBody() && !isHead)
				{
					co_await writeFileResponse(socket, nativeRes);
				}
				else
				{
					co_await writeResponse(socket, nativeRes, isHead);
				}

				// 写完后更新活跃时间戳
				lastActiveMs->store(std::chrono::duration_cast<std::chrono::milliseconds>(
										std::chrono::steady_clock::now().time_since_epoch())
										.count(),
									std::memory_order_relaxed);

				// 延迟 memmove：响应已发送或已暂存，nativeReq.target/headers 不再被引用，
				// 安全地将残留数据移到缓冲区开头（为下一个 pipelined 请求做准备）
				if (memmoveLen > 0)
				{
					std::memmove(readBuf.data(), readBuf.data() + memmoveSrc, memmoveLen);
				}

				if (!shouldKeepAlive)
				{
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

		// 取消 idleTimerLoop 协程：cancel timer 使其 co_await 收到 operation_aborted 并退出
		if (deadline)
		{
			deadline->cancel();
		}
		// AliveGuard 析构设 alive=false → SocketGuard 析构关闭 socket
	}

	Awaitable<void> HttpServer::handleWebSocket(tcp::socket socket,
												const NativeRequest& req,
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
			// 防御性复制关键头部值（NativeRequest 的 string_view 引用 handleSession 的 readBuf，
			// 虽然此路径下 readBuf 生命周期足够，但防御性拷贝更安全）
			std::string clientKeyStr(req.headers.find("Sec-WebSocket-Key"));
			std::string extHeaderStr(req.headers.find("Sec-WebSocket-Extensions"));
			std::string protoHeaderStr(req.headers.find("Sec-WebSocket-Protocol"));

			// 1. 验证 WS 握手头部
			if (validateWsUpgrade(req).empty())
			{
				co_return;
			}

			// 2. 计算 Sec-WebSocket-Accept
			auto acceptKey = computeWsAcceptKey(clientKeyStr);

			// 3. 协商 permessage-deflate
			WsDeflateNegotiation deflateNeg;
			WsCompressionConfig compressionCfg;
			compressionCfg.enabled = wsRoute.enableCompression;
			compressionCfg.serverMaxWindowBits = wsRoute.serverMaxWindowBits;
			compressionCfg.clientMaxWindowBits = wsRoute.clientMaxWindowBits;
			compressionCfg.serverNoContextTakeover = wsRoute.serverNoContextTakeover;

			if (wsRoute.enableCompression && !extHeaderStr.empty())
			{
				deflateNeg = negotiateDeflate(extHeaderStr, compressionCfg);
			}

			// 4. 协商子协议（Feature 5）
			std::string negotiatedProtocol;
			if (!wsRoute.subprotocols.empty() && !protoHeaderStr.empty())
			{
				negotiatedProtocol = negotiateSubprotocol(protoHeaderStr, wsRoute.subprotocols);
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

} // namespace hical
