/**
 * @file HttpClient.cpp
 * @brief 协程 HTTP 客户端实现：建连、请求序列化、解析响应头、读 body
 * 响应 body 按头部选路：Transfer-Encoding: chunked 走原地解码，Content-Length 读满
 * 指定字节，两者都没有就靠连接关闭（EOF）收尾。body 读干净后连接才允许复用。
 * 每段 I/O 都套了 cancel_after，超过 connectTimeout / requestTimeout 直接取消，
 * 免得对端接上就不吭声时把请求永久吊住。
 * 明文（tcp::socket）和 TLS（ssl::stream<tcp::socket>）共用 requestOn() 这一份实现，
 * 建连 / 握手 / 关闭的差异靠 if constexpr 在编译期分掉，请求序列化、读循环、body
 * 读取不分叉。选哪条由 tlsContext 是否为空决定。
 */

#include "HttpClient.h"
#include <boost/asio/cancel_after.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/write.hpp>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509_vfy.h>
#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstring>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

// picohttpparser（C 库）。服务端用的是同族的 phr_parse_request
extern "C"
{
#include "picohttpparser.h"
}

namespace hical
{

	namespace
	{

		/// 解析响应头预留的槽位，跟服务端保持一致
		constexpr std::size_t kMaxResponseHeaders = 64;

		/// HTTP 默认端口：Host 头里是默认端口就不用回填
		constexpr uint16_t kDefaultHttpPort = 80;

		/// 读无长度信息的 body（Content-Length / EOF 路径）时每次用的栈缓冲大小
		constexpr std::size_t kBodyReadChunk = 16 * 1024;

		/// 这几个头由客户端自己补齐，用户要是也给了就跳过，免得发出去两份
		bool isManagedRequestHeader(std::string_view name) noexcept
		{
			return HeaderMap::iequals(name, "Host") || HeaderMap::iequals(name, "Content-Length")
				   || HeaderMap::iequals(name, "Connection");
		}

		/**
		 * @brief 判断头部值里是否含某个 token（大小写不敏感，逗号分隔的列表也能命中）
		 * 例如 "gzip, chunked" 里找 chunked、"keep-alive, Upgrade" 里找 close。
		 * @param value 头部值（可能为空）
		 * @param token 要找的 token
		 * @return true 表示列表里存在这个 token
		 */
		bool headerHasToken(std::string_view value, std::string_view token) noexcept
		{
			std::size_t pos = 0;
			while (pos < value.size())
			{
				// 跳过逗号和空白，切出一个 token 段（前后空格裁掉）
				while (pos < value.size() && (value[pos] == ',' || value[pos] == ' ' || value[pos] == '\t'))
				{
					++pos;
				}
				std::size_t start = pos;
				while (pos < value.size() && value[pos] != ',')
				{
					++pos;
				}
				std::size_t end = pos;
				while (end > start && (value[end - 1] == ' ' || value[end - 1] == '\t'))
				{
					--end;
				}
				if (HeaderMap::iequals(value.substr(start, end - start), token))
				{
					return true;
				}
			}
			return false;
		}

		/**
		 * @brief 解析 Content-Length 头的数值
		 * @param value 头部值（可能为空）
		 * @param out 解析结果
		 * @return true 表示解析出合法数字
		 */
		bool parseContentLength(std::string_view value, std::size_t& out) noexcept
		{
			if (value.empty())
			{
				return false;
			}
			std::size_t parsed = 0;
			auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), parsed);
			if (ec != std::errc {} || ptr == value.data())
			{
				return false;
			}
			out = parsed;
			return true;
		}

		/// 关掉某档超时时用的兜底期限。设一年，跑不完也轮不到它触发
		constexpr std::chrono::steady_clock::duration kDisabledTimeout = std::chrono::hours {24 * 365};

		/**
		 * @brief 把配置里的超时时长落成一个绝对截止点
		 * @param timeout 配置的时长；非正值当作不设超时
		 * @return 截止时刻
		 */
		std::chrono::steady_clock::time_point makeDeadline(std::chrono::steady_clock::duration timeout) noexcept
		{
			using clock = std::chrono::steady_clock;
			return clock::now() + (timeout > clock::duration::zero() ? timeout : kDisabledTimeout);
		}

		/**
		 * @brief 算到截止点还剩多久，拿去喂 cancel_after
		 * 每次发 I/O 前现算，这样同一段里的多次读共用一个绝对截止点，
		 * 而不是每次读都从头计时（否则慢速滴发的对端能一直拖下去）。
		 * @param deadline 绝对截止点
		 * @return 剩余时长；已经过点就返回 0，cancel_after 见 0 立刻取消
		 */
		std::chrono::steady_clock::duration remainingUntil(std::chrono::steady_clock::time_point deadline) noexcept
		{
			using duration = std::chrono::steady_clock::duration;
			auto left = deadline - std::chrono::steady_clock::now();
			return left > duration::zero() ? left : duration::zero();
		}

		/**
		 * @brief 把一次 I/O 的错误码翻成 NetworkError
		 * cancel_after 到点是把底层操作取消掉，被取消的 socket 操作回的是 operation_aborted；
		 * 这种「被自家定时器掐掉」的情形一律算超时，别跟真实网络错误搅一起。
		 * @param ec I/O 错误码
		 * @param timeoutMessage 判定为超时时写进 message 的描述
		 * @return 网络错误
		 */
		NetworkError ioErrorOf(const boost::system::error_code& ec, const char* timeoutMessage)
		{
			if (ec == boost::asio::error::operation_aborted || ec == boost::asio::error::timed_out)
			{
				return NetworkError {ErrorCode::hTimedOut, timeoutMessage};
			}
			return toNetworkError(ec);
		}

		/// 判断某个 socket 类型是不是 TLS 流。跟 GenericConnection.h 里的 hIsSslStream
		/// 是同一回事，这里单独写一份是为了不把那个重型头（连带一堆 asio 依赖）拖进来。
		template <typename T>
		struct IsTlsStream : std::false_type
		{
		};

		template <typename T>
		struct IsTlsStream<boost::asio::ssl::stream<T>> : std::true_type
		{
		};

		template <typename T>
		inline constexpr bool hIsTlsStream = IsTlsStream<T>::value;

		/**
		 * @brief 取底下的 tcp::socket
		 * 明文就是它自己，TLS 是它包的那条 socket（next_layer）。connect / close /
		 * shutdown 这些只在 TCP 层做的操作都走它，好让明文和 TLS 共用一套写法。
		 * @param sock 明文 socket 或 TLS 流
		 * @return 底层 tcp::socket 引用
		 */
		template <typename SocketType>
		boost::asio::ip::tcp::socket& underlyingTcpSocket(SocketType& sock) noexcept
		{
			if constexpr (hIsTlsStream<SocketType>)
			{
				return sock.next_layer();
			}
			else
			{
				return sock;
			}
		}

		/**
		 * @brief 把 TLS 握手的错误码翻成 NetworkError
		 * 证书类失败优先判：握手挂了但 verify 结果显示对端证书链验不过，就是证书问题。
		 * 单看错误码不靠谱——OpenSSL 的错误队列里第一条常是 X509 那条而不是 SSL 那条，
		 * 光靠 reason code 分辨会漏。
		 * @param ec 握手返回的错误码
		 * @param ssl 底层 SSL 句柄；校验开着时传，关了传 nullptr
		 * @return 网络错误
		 */
		NetworkError tlsHandshakeErrorOf(const boost::system::error_code& ec, SSL* ssl)
		{
			if (ec == boost::asio::error::operation_aborted || ec == boost::asio::error::timed_out)
			{
				return NetworkError {ErrorCode::hTimedOut, "tls handshake timed out"};
			}

			if (ssl != nullptr)
			{
				long verifyResult = SSL_get_verify_result(ssl);
				if (verifyResult != X509_V_OK)
				{
					return NetworkError {ErrorCode::hSslInvalidCertificate,
										 X509_verify_cert_error_string(verifyResult)};
				}
			}

			if (ec.category() == boost::asio::error::get_ssl_category())
			{
				unsigned long reason = ERR_GET_REASON(static_cast<unsigned long>(ec.value()));
				switch (reason)
				{
					case SSL_R_CERTIFICATE_VERIFY_FAILED:
						return NetworkError {ErrorCode::hSslInvalidCertificate, ec.message()};
					case SSL_R_NO_SHARED_CIPHER:
					case SSL_R_UNSUPPORTED_PROTOCOL:
					case SSL_R_WRONG_VERSION_NUMBER:
					case SSL_R_UNKNOWN_PROTOCOL:
						return NetworkError {ErrorCode::hSslProtocolError, ec.message()};
					default:
						return NetworkError {ErrorCode::hSslHandshakeError, ec.message()};
				}
			}

			// 对端在优雅关闭前把连接断了：也算握手没成
			if (ec == boost::asio::ssl::error::stream_truncated)
			{
				return NetworkError {ErrorCode::hSslHandshakeError, ec.message()};
			}

			return toNetworkError(ec);
		}

	} // namespace

	HttpClient::Transport HttpClient::makeTransport(boost::asio::io_context& io, const HttpClientOptions& options)
	{
		// tlsContext 非空即 HTTPS：建 TLS 流，否则建裸 socket
		if (options.tlsContext)
		{
			return Transport(std::in_place_index<1>, io, options.tlsContext->native());
		}
		return Transport(std::in_place_index<0>, io);
	}

	void HttpClient::close()
	{
		boost::system::error_code ec;
		// 明文关自己，TLS 关它包的底层 socket——协议层不发 close_notify，
		// 客户端要的是「立刻断」，close_notify 这一轮已经没必要客气了。
		std::visit(
			[&ec](auto& sock)
			{
				auto& tcp = underlyingTcpSocket(sock);
				if (tcp.is_open())
				{
					tcp.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
					tcp.close(ec);
				}
			},
			transport_);
		state_ = ConnectionState::hClosed;
	}

	std::string HttpClient::buildRequest(HttpMethod method,
										 std::string_view target,
										 std::string_view body,
										 const HeaderMap& headers) const
	{
		std::string req;
		req.reserve(256 + body.size() + headers.size() * 32);

		// 请求行
		req.append(httpMethodToString(method));
		req.push_back(' ');
		req.append(target);
		req.append(" HTTP/1.1\r\n");

		// Host 必带：HTTP/1.1 少了它服务端有权直接回 400。默认端口不回填
		req.append("Host: ");
		req.append(host_);
		if (port_ != kDefaultHttpPort)
		{
			req.push_back(':');
			req.append(std::to_string(port_));
		}
		req.append("\r\n");

		// 有 body 才带长度，GET 这种没 body 的就别画蛇添足
		if (!body.empty())
		{
			req.append("Content-Length: ");
			req.append(std::to_string(body.size()));
			req.append("\r\n");
		}

		// 默认保持长连接
		req.append("Connection: keep-alive\r\n");

		// 用户自定义头，客户端托管的几个跳过
		for (const auto& [name, value] : headers)
		{
			if (isManagedRequestHeader(name))
			{
				continue;
			}
			req.append(name);
			req.append(": ");
			req.append(value);
			req.append("\r\n");
		}

		req.append("\r\n");
		req.append(body);
		return req;
	}

	template <typename SocketType>
	Awaitable<std::size_t> HttpClient::readIntoRecvBuf(SocketType& sock,
													   boost::system::error_code& ec,
													   std::chrono::steady_clock::time_point deadline)
	{
		// 缓冲塞满了就翻倍，但不超过 kMaxRecvBuffer；已经顶到上限还没地方写就报 no_buffer_space
		if (recvUsed_ >= recvBuf_.size())
		{
			std::size_t newSize = recvBuf_.empty() ? kInitialRecvBuffer : recvBuf_.size() * 2;
			if (newSize > kMaxRecvBuffer)
			{
				newSize = kMaxRecvBuffer;
			}
			if (newSize <= recvUsed_)
			{
				ec = boost::asio::error::no_buffer_space;
				co_return 0;
			}
			recvBuf_.resize(newSize);
		}

		std::size_t bytesRead = co_await sock.async_read_some(
			boost::asio::buffer(recvBuf_.data() + recvUsed_, recvBuf_.size() - recvUsed_),
			boost::asio::cancel_after(remainingUntil(deadline),
									  boost::asio::redirect_error(boost::asio::use_awaitable, ec)));
		if (!ec)
		{
			recvUsed_ += bytesRead;
		}
		co_return bytesRead;
	}

	Awaitable<HttpClientResult> HttpClient::request(HttpMethod method,
													std::string_view target,
													std::string_view body,
													const HeaderMap& headers)
	{
		// tlsContext 非空即 HTTPS。这里不搞成协程，直接把对应那份 requestOn 的 awaitable
		// 返出去，省掉一层只会转发 co_await 的协程帧（明文是主路径，能省则省）。
		// 两条路各实例化一份 requestOn，请求序列化 / 读循环 / body 读取是同一份代码，
		// 只有建连 / 握手 / 关闭靠 if constexpr 分。
		if (std::holds_alternative<TlsSocket>(transport_))
		{
			return requestOn<TlsSocket>(method, target, body, headers);
		}
		return requestOn<PlainSocket>(method, target, body, headers);
	}

	template <typename SocketType>
	Awaitable<NetworkError> HttpClient::ensureConnected()
	{
		recvBuf_.clear();
		recvUsed_ = 0;
		state_ = ConnectionState::hConnecting;
		boost::system::error_code ec;

		if constexpr (hIsTlsStream<SocketType>)
		{
			// 重连一律把 ssl::stream 重建一遍：里面的 SSL 对象用过一次就没法直接接到
			// 新的 TCP 连接上重来，残留的会话状态会让下一次 handshake 报 "bad record type"。
			// 明文 socket 没这包袱，复用即可。
			transport_.emplace<SocketType>(ioCtx_, options_.tlsContext->native());
		}
		else
		{
			auto& tcp = std::get<SocketType>(transport_);
			if (tcp.is_open())
			{
				tcp.close(ec);
			}
		}

		auto& sock = std::get<SocketType>(transport_);

		// 建连阶段（域名解析 + TCP 连接 + TLS 握手）共用一个截止点。解析走的是 resolver，
		// Boost 当前不给它挂 per-op 取消，所以这一步的限时算尽力而为；连接和握手本身
		// 是 socket 操作，超时会实实在在把它们取消掉。
		// 握手算在建连这一段：从调用方看「连接可用」就包含握手完成，跟 connect 共用一个
		// 预算，慢握手也就拖不出 connectTimeout。
		auto connectDeadline = makeDeadline(options_.connectTimeout);

		std::string service = std::to_string(port_);
		boost::asio::ip::tcp::resolver resolver(ioCtx_);
		auto endpoints = co_await resolver.async_resolve(
			host_,
			service,
			boost::asio::cancel_after(remainingUntil(connectDeadline),
									  boost::asio::redirect_error(boost::asio::use_awaitable, ec)));
		if (ec)
		{
			close();
			co_return ioErrorOf(ec, "connect timed out");
		}

		co_await boost::asio::async_connect(
			underlyingTcpSocket(sock),
			endpoints,
			boost::asio::cancel_after(remainingUntil(connectDeadline),
									  boost::asio::redirect_error(boost::asio::use_awaitable, ec)));
		if (ec)
		{
			close();
			co_return ioErrorOf(ec, "connect timed out");
		}

		if constexpr (hIsTlsStream<SocketType>)
		{
			// SNI 必设：不报 Host 名的话真实 CDN 会拒连或给出不匹配的证书。
			// 本机自签证书不看 SNI，漏了它本机测不出来——但线上一连就是挂。
			const std::string& sni = options_.sniHost.empty() ? host_ : options_.sniHost;
			if (SSL_set_tlsext_host_name(sock.native_handle(), sni.c_str()) != 1)
			{
				close();
				co_return NetworkError {ErrorCode::hSslHandshakeError, "failed to set SNI host name"};
			}

			// 校不校验证书按连接设，不动共享的 SslContext——同一条 context 可能被
			// 明文/TLS 多个客户端共用，改它的 verify 模式会互相打架。
			boost::system::error_code verifyEc;
			sock.set_verify_mode(options_.verifyPeer ? boost::asio::ssl::verify_peer : boost::asio::ssl::verify_none,
								 verifyEc);
			if (verifyEc)
			{
				close();
				co_return tlsHandshakeErrorOf(verifyEc, nullptr);
			}

			co_await sock.async_handshake(
				boost::asio::ssl::stream_base::client,
				boost::asio::cancel_after(remainingUntil(connectDeadline),
										  boost::asio::redirect_error(boost::asio::use_awaitable, ec)));
			if (ec)
			{
				// 只有真开了校验才拿 SSL 句柄去读 verify 结果；verify_none 下
				// SSL_get_verify_result 不是证书失败的判据。
				NetworkError err = tlsHandshakeErrorOf(ec, options_.verifyPeer ? sock.native_handle() : nullptr);
				close();
				co_return err;
			}
		}

		state_ = ConnectionState::hActive;
		co_return NetworkError {};
	}

	template <typename SocketType>
	Awaitable<HttpClientResult> HttpClient::requestOn(HttpMethod method,
													  std::string_view target,
													  std::string_view body,
													  const HeaderMap& headers)
	{
		// 先把入参序列化进本地串，之后才允许挂起。target / body / headers 都是
		// 调用方的对象，协程帧按引用存着它们，一旦 co_await 出去、调用方那边析构，
		// 醒来再读就是悬空引用。本函数是被 request() 同步调起的，会在第一个 co_await
		// 之前跑到这里，所以这一步仍然赶在任何挂起之前。
		std::string requestBytes = buildRequest(method, target, body, headers);

		HttpClientResult result;

		// 复用还是重建：上次 body 读干净了、state_ 还停在 hActive 且底层 socket 还开着，
		// 就接着用这条连接，省掉 resolve + connect（TLS 连握手一起省）。
		if (state_ != ConnectionState::hActive || !underlyingTcpSocket(std::get<SocketType>(transport_)).is_open())
		{
			NetworkError connectError = co_await ensureConnected<SocketType>();
			if (!connectError.ok())
			{
				result.error = std::move(connectError);
				co_return result;
			}
		}

		// 引用得等 ensureConnected 之后再取：TLS 那条路里它刚把 ssl::stream 重建过，
		// 重建前拿的引用会悬空。
		auto& sock = std::get<SocketType>(transport_);
		boost::system::error_code ec;

		// 请求阶段（写请求 + 读整段响应）共用一个截止点
		auto requestDeadline = makeDeadline(options_.requestTimeout);

		co_await boost::asio::async_write(
			sock,
			boost::asio::buffer(requestBytes),
			boost::asio::cancel_after(remainingUntil(requestDeadline),
									  boost::asio::redirect_error(boost::asio::use_awaitable, ec)));
		if (ec)
		{
			result.error = ioErrorOf(ec, "request timed out");
			close();
			co_return result;
		}

		// ====== 读 + 解析响应头 ======
		// 循环结构照 HttpSessionImpl 那段来：缓冲区里还有没试过的字节就先解析，
		// 不够再 read。phr_parse_response 返回 -2 是「还不完整」，-1 是「格式错」，
		// >0 是头部解析完，值正好是头部的字节数（body 从这里开始）。
		const char* msg = nullptr;
		std::size_t msgLen = 0;
		int minorVersion = 0;
		int status = 0;
		phr_header headerSlots[kMaxResponseHeaders];
		std::size_t numHeaders = kMaxResponseHeaders;
		std::size_t prevLen = 0;

		// 复用连接时 recvBuf_ 里可能还留着上次的残段，只补到初始容量，别缩容把它冲掉
		if (recvBuf_.size() < kInitialRecvBuffer)
		{
			recvBuf_.resize(kInitialRecvBuffer);
		}

		std::size_t headerLen = 0;
		for (;;)
		{
			if (recvUsed_ > prevLen)
			{
				numHeaders = kMaxResponseHeaders;
				int parseResult = phr_parse_response(recvBuf_.data(),
													 recvUsed_,
													 &minorVersion,
													 &status,
													 &msg,
													 &msgLen,
													 headerSlots,
													 &numHeaders,
													 prevLen);
				prevLen = recvUsed_;

				if (parseResult > 0)
				{
					headerLen = static_cast<std::size_t>(parseResult);
					break;
				}
				if (parseResult == -1)
				{
					result.error.code = ErrorCode::hUnknown;
					result.error.message = "malformed HTTP response";
					close();
					co_return result;
				}
				// -2：数据不完整，落到下面继续 read
			}

			if (recvUsed_ >= recvBuf_.size())
			{
				if (recvBuf_.size() >= kMaxRecvBuffer)
				{
					result.error.code = ErrorCode::hUnknown;
					result.error.message = "response header too large";
					close();
					co_return result;
				}
				recvBuf_.resize(recvBuf_.size() * 2);
			}

			std::size_t bytesRead = co_await sock.async_read_some(
				boost::asio::buffer(recvBuf_.data() + recvUsed_, recvBuf_.size() - recvUsed_),
				boost::asio::cancel_after(remainingUntil(requestDeadline),
										  boost::asio::redirect_error(boost::asio::use_awaitable, ec)));
			if (ec)
			{
				// 头都没凑齐就断了：eof / reset 算网络错误，超时算 hTimedOut
				result.error = ioErrorOf(ec, "request timed out");
				close();
				co_return result;
			}
			recvUsed_ += bytesRead;
		}

		// ====== 填结果 ======
		result.status = status;
		if (msg != nullptr)
		{
			result.statusText.assign(msg, msgLen);
		}
		for (std::size_t i = 0; i < numHeaders; ++i)
		{
			// 折行续行（obs-fold）picohttpparser 会把 name 置空，跳过
			if (headerSlots[i].name == nullptr)
			{
				continue;
			}
			result.headers.insert(std::string_view(headerSlots[i].name, headerSlots[i].name_len),
								  std::string_view(headerSlots[i].value, headerSlots[i].value_len));
		}

		// 头部和 body 常在同一个 read 里一起到。把头部字节从缓冲区掐掉，
		// [0, recvUsed_) 剩下的就是 body 开头，接着读。
		recvBuf_.erase(0, headerLen);
		recvUsed_ -= headerLen;

		// ====== 读 body ======
		// 三条路：chunked / Content-Length / 读到 EOF。Transfer-Encoding 优先于
		// Content-Length（RFC 7230 §3.3.3），两个同时出现按 chunked 走。
		// 1xx / 204 / 304 按规范就没有 body，别去等 EOF 把连接吊死。
		bool isChunked = headerHasToken(result.headers.find("Transfer-Encoding"), "chunked");
		bool bodyless = (status >= 100 && status < 200) || status == 204 || status == 304;
		bool reachedEof = false;

		if (bodyless)
		{
			// 无 body，什么都不读
		}
		else if (isChunked)
		{
			// 原地解码，契约照 HttpSessionImpl 那段：buf[0, recvUsed_) 是待解码编码数据，
			// *len 进出参（进=编码长度，出=本次解码字节数）。
			//   -2：不完整，但编码数据已被全部消费（recvUsed_ 清零继续读）；
			//   -1：编码错误；
			//   >=0：解码完成，返回值是 chunked 之后的残留字节数（从 buf[*len] 起）。
			struct phr_chunked_decoder decoder = {};
			for (;;)
			{
				std::size_t decodedLen = recvUsed_;
				ssize_t decodeRet = phr_decode_chunked(&decoder, recvBuf_.data(), &decodedLen);
				if (decodedLen > 0)
				{
					result.body.append(recvBuf_.data(), decodedLen);
				}

				if (decodeRet >= 0)
				{
					// 完成：把 chunked 后面的残留搬回缓冲区开头，留给下次复用连接时先解析
					std::size_t tail = static_cast<std::size_t>(decodeRet);
					if (tail > 0)
					{
						std::memmove(recvBuf_.data(), recvBuf_.data() + decodedLen, tail);
					}
					recvUsed_ = tail;
					break;
				}
				if (decodeRet == -1)
				{
					result.error.code = ErrorCode::hUnknown;
					result.error.message = "malformed chunked body";
					close();
					co_return result;
				}

				// decodeRet == -2：编码数据已全部消费，清空缓冲继续读
				recvUsed_ = 0;
				co_await readIntoRecvBuf(sock, ec, requestDeadline);
				if (ec)
				{
					// 编码中途断了：chunked 没收尾，按错误处理
					result.error = ioErrorOf(ec, "request timed out");
					close();
					co_return result;
				}
			}
		}
		else
		{
			std::size_t contentLength = 0;
			bool hasContentLength = parseContentLength(result.headers.find("Content-Length"), contentLength);

			if (hasContentLength)
			{
				// 头解析时可能已经把 body 开头（甚至整段）读进来了，先搬走
				std::size_t prefix = std::min(recvUsed_, contentLength);
				result.body.append(recvBuf_.data(), prefix);
				recvBuf_.erase(0, prefix);
				recvUsed_ -= prefix;

				std::size_t remaining = contentLength - prefix;
				char chunk[kBodyReadChunk];
				while (remaining > 0)
				{
					std::size_t want = std::min<std::size_t>(remaining, sizeof(chunk));
					std::size_t bytesRead = co_await sock.async_read_some(
						boost::asio::buffer(chunk, want),
						boost::asio::cancel_after(remainingUntil(requestDeadline),
												  boost::asio::redirect_error(boost::asio::use_awaitable, ec)));
					if (ec)
					{
						result.error = ioErrorOf(ec, "request timed out");
						close();
						co_return result;
					}
					result.body.append(chunk, bytesRead);
					remaining -= bytesRead;
				}
			}
			else
			{
				// 没有长度信息也没 chunked：读到对端关连接为止，EOF 收尾
				reachedEof = true;
				result.body.append(recvBuf_.data(), recvUsed_);
				recvUsed_ = 0;

				char chunk[kBodyReadChunk];
				for (;;)
				{
					std::size_t bytesRead = co_await sock.async_read_some(
						boost::asio::buffer(chunk, sizeof(chunk)),
						boost::asio::cancel_after(remainingUntil(requestDeadline),
												  boost::asio::redirect_error(boost::asio::use_awaitable, ec)));
					if (ec == boost::asio::error::eof)
					{
						break;
					}
					if (ec)
					{
						// 超时取消回的是 operation_aborted，不会误判成 EOF
						result.error = ioErrorOf(ec, "request timed out");
						close();
						co_return result;
					}
					result.body.append(chunk, bytesRead);
				}
			}
		}

		// ====== keep-alive 判定 ======
		// HTTP/1.1 默认保持，除非响应说 close；HTTP/1.0 默认关闭，除非响应说 keep-alive。
		// 靠 EOF 收尾的连接对端已经关了，必须置 closed，否则下次请求会撞上一条半死连接。
		std::string_view connectionHeader = result.headers.find("Connection");
		bool keepAlive = minorVersion >= 1 ? !headerHasToken(connectionHeader, "close")
										   : headerHasToken(connectionHeader, "keep-alive");
		if (reachedEof)
		{
			keepAlive = false;
		}

		if (keepAlive)
		{
			state_ = ConnectionState::hActive;
		}
		else
		{
			close();
		}

		co_return result;
	}

	Awaitable<HttpClientResult> HttpClient::get(std::string_view target, const HeaderMap& headers)
	{
		co_return co_await request(HttpMethod::hGet, target, {}, headers);
	}

	Awaitable<HttpClientResult> HttpClient::post(std::string_view target,
												 std::string_view body,
												 std::string_view contentType)
	{
		// 这个局部 HeaderMap 活在协程帧里，跨 co_await 安全；别把它改成
		// 非协程的 return request(...)，那样局部头表会先析构再被 request 引用。
		HeaderMap headers;
		headers.set("Content-Type", contentType);
		co_return co_await request(HttpMethod::hPost, target, body, headers);
	}

} // namespace hical
