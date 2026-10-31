/**
 * @file HttpClient.cpp
 * @brief 协程 HTTP 客户端实现：建连、请求序列化、解析响应头、读 body
 * 响应 body 按头部选路：Transfer-Encoding: chunked 走原地解码，Content-Length 读满
 * 指定字节，两者都没有就靠连接关闭（EOF）收尾。body 读干净后连接才允许复用。
 * 每段 I/O 都使用绝对期限取消，超过 connectTimeout / requestTimeout 直接取消，
 * 免得对端接上就不吭声时把请求永久吊住。
 * 明文（tcp::socket）和 TLS（ssl::stream<tcp::socket>）共用 requestOn() 这一份实现，
 * 建连 / 握手 / 关闭的差异靠 if constexpr 在编译期分掉，请求序列化、读循环、body
 * 读取不分叉。选哪条由 tlsContext 是否为空决定。
 */

#include "HttpClient.h"
#ifdef HICAL_HAS_HTTP2
	#include "Http2ClientSession.h"
#endif
#include <boost/asio/cancel_at.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/write.hpp>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509_vfy.h>
#include <openssl/x509v3.h>
#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstring>
#include <deque>
#include <optional>
#include <stdexcept>
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

		/**
		 * @brief 将超时时长转换为绝对期限
		 * @param timeout 配置的时长；非正值当作不设超时
		 * @return 截止时刻
		 */
		std::chrono::steady_clock::time_point makeDeadline(std::chrono::steady_clock::duration timeout) noexcept
		{
			using clock = std::chrono::steady_clock;
			const auto now = clock::now();
			return timeout <= clock::duration::zero() || timeout >= clock::time_point::max() - now
					   ? clock::time_point::max()
					   : now + timeout;
		}

		/**
		 * @brief 把一次 I/O 的错误码翻成 NetworkError
		 * 期限到点会取消底层操作，被取消的 socket 操作回的是 operation_aborted；
		 * 此处将期限取消转换为超时，主动取消由调用方另行判断。
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

		NetworkError configureTlsIdentity(SSL* ssl, const std::string& host, bool verifyPeer)
		{
			SSL_set_verify(ssl, verifyPeer ? SSL_VERIFY_PEER : SSL_VERIFY_NONE, SSL_get_verify_callback(ssl));
			if (!verifyPeer)
			{
				return {};
			}
			if (host.empty() || host.find('\0') != std::string::npos)
			{
				return {ErrorCode::hInvalidConfiguration, "TLS 目标身份不能为空或包含空字符"};
			}
			auto* parameters = SSL_get0_param(ssl);
			if (SSL_set1_host(ssl, nullptr) != 1 || X509_VERIFY_PARAM_set1_ip(parameters, nullptr, 0) != 1)
			{
				return {ErrorCode::hInvalidConfiguration, "无法清理 TLS 连接继承的目标身份"};
			}
			SSL_set_hostflags(ssl, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
			boost::system::error_code error;
			const auto address = boost::asio::ip::make_address(host, error);
			int configured = 0;
			if (error)
			{
				configured = SSL_set1_host(ssl, host.c_str());
			}
			else if (address.is_v4())
			{
				const auto bytes = address.to_v4().to_bytes();
				configured = X509_VERIFY_PARAM_set1_ip(parameters, bytes.data(), bytes.size());
			}
			else
			{
				const auto bytes = address.to_v6().to_bytes();
				configured = X509_VERIFY_PARAM_set1_ip(parameters, bytes.data(), bytes.size());
			}
			return configured == 1 ? NetworkError {}
								   : NetworkError {ErrorCode::hInvalidConfiguration, "无法设置 TLS 目标身份"};
		}

	} // namespace

	struct HttpClient::OwnedRequest
	{
		struct PendingTicket
		{
			explicit PendingTicket(std::shared_ptr<Impl> connection);
			~PendingTicket();
			PendingTicket(PendingTicket&&) noexcept = default;
			PendingTicket(const PendingTicket&) = delete;
			PendingTicket& operator=(const PendingTicket&) = delete;
			std::shared_ptr<Impl> owner;
		};

		HttpMethod method;
		std::string target;
		std::string body;
		HeaderMap headers;
		std::chrono::steady_clock::duration timeout;
		std::optional<std::chrono::steady_clock::time_point> deadline;
		PendingTicket pending;
	};

	struct HttpClient::Impl : std::enable_shared_from_this<Impl>
	{
		using PlainSocket = boost::asio::ip::tcp::socket;
		using TlsSocket = boost::asio::ssl::stream<PlainSocket>;
		using Transport = std::variant<std::shared_ptr<PlainSocket>, std::shared_ptr<TlsSocket>>;
		using Clock = std::chrono::steady_clock;
		enum class ConnectionState : uint8_t
		{
			hIdle,
			hConnecting,
			hActive,
			hClosed
		};
		static constexpr size_t kInitialRecvBuffer = 4096;
		static constexpr size_t kMaxRecvBuffer = 64 * 1024;

		struct GateWaiter
		{
			GateWaiter(boost::asio::io_context& io, uint64_t token, Clock::time_point deadline)
				: timer(io, deadline), generation(token)
			{
			}

			boost::asio::steady_timer timer;
			uint64_t generation;
			bool granted {false};
			bool abandoned {false};
		};

		struct GateLease
		{
			explicit GateLease(Impl* connection = nullptr) : owner(connection)
			{
			}

			~GateLease()
			{
				if (owner)
				{
					owner->releaseGate();
				}
			}

			GateLease(const GateLease&) = delete;
			GateLease& operator=(const GateLease&) = delete;

			GateLease(GateLease&& other) noexcept : owner(std::exchange(other.owner, nullptr))
			{
			}

			GateLease& operator=(GateLease&&) = delete;

			explicit operator bool() const noexcept
			{
				return owner != nullptr;
			}

			Impl* owner;
		};

		struct ExchangeGuard
		{
			~ExchangeGuard()
			{
				if (!completed && owner.transportGeneration_ == generation)
				{
					owner.disconnect(generation);
					// 此时本次 I/O 已收尾且独占门仍在，才能清理可能被异步读取借用的缓冲区。
					owner.recvBuf_.clear();
					owner.recvUsed_ = 0;
				}
			}

			Impl& owner;
			uint64_t generation;
			bool completed {false};
		};

		struct ConnectFlight
		{
			ConnectFlight(boost::asio::io_context& io, uint64_t token)
				: done(io, Clock::time_point::max()), generation(token)
			{
			}

			boost::asio::steady_timer done;
			uint64_t generation;
			NetworkError error;
			bool completed {false};
		};

		Impl(boost::asio::io_context& io, std::string host, uint16_t port, HttpClientOptions options)
			: ioCtx_(io)
			, host_(std::move(host))
			, port_(port)
			, options_(std::move(options))
			, key_(host_ + ":" + std::to_string(port_))
			, transport_(makeTransport(ioCtx_, options_))
		{
		}

		static Transport makeTransport(boost::asio::io_context& io, const HttpClientOptions& options)
		{
			if (options.tlsContext)
			{
				return Transport(std::in_place_index<1>, std::make_shared<TlsSocket>(io, options.tlsContext->native()));
			}
			return Transport(std::in_place_index<0>, std::make_shared<PlainSocket>(io));
		}

		bool current(uint64_t token) const noexcept
		{
			return !stopping_.load(std::memory_order_acquire) && generation_.load(std::memory_order_acquire) == token;
		}

		bool active(uint64_t token) const
		{
			return current(token) && state_ == ConnectionState::hActive && transportGeneration_ == token
				   && std::visit(
					   [](const auto& socket)
					   {
						   return underlyingTcpSocket(*socket).is_open();
					   },
					   transport_);
		}

		static NetworkError cancelled()
		{
			return {ErrorCode::hOperationAborted, "HTTP 客户端请求已取消"};
		}

		static NetworkError requestTimedOut()
		{
			return {ErrorCode::hTimedOut, "HTTP 请求已超过原绝对期限"};
		}

		void notifyPoolCapacity()
		{
			if (capacityChanged_)
			{
				capacityChanged_();
			}
		}

		NetworkError requestError(const boost::system::error_code& error,
								  const char* message,
								  uint64_t token,
								  bool callerCancelled = false) const
		{
			return callerCancelled || !current(token) ? cancelled() : ioErrorOf(error, message);
		}

		void disconnect(uint64_t token)
		{
			if (transportGeneration_ != token)
			{
				return;
			}
#ifdef HICAL_HAS_HTTP2
			if (http2Session_)
			{
				http2Session_->close();
			}
#endif
			boost::system::error_code ignored;
			std::visit(
				[&](auto& socket)
				{
					auto& tcp = underlyingTcpSocket(*socket);
					tcp.cancel(ignored);
					tcp.close(ignored);
				},
				transport_);
			state_ = ConnectionState::hClosed;
			liveGeneration_.store(0, std::memory_order_release);
			notifyPoolCapacity();
		}

		void stopOnLoop(uint64_t cutoff)
		{
			// 只有代次和停止标志通过原子变量跨线程访问；socket、resolver 和门队列只在所属 loop 使用。
			if (transportGeneration_ <= cutoff)
			{
				if (resolver_)
				{
					resolver_->cancel();
				}
				disconnect(transportGeneration_);
			}
			for (const auto& waiter : gateWaiters_)
			{
				if (waiter->generation <= cutoff)
				{
					waiter->abandoned = true;
					waiter->timer.cancel();
				}
			}
		}

		void scheduleStop(bool terminal)
		{
			if (terminal)
			{
				stopping_.store(true, std::memory_order_release);
			}
			const auto cutoff = generation_.fetch_add(1, std::memory_order_acq_rel);
			auto self = shared_from_this();
			boost::asio::post(ioCtx_,
							  [self, cutoff]
							  {
								  self->stopOnLoop(cutoff);
							  });
		}

		void abandonGate(const std::shared_ptr<GateWaiter>& waiter)
		{
			waiter->abandoned = true;
			const auto found = std::find(gateWaiters_.begin(), gateWaiters_.end(), waiter);
			if (found != gateWaiters_.end())
			{
				gateWaiters_.erase(found);
			}
			if (waiter->granted)
			{
				// 授予与取消同轮完成时，未交给调用方的门也必须归还一次。
				releaseGate();
			}
		}

		Awaitable<GateLease> acquireGate(uint64_t token,
										 Clock::time_point deadline = Clock::time_point::max(),
										 NetworkError* failure = nullptr)
		{
			auto cancellation = co_await boost::asio::this_coro::cancellation_state;
			auto reject = [&]
			{
				if (failure)
				{
					*failure = !current(token) || cancellation.cancelled() != boost::asio::cancellation_type::none
								   ? cancelled()
								   : requestTimedOut();
				}
			};
			if (!current(token) || Clock::now() >= deadline
				|| cancellation.cancelled() != boost::asio::cancellation_type::none)
			{
				reject();
				co_return GateLease {};
			}
			if (!gateBusy_)
			{
				gateBusy_ = true;
				co_return GateLease(this);
			}
			auto waiter = std::make_shared<GateWaiter>(ioCtx_, token, deadline);
			gateWaiters_.push_back(waiter);

			struct Registration
			{
				Impl& owner;
				std::shared_ptr<GateWaiter> waiter;
				bool claimed {false};

				~Registration()
				{
					if (!claimed)
					{
						owner.abandonGate(waiter);
					}
				}
			} registration {*this, waiter};

			boost::system::error_code error;
			co_await waiter->timer.async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, error));
			if (!waiter->granted)
			{
				if (failure)
				{
					*failure = error || !current(token) ? cancelled() : requestTimedOut();
				}
				co_return GateLease {};
			}
			registration.claimed = true;
			GateLease lease(this);
			if (!current(token) || Clock::now() >= deadline
				|| cancellation.cancelled() != boost::asio::cancellation_type::none)
			{
				reject();
				co_return GateLease {};
			}
			co_return lease;
		}

		void releaseGate()
		{
			while (!gateWaiters_.empty())
			{
				auto waiter = std::move(gateWaiters_.front());
				gateWaiters_.pop_front();
				if (waiter->abandoned || !current(waiter->generation))
				{
					waiter->abandoned = true;
					waiter->timer.cancel();
					continue;
				}
				waiter->granted = true;
				waiter->timer.cancel();
				return;
			}
			gateBusy_ = false;
		}

		Awaitable<NetworkError> ensureConnected(uint64_t token, Clock::time_point deadline)
		{
			auto cancellation = co_await boost::asio::this_coro::cancellation_state;
			for (;;)
			{
				if (!current(token))
				{
					co_return cancelled();
				}
				if (Clock::now() >= deadline)
				{
					co_return requestTimedOut();
				}
				if (active(token))
				{
					co_return NetworkError {};
				}
				auto flight = connecting_;
				if (!flight || flight->completed)
				{
					flight = std::make_shared<ConnectFlight>(ioCtx_, token);
					connecting_ = flight;
					coSpawn(ioCtx_, connectOwned(shared_from_this(), flight));
					notifyPoolCapacity();
				}
				if (!flight->completed)
				{
					boost::system::error_code error;
					// 超时只取消当前等待，共享建连的 resolver、socket 和完成通知继续运行。
					co_await flight->done.async_wait(
						boost::asio::cancel_at(deadline,
											   boost::asio::redirect_error(boost::asio::use_awaitable, error)));
					if (cancellation.cancelled() != boost::asio::cancellation_type::none)
					{
						co_return cancelled();
					}
					if (Clock::now() >= deadline)
					{
						co_return requestTimedOut();
					}
					if (!flight->completed)
					{
						co_return cancelled();
					}
				}
				if (!current(token))
				{
					co_return cancelled();
				}
				if (flight->generation == token)
				{
					co_return flight->error;
				}
				// 新代次须等旧建连协程归还独占门，避免替换仍在握手的 TLS 对象。
			}
		}

		static Awaitable<void> connectOwned(std::shared_ptr<Impl> self, std::shared_ptr<ConnectFlight> flight)
		{
			NetworkError result;
			try
			{
				auto lease = co_await self->acquireGate(flight->generation);
				if (!lease)
				{
					result = cancelled();
				}
				else if (self->options_.tlsContext)
				{
					result = co_await self->connectOn<TlsSocket>(flight->generation);
#ifdef HICAL_HAS_HTTP2
					if (!result && self->protocol_ == HttpProtocol::hHttp2)
					{
						coSpawn(self->ioCtx_,
								runHttp2Owned(self, self->http2Session_, flight->generation, std::move(lease)));
					}
#endif
				}
				else
				{
					result = co_await self->connectOn<PlainSocket>(flight->generation);
				}
			}
			catch (const boost::system::system_error& exception)
			{
				self->disconnect(flight->generation);
				result = self->current(flight->generation) ? toNetworkError(exception.code()) : cancelled();
			}
			catch (const std::invalid_argument& exception)
			{
				self->disconnect(flight->generation);
				result = {ErrorCode::hInvalidConfiguration, exception.what()};
			}
			catch (const std::exception& exception)
			{
				self->disconnect(flight->generation);
				result = {ErrorCode::hUnknown, exception.what()};
			}
			flight->error = std::move(result);
			flight->completed = true;
			flight->done.cancel();
			self->notifyPoolCapacity();
		}

#ifdef HICAL_HAS_HTTP2
		static Awaitable<void> runHttp2Owned(std::shared_ptr<Impl> self,
											 std::shared_ptr<detail::Http2ClientSession> session,
											 uint64_t generation,
											 GateLease lease)
		{
			// 局部守卫先于 self 析构，归还独占门时 Impl 仍存活；Impl 不持有这个后台任务。
			auto guard = std::move(lease);
			try
			{
				co_await session->run();
			}
			catch (const boost::system::system_error& exception)
			{
				session->close(toNetworkError(exception.code()));
			}
			catch (const std::exception& exception)
			{
				session->close({ErrorCode::hUnknown, exception.what()});
			}
			if (self->transportGeneration_ == generation && self->http2Session_ == session)
			{
				self->http2Session_.reset();
				self->disconnect(generation);
			}
		}
#endif

		std::string buildRequest(HttpMethod method,
								 std::string_view target,
								 std::string_view body,
								 const HeaderMap& headers) const;
		template <typename SocketType>
		Awaitable<size_t> readIntoRecvBuf(SocketType& socket,
										  boost::system::error_code& error,
										  Clock::time_point deadline);
		template <typename SocketType>
		Awaitable<NetworkError> connectOn(uint64_t generation);
		template <typename SocketType>
		Awaitable<HttpClientResult> requestOn(OwnedRequest request, uint64_t generation);

		boost::asio::io_context& ioCtx_;
		std::string host_;
		uint16_t port_;
		HttpClientOptions options_;
		std::string key_;
		ConnectionState state_ {ConnectionState::hIdle};
		HttpProtocol protocol_ {HttpProtocol::hHttp1};
		Transport transport_;
#ifdef HICAL_HAS_HTTP2
		std::shared_ptr<detail::Http2ClientSession> http2Session_;
#endif
		uint64_t transportGeneration_ {0};
		std::atomic<uint64_t> generation_ {1};
		std::atomic<uint64_t> liveGeneration_ {0};
		std::atomic<bool> stopping_ {false};
		std::atomic<size_t> pendingRequests_ {0};
		std::shared_ptr<boost::asio::ip::tcp::resolver> resolver_;
		std::shared_ptr<ConnectFlight> connecting_;
		std::function<void()> capacityChanged_;
		std::deque<std::shared_ptr<GateWaiter>> gateWaiters_;
		bool gateBusy_ {false};
		std::string recvBuf_;
		size_t recvUsed_ {0};
	};

	HttpClient::OwnedRequest::PendingTicket::PendingTicket(std::shared_ptr<Impl> connection)
		: owner(std::move(connection))
	{
		owner->pendingRequests_.fetch_add(1, std::memory_order_relaxed);
	}

	HttpClient::OwnedRequest::PendingTicket::~PendingTicket()
	{
		if (owner)
		{
			owner->pendingRequests_.fetch_sub(1, std::memory_order_release);
		}
	}

	bool HttpClient::hasPendingRequests() const noexcept
	{
		return impl_->pendingRequests_.load(std::memory_order_acquire) != 0;
	}

	HttpClient::PoolCapacity HttpClient::poolCapacity() const noexcept
	{
		PoolCapacity result;
		const auto token = impl_->generation_.load(std::memory_order_acquire);
		result.connected = impl_->active(token);
		result.connecting = impl_->current(token) && impl_->connecting_ && !impl_->connecting_->completed
							&& impl_->connecting_->generation == token;
		result.protocol = impl_->protocol_;
#ifdef HICAL_HAS_HTTP2
		if (result.connected && impl_->http2Session_)
		{
			const auto capacity = impl_->http2Session_->capacity();
			result.settingsSeen = capacity.settingsSeen;
			result.accepting = capacity.accepting;
			result.localLimit = capacity.localLimit;
			result.peerLimit = capacity.peerLimit;
			result.admitted = capacity.admitted;
			result.available = capacity.available;
		}
#endif
		return result;
	}

	void HttpClient::setPoolCapacityChanged(std::function<void()> callback)
	{
		impl_->capacityChanged_ = std::move(callback);
	}

	std::string HttpClient::Impl::buildRequest(HttpMethod method,
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
	Awaitable<std::size_t> HttpClient::Impl::readIntoRecvBuf(SocketType& sock,
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
			boost::asio::cancel_at(deadline, boost::asio::redirect_error(boost::asio::use_awaitable, ec)));
		if (!ec)
		{
			recvUsed_ += bytesRead;
		}
		co_return bytesRead;
	}

	template <typename SocketType>
	Awaitable<NetworkError> HttpClient::Impl::connectOn(uint64_t generation)
	{
		transportGeneration_ = generation;
		protocol_ = HttpProtocol::hHttp1;
#ifdef HICAL_HAS_HTTP2
		http2Session_.reset();
#endif
		recvBuf_.clear();
		recvUsed_ = 0;
		state_ = ConnectionState::hConnecting;
		boost::system::error_code ec;

		if constexpr (hIsTlsStream<SocketType>)
		{
			// 重连一律把 ssl::stream 重建一遍：里面的 SSL 对象用过一次就没法直接接到
			// 新的 TCP 连接上重来，残留的会话状态会让下一次 handshake 报 "bad record type"。
			// 明文 socket 没这包袱，复用即可。
			transport_.emplace<std::shared_ptr<SocketType>>(
				std::make_shared<SocketType>(ioCtx_, options_.tlsContext->native()));
		}
		else
		{
			auto& tcp = *std::get<std::shared_ptr<SocketType>>(transport_);
			if (tcp.is_open())
			{
				tcp.close(ec);
			}
		}

		auto socket = std::get<std::shared_ptr<SocketType>>(transport_);
		auto& sock = *socket;

		// 建连阶段（域名解析 + TCP 连接 + TLS 握手）共用一个截止点。解析走的是 resolver，
		// Boost 当前不给它挂 per-op 取消，所以这一步的限时算尽力而为；连接和握手本身
		// 是 socket 操作，超时会实实在在把它们取消掉。
		// 握手算在建连这一段：从调用方看「连接可用」就包含握手完成，跟 connect 共用一个
		// 预算，慢握手也就拖不出 connectTimeout。
		auto connectDeadline = makeDeadline(options_.connectTimeout);

		std::string service = std::to_string(port_);
		auto resolver = std::make_shared<boost::asio::ip::tcp::resolver>(ioCtx_);
		resolver_ = resolver;
		auto endpoints = co_await resolver->async_resolve(
			host_,
			service,
			boost::asio::cancel_at(connectDeadline, boost::asio::redirect_error(boost::asio::use_awaitable, ec)));
		if (ec || !current(generation))
		{
			disconnect(generation);
			co_return requestError(ec, "connect timed out", generation);
		}

		co_await boost::asio::async_connect(
			underlyingTcpSocket(sock),
			endpoints,
			boost::asio::cancel_at(connectDeadline, boost::asio::redirect_error(boost::asio::use_awaitable, ec)));
		if (ec || !current(generation))
		{
			disconnect(generation);
			co_return requestError(ec, "connect timed out", generation);
		}

		if constexpr (hIsTlsStream<SocketType>)
		{
			// SNI 只决定服务端路由，证书身份始终使用构造时的目标 host。
			const std::string& sni = options_.sniHost.empty() ? host_ : options_.sniHost;
			if (sni.empty() || sni.find('\0') != std::string::npos
				|| SSL_set_tlsext_host_name(sock.native_handle(), sni.c_str()) != 1)
			{
				disconnect(generation);
				co_return NetworkError {ErrorCode::hSslHandshakeError, "failed to set SNI host name"};
			}

			if (auto error = configureTlsIdentity(sock.native_handle(), host_, options_.verifyPeer))
			{
				disconnect(generation);
				co_return error;
			}
#ifdef HICAL_HAS_HTTP2
			if (auto error = SslContext::configureClientHttpProtocols(sock.native_handle(), options_.protocolMode))
			{
				disconnect(generation);
				co_return error;
			}
#else
			constexpr unsigned char kHttp1Alpn[] = {8, 'h', 't', 't', 'p', '/', '1', '.', '1'};
			if (SSL_set_alpn_protos(sock.native_handle(), kHttp1Alpn, sizeof(kHttp1Alpn)) != 0)
			{
				disconnect(generation);
				co_return NetworkError {ErrorCode::hInvalidConfiguration, "无法设置客户端 HTTP/1 ALPN"};
			}
#endif

			co_await sock.async_handshake(
				boost::asio::ssl::stream_base::client,
				boost::asio::cancel_at(connectDeadline, boost::asio::redirect_error(boost::asio::use_awaitable, ec)));
			if (ec || !current(generation))
			{
				// 只有真开了校验才拿 SSL 句柄去读 verify 结果；verify_none 下
				// SSL_get_verify_result 不是证书失败的判据。
				NetworkError err = current(generation)
									   ? tlsHandshakeErrorOf(ec, options_.verifyPeer ? sock.native_handle() : nullptr)
									   : cancelled();
				disconnect(generation);
				co_return err;
			}
#ifdef HICAL_HAS_HTTP2
			if (auto error = SslContext::negotiatedHttpProtocol(sock.native_handle(), options_.protocolMode, protocol_))
			{
				disconnect(generation);
				co_return error;
			}
			if (protocol_ == HttpProtocol::hHttp2)
			{
				http2Session_ = detail::Http2ClientSession::create(socket, options_.http2);
				http2Session_->setCapacityChanged(
					[weak = weak_from_this()]
					{
						if (auto self = weak.lock())
						{
							self->notifyPoolCapacity();
						}
					});
			}
#endif
		}

		resolver_.reset();
		state_ = ConnectionState::hActive;
		liveGeneration_.store(generation, std::memory_order_release);
		notifyPoolCapacity();
		co_return NetworkError {};
	}

	template <typename SocketType>
	Awaitable<HttpClientResult> HttpClient::Impl::requestOn(OwnedRequest request, uint64_t generation)
	{
		auto cancellation = co_await boost::asio::this_coro::cancellation_state;
		const auto callerCancelled = [&cancellation]
		{
			return cancellation.cancelled() != boost::asio::cancellation_type::none;
		};
		std::string requestBytes = buildRequest(request.method, request.target, request.body, request.headers);
		HttpClientResult result;
		auto socket = std::get<std::shared_ptr<SocketType>>(transport_);
		auto& sock = *socket;
		boost::system::error_code ec;

		// 普通入口已固定提交期限；序列化结束后再次检查，尚未发送时无需关闭有效连接。
		auto requestDeadline = request.deadline ? *request.deadline : makeDeadline(request.timeout);
		if (Clock::now() >= requestDeadline)
		{
			result.error = requestTimedOut();
			result.requestDisposition = HttpRequestDisposition::hNotSent;
			co_return result;
		}
		ExchangeGuard exchange {*this, generation};

		co_await boost::asio::async_write(
			sock,
			boost::asio::buffer(requestBytes),
			boost::asio::cancel_at(requestDeadline, boost::asio::redirect_error(boost::asio::use_awaitable, ec)));
		if (ec || !current(generation) || callerCancelled())
		{
			result.error = requestError(ec, "request timed out", generation, callerCancelled());
			disconnect(generation);
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
					disconnect(generation);
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
					disconnect(generation);
					co_return result;
				}
				recvBuf_.resize(recvBuf_.size() * 2);
			}

			std::size_t bytesRead = co_await sock.async_read_some(
				boost::asio::buffer(recvBuf_.data() + recvUsed_, recvBuf_.size() - recvUsed_),
				boost::asio::cancel_at(requestDeadline, boost::asio::redirect_error(boost::asio::use_awaitable, ec)));
			if (ec || !current(generation) || callerCancelled())
			{
				// 头都没凑齐就断了：eof / reset 算网络错误，超时算 hTimedOut
				result.error = requestError(ec, "request timed out", generation, callerCancelled());
				disconnect(generation);
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
					disconnect(generation);
					co_return result;
				}

				// decodeRet == -2：编码数据已全部消费，清空缓冲继续读
				recvUsed_ = 0;
				co_await readIntoRecvBuf(sock, ec, requestDeadline);
				if (ec || !current(generation) || callerCancelled())
				{
					// 编码中途断了：chunked 没收尾，按错误处理
					result.error = requestError(ec, "request timed out", generation, callerCancelled());
					disconnect(generation);
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
						boost::asio::cancel_at(requestDeadline,
											   boost::asio::redirect_error(boost::asio::use_awaitable, ec)));
					if (ec || !current(generation) || callerCancelled())
					{
						result.error = requestError(ec, "request timed out", generation, callerCancelled());
						disconnect(generation);
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
						boost::asio::cancel_at(requestDeadline,
											   boost::asio::redirect_error(boost::asio::use_awaitable, ec)));
					if (ec == boost::asio::error::eof && current(generation) && !callerCancelled())
					{
						break;
					}
					if (ec || !current(generation) || callerCancelled())
					{
						// 超时取消回的是 operation_aborted，不会误判成 EOF
						result.error = requestError(ec, "request timed out", generation, callerCancelled());
						disconnect(generation);
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

		if (!current(generation) || callerCancelled())
		{
			result.error = cancelled();
			disconnect(generation);
			co_return result;
		}

		if (keepAlive)
		{
			state_ = ConnectionState::hActive;
		}
		else
		{
			disconnect(generation);
		}

		exchange.completed = true;
		co_return result;
	}

	HttpClient::HttpClient(boost::asio::io_context& ioCtx, std::string host, uint16_t port, HttpClientOptions options)
		: impl_(std::make_shared<Impl>(ioCtx, std::move(host), port, std::move(options)))
	{
	}

	HttpClient::~HttpClient()
	{
		impl_->scheduleStop(true);
	}

	bool HttpClient::isAlive() const noexcept
	{
		return !impl_->stopping_.load(std::memory_order_acquire)
			   && impl_->liveGeneration_.load(std::memory_order_acquire)
					  == impl_->generation_.load(std::memory_order_acquire);
	}

	std::string_view HttpClient::key() const noexcept
	{
		return impl_->key_;
	}

	void HttpClient::close()
	{
		impl_->scheduleStop(false);
	}

	Awaitable<HttpClientResult> HttpClient::request(HttpMethod method,
													std::string_view target,
													std::string_view body,
													const HeaderMap& headers)
	{
		auto impl = impl_;
		const auto deadline = makeDeadline(impl->options_.requestTimeout);
		OwnedRequest owned {method,
							std::string(target),
							std::string(body),
							headers,
							impl->options_.requestTimeout,
							deadline,
							OwnedRequest::PendingTicket(impl)};
		const auto generation = impl->generation_.load(std::memory_order_acquire);
		return requestOwned(std::move(impl), std::move(owned), generation);
	}

	Awaitable<HttpClientResult> HttpClient::requestUntil(HttpMethod method,
														 std::string_view target,
														 std::string_view body,
														 const HeaderMap& headers,
														 std::chrono::steady_clock::time_point deadline)
	{
		auto impl = impl_;
		OwnedRequest owned {method,
							std::string(target),
							std::string(body),
							headers,
							impl->options_.requestTimeout,
							deadline,
							OwnedRequest::PendingTicket(impl)};
		const auto generation = impl->generation_.load(std::memory_order_acquire);
		return requestOwned(std::move(impl), std::move(owned), generation);
	}

	Awaitable<HttpClientResult> HttpClient::get(std::string_view target, const HeaderMap& headers)
	{
		return request(HttpMethod::hGet, target, {}, headers);
	}

	Awaitable<HttpClientResult> HttpClient::post(std::string_view target,
												 std::string_view body,
												 std::string_view contentType)
	{
		const auto deadline = makeDeadline(impl_->options_.requestTimeout);
		HeaderMap headers;
		headers.set("Content-Type", contentType);
		return requestUntil(HttpMethod::hPost, target, body, headers, deadline);
	}

	Awaitable<HttpClientResult> HttpClient::requestOwned(std::shared_ptr<Impl> impl,
														 OwnedRequest request,
														 uint64_t generation)
	{
		HttpClientResult rejected;
		rejected.requestDisposition = HttpRequestDisposition::hNotSent;
		const auto deadline = request.deadline.value_or(std::chrono::steady_clock::time_point::max());
		if (!impl->current(generation))
		{
			rejected.error = Impl::cancelled();
			co_return rejected;
		}
		if (std::chrono::steady_clock::now() >= deadline)
		{
			rejected.error = Impl::requestTimedOut();
			co_return rejected;
		}
		const auto mode = impl->options_.protocolMode;
		if (mode != HttpProtocolMode::hHttp1Only && mode != HttpProtocolMode::hNegotiateHttp2
			&& mode != HttpProtocolMode::hHttp2Only)
		{
			rejected.error = {ErrorCode::hInvalidConfiguration, "无效的 HTTP 协议协商模式"};
			co_return rejected;
		}
		if (mode != HttpProtocolMode::hHttp1Only)
		{
#ifdef HICAL_HAS_HTTP2
			if (!impl->options_.tlsContext)
			{
				rejected.error = {ErrorCode::hInvalidConfiguration, "HTTP/2 客户端必须配置 TLS 上下文"};
				co_return rejected;
			}
			if (const auto error = impl->options_.http2.validationError(); !error.empty())
			{
				rejected.error = {ErrorCode::hInvalidConfiguration, std::string(error)};
				co_return rejected;
			}
#else
			rejected.error = {ErrorCode::hProtocolUnavailable, "HTTP/2 客户端模块未启用"};
			co_return rejected;
#endif
		}
		try
		{
			for (;;)
			{
				auto error = co_await impl->ensureConnected(generation, deadline);
				rejected.protocol = impl->protocol_;
				if (error)
				{
					rejected.error = std::move(error);
					co_return rejected;
				}
#ifdef HICAL_HAS_HTTP2
				if (impl->protocol_ == HttpProtocol::hHttp2)
				{
					auto session = impl->http2Session_;
					if (!impl->current(generation) || !session)
					{
						rejected.error = Impl::cancelled();
						co_return rejected;
					}
					detail::Http2ClientSession::Request h2;
					h2.method = request.method;
					h2.authority = impl->host_;
					if (h2.authority.find(':') != std::string::npos && !h2.authority.starts_with('['))
					{
						h2.authority = "[" + h2.authority + "]";
					}
					constexpr uint16_t kDefaultHttpsPort = 443;
					if (impl->port_ != kDefaultHttpsPort)
					{
						h2.authority += ":" + std::to_string(impl->port_);
					}
					h2.target = std::move(request.target);
					h2.body = std::move(request.body);
					h2.headers = std::move(request.headers);
					h2.deadline = deadline;
					// 外层协程帧持有请求、待处理计数和 Impl，直到完整响应返回。
					co_return co_await session->request(std::move(h2));
				}
#endif
				NetworkError gateError;
				auto lease = co_await impl->acquireGate(generation, deadline, &gateError);
				if (!lease)
				{
					rejected.error = gateError ? std::move(gateError) : Impl::cancelled();
					co_return rejected;
				}
				if (!impl->active(generation))
				{
					continue;
				}
				// await_transform 可能在进入 requestOn 前因取消抛出；开始交换后不能再认定请求未发送。
				rejected.requestDisposition = HttpRequestDisposition::hPossiblyProcessed;
				if (impl->options_.tlsContext)
				{
					co_return co_await impl->requestOn<Impl::TlsSocket>(std::move(request), generation);
				}
				co_return co_await impl->requestOn<Impl::PlainSocket>(std::move(request), generation);
			}
		}
		catch (const boost::system::system_error& exception)
		{
			rejected.error = exception.code() == boost::asio::error::operation_aborted
								 ? Impl::cancelled()
								 : toNetworkError(exception.code());
			co_return rejected;
		}
	}
} // namespace hical
