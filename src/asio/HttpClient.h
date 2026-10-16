/**
 * @file HttpClient.h
 * @brief 协程 HTTP/1.1 客户端接口与结果结构
 */

#pragma once

#include "../core/Coroutine.h"
#include "../core/Error.h"
#include "../core/HeaderMap.h"
#include "../core/HttpTypes.h"
#include "../core/SslContext.h"
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace hical
{

	/**
	 * @brief 一次 HTTP 请求的结果
	 * 网络层的失败（连不上、超时、TLS 握手挂掉）走 error 字段返回，不抛异常——
	 * 对客户端来说对端不可达属于常规情况，硬用异常打断控制流只会让调用方难受。
	 * 注意 HTTP 层的错误（404/500）不算网络错误：那种情况 error 是空的，
	 * 得看 status。两条轴分开判断，别混。
	 */
	struct HttpClientResult
	{
		/** 网络层错误码；无网络错误时为空 */
		NetworkError error;

		/** HTTP 状态码，0 表示压根没收到响应 */
		int status {0};

		/** 状态行里的原因短语（如 "OK"、"Not Found"） */
		std::string statusText;

		/** 响应体（已按 Content-Length / chunked 解码后的内容） */
		std::string body;

		/** 响应头 */
		HeaderMap headers;

		/**
		 * @brief 请求是否成功
		 * @return true 表示无网络错误且拿到 2xx 响应
		 */
		[[nodiscard]] bool ok() const noexcept
		{
			return error.ok() && status >= 200 && status < 300;
		}

		/**
		 * @brief 是否栽在网络层
		 * @return true 表示连接失败 / 超时 / TLS 等传输层错误
		 */
		[[nodiscard]] bool isNetworkError() const noexcept
		{
			return !error.ok();
		}

		/**
		 * @brief 等价于 ok()，方便 if (result) 这样写
		 */
		explicit operator bool() const noexcept
		{
			return ok();
		}
	};

	/**
	 * @brief 客户端配置
	 * 不设超时的链路最怕对端接了连接却永远不回，所以 connect / request 两档超时
	 * 都给了非零默认值。想关掉某档超时把对应字段设成 0 或负数即可。
	 */
	struct HttpClientOptions
	{
		/// 建连超时默认值：DNS 解析 + TCP 连接一共限这么多
		static constexpr auto kDefaultConnectTimeout = std::chrono::seconds {10};

		/// 单次请求超时默认值：写完请求到读完整段响应的总限时
		static constexpr auto kDefaultRequestTimeout = std::chrono::seconds {30};

		/** TLS 上下文（用 ssl::context::tls_client 构造），非空即启用 HTTPS */
		std::shared_ptr<SslContext> tlsContext;

		/** SNI 主机名，留空则回落到构造时传入的 host */
		std::string sniHost;

		/** 是否校验对端证书 */
		bool verifyPeer {true};

		/** resolve + connect 的总时限；非正值表示不设超时 */
		std::chrono::steady_clock::duration connectTimeout {kDefaultConnectTimeout};

		/** 单次请求（写请求 + 读完整响应）的时限；非正值表示不设超时 */
		std::chrono::steady_clock::duration requestTimeout {kDefaultRequestTimeout};
	};

	/**
	 * @brief 协程 HTTP/1.1 客户端
	 * 一个实例对应一条到 host:port 的连接，请求之间尽量复用（keep-alive）。
	 * 所有异步操作都跑在构造时传入的 io_context 上；实例本身不跨线程共享，
	 * 连接池（HttpClientPool）按 key() 分桶管理一批实例。
	 * 请求协程在挂起点之后要用 this 上的 socket / 缓冲，所以调用方必须保证
	 * 本对象活到 awaitable 跑完为止——池子持有实例、协程期间不释放即可满足。
	 * 解析响应复用服务端同款 picohttpparser，但接口层不暴露任何解析器类型，
	 * 使用者不用关心底层拿什么解析。
	 * 明文和 TLS 两条路走同一套建连 / 请求 / 读响应的代码：底层传输是个二选一的
	 * variant，tlsContext 非空就是 ssl::stream，差别用 if constexpr 在编译期分掉。
	 */
	class HttpClient
	{
	public:
		/**
		 * @brief 构造客户端
		 * @param ioCtx 事件循环，生命周期必须长于本对象
		 * @param host 目标主机（域名或 IP）
		 * @param port 目标端口
		 * @param options 客户端配置；tlsContext 非空则走 HTTPS
		 */
		HttpClient(boost::asio::io_context& ioCtx, std::string host, uint16_t port, HttpClientOptions options = {})
			: ioCtx_(ioCtx)
			, host_(std::move(host))
			, port_(port)
			, options_(std::move(options))
			, key_(makeKey(host_, port_))
			, transport_(makeTransport(ioCtx_, options_))
		{
		}

		~HttpClient() = default;

		// 独占一条连接，拷贝 / 移动都禁掉，省得连接状态被分叉
		HttpClient(const HttpClient&) = delete;
		HttpClient& operator=(const HttpClient&) = delete;
		HttpClient(HttpClient&&) = delete;
		HttpClient& operator=(HttpClient&&) = delete;

		/**
		 * @brief 发起一次请求
		 * @param method HTTP 方法
		 * @param target 请求目标，如 "/api/notice"
		 * @param body 请求体；为空则不带 body（GET 一般留空）
		 * @param headers 附加请求头；Host / Content-Length 由客户端补齐
		 * @return 请求结果；网络错误在 result.error 里，不抛异常
		 */
		Awaitable<HttpClientResult> request(HttpMethod method,
											std::string_view target,
											std::string_view body = {},
											const HeaderMap& headers = {});

		/**
		 * @brief 便捷 GET
		 * @param target 请求目标
		 * @param headers 附加请求头
		 */
		Awaitable<HttpClientResult> get(std::string_view target, const HeaderMap& headers = {});

		/**
		 * @brief 便捷 POST
		 * @param target 请求目标
		 * @param body 请求体
		 * @param contentType Content-Type，默认 application/json
		 */
		Awaitable<HttpClientResult> post(std::string_view target,
										 std::string_view body,
										 std::string_view contentType = "application/json");

		/**
		 * @brief 连接当前是否可用（可被复用）
		 * @return true 表示连接已建立且没关
		 */
		[[nodiscard]] bool isAlive() const noexcept
		{
			return state_ == ConnectionState::hActive;
		}

		/**
		 * @brief 主动关闭连接
		 */
		void close();

		/**
		 * @brief 连接分桶键
		 * @return "host:port"，连接池据此归并同类目标
		 */
		[[nodiscard]] std::string_view key() const noexcept
		{
			return key_;
		}

	private:
		/**
		 * @brief 连接状态机
		 */
		enum class ConnectionState : uint8_t
		{
			hIdle,       ///< 还没连过
			hConnecting, ///< 正在建连
			hActive,     ///< 连接可用
			hClosed      ///< 已关闭或出错
		};

		/// 拼 "host:port" 分桶键
		static std::string makeKey(const std::string& host, uint16_t port)
		{
			return host + ":" + std::to_string(port);
		}

		/// 明文传输：裸 tcp::socket
		using PlainSocket = boost::asio::ip::tcp::socket;

		/// 加密传输：ssl::stream 包着一条 tcp::socket
		using TlsSocket = boost::asio::ssl::stream<boost::asio::ip::tcp::socket>;

		/// 底层传输二选一：tlsContext 非空就是 TlsSocket，否则 PlainSocket
		using Transport = std::variant<PlainSocket, TlsSocket>;

		/// 按配置生成传输对象：tlsContext 非空就建 TLS 流，否则建裸 socket
		static Transport makeTransport(boost::asio::io_context& io, const HttpClientOptions& options);

		/// 把一次请求序列化成 HTTP/1.1 报文（请求行 + 头 + body）。
		/// requestOn() 里必须在首个 co_await 之前调它：入参是调用方的
		/// string_view / 引用，序列化进本地 string 才不会在挂起后悬空。
		std::string buildRequest(HttpMethod method,
								 std::string_view target,
								 std::string_view body,
								 const HeaderMap& headers) const;

		/// 从 socket 再读一段追加到 recvBuf_ 尾部，返回本次读到的字节数（出错时置 ec）。
		/// chunked 原地解码要用它：缓冲区不够就翻倍，顶到 kMaxRecvBuffer 还塞不下按 no_buffer_space 报错。
		/// deadline 是本次请求的绝对截止点，每次读都按剩余时间挂 cancel_after。
		template <typename SocketType>
		Awaitable<std::size_t> readIntoRecvBuf(SocketType& sock,
											   boost::system::error_code& ec,
											   std::chrono::steady_clock::time_point deadline);

		/// 请求-响应往返的实现体。明文和 TLS 各实例化一份，靠 if constexpr 在编译期
		/// 把建连 / 握手 / 关闭的差异分掉，请求序列化、读循环、body 读取是同一份代码。
		template <typename SocketType>
		Awaitable<HttpClientResult> requestOn(HttpMethod method,
											  std::string_view target,
											  std::string_view body,
											  const HeaderMap& headers);

		/// 确保连接已建立：按需 resolve + connect，TLS 再握手。失败时已经 close() 过
		/// （连接不可复用），成功时 state_ 置 hActive。TLS 每次重连都重建 ssl::stream，
		/// 规避 SSL 会话状态残留。
		template <typename SocketType>
		Awaitable<NetworkError> ensureConnected();

		/// 接收缓冲初始容量，装不下就翻倍
		static constexpr std::size_t kInitialRecvBuffer = 4096;

		/// 响应头命中前允许累积的最大字节数，超了就当对端不正常，直接断
		static constexpr std::size_t kMaxRecvBuffer = 64 * 1024;

		boost::asio::io_context& ioCtx_;
		std::string host_;
		uint16_t port_ {0};
		HttpClientOptions options_;
		std::string key_;
		ConnectionState state_ {ConnectionState::hIdle};

		/// 当前连接（明文或 TLS 二选一）；一个实例同时只跑一条请求，复用这个传输
		Transport transport_;

		/// 累积的接收字节。头解析完 [0, recvUsed_) 里剩的是 body 开头，
		/// 接着从这里读 body。读完 body 后残留的字节（chunked 尾 / 流水线残段）
		/// 若还有，就留给下一次复用连接时先解析。
		std::string recvBuf_;
		std::size_t recvUsed_ {0};
	};

} // namespace hical
