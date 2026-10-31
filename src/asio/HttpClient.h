/**
 * @file HttpClient.h
 * @brief 协程 HTTP/1.1 与可选 HTTP/2 客户端接口及结果结构
 */

#pragma once

#include "../core/Coroutine.h"
#include "../core/Error.h"
#include "../core/HeaderMap.h"
#include "../core/Http2Options.h"
#include "../core/HttpTypes.h"
#include "../core/SslContext.h"
#include <boost/asio/io_context.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace hical
{

	/**
	 * @brief 一次 HTTP 请求的结果
	 * 连接失败、超时和 TLS 错误通过 error 返回；404/500 等 HTTP 状态通过 status 返回。
	 */
	struct HttpClientResult
	{
		/** 网络层错误码；无网络错误时为空 */
		NetworkError error;

		/** HTTP 状态码，0 表示压根没收到响应 */
		int status {0};

		/** HTTP/1 状态行的原因短语（如 "OK"）；HTTP/2 为空 */
		std::string statusText;

		/** 响应体（已按 Content-Length / chunked 解码后的内容） */
		std::string body;

		/** 响应头 */
		HeaderMap headers;

		/** 实际协商的协议；未协商时默认 H1，协商成功后即记录，与请求是否发送分开 */
		HttpProtocol protocol {HttpProtocol::hHttp1};
		/** END_STREAM 前收到的独立尾字段 */
		HeaderMap trailers;
		/** 只有传输层证据才能收紧分类，status==0 不等于请求未处理 */
		HttpRequestDisposition requestDisposition {HttpRequestDisposition::hPossiblyProcessed};

		/**
		 * @brief 请求是否成功
		 * @return true 表示无网络错误且拿到 2xx 响应
		 */
		[[nodiscard]] bool ok() const noexcept
		{
			return error.ok() && status >= 200 && status < 300;
		}

		/**
		 * @brief 是否发生传输错误
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
	 * 建连和请求默认都有超时；对应字段设为 0 或负数可关闭该项超时。
	 */
	struct HttpClientOptions
	{
		/// 建连超时默认值：DNS 解析、TCP 连接和 TLS 握手共用此限时
		static constexpr auto kDefaultConnectTimeout = std::chrono::seconds {10};

		/// 单次请求超时默认值：从提交到完整响应的总限时
		static constexpr auto kDefaultRequestTimeout = std::chrono::seconds {30};

		/** TLS 上下文（用 ssl::context::tls_client 构造），非空即启用 HTTPS */
		std::shared_ptr<SslContext> tlsContext;

		/** SNI 路由主机名，留空则使用目标 host；覆盖此值不改变证书身份校验的目标 */
		std::string sniHost;

		/** 是否校验对端证书链及构造时的目标 host（DNS 名或 IP SAN） */
		bool verifyPeer {true};

		/** resolve、connect 和 TLS 握手的总时限；非正值表示不设超时 */
		std::chrono::steady_clock::duration connectTimeout {kDefaultConnectTimeout};

		/** 从普通入口提交起覆盖建连等待、排队、写请求和完整响应；非正值不设请求时限 */
		std::chrono::steady_clock::duration requestTimeout {kDefaultRequestTimeout};

		/** 默认只使用 H1；H2 模块关闭时其他模式明确返回协议不可用 */
		HttpProtocolMode protocolMode {HttpProtocolMode::hHttp1Only};
		/** 仅 H2 使用的流、响应聚合和队列预算；H1 不创建对应资源 */
		Http2Options http2;
	};

	/**
	 * @brief 协程 HTTP 客户端，默认仅使用 HTTP/1.1
	 * 一个实例对应一条到 host:port 的连接，请求之间尽量复用（keep-alive）。
	 * 所有异步操作都跑在构造时传入的 io_context 上，实例本身不跨线程共享。
	 * request/get/post 返回前复制参数并持有内部状态；客户端对象析构会取消旧请求，
	 * 已保存的 awaitable 仍可安全启动并返回取消错误。io_context 必须覆盖全部异步操作。
	 * 同一个实例的 H1 请求完整串行执行，多个首请求共享一次建连。
	 * 显式启用并协商 H2 后，一条 TLS 连接使用有界并发流，响应完整收齐后返回。
	 * H1 响应使用 picohttpparser 解析，H2 由独立会话处理。
	 * 明文和 TLS 的 H1 请求共用实现，tlsContext 非空时使用 ssl::stream。
	 */
	class HttpClient
	{
	public:
		/**
		 * @brief 构造客户端
		 * @param ioCtx 事件循环，生命周期必须覆盖本对象及全部未完成 awaitable
		 * @param host 目标主机（域名或 IP）
		 * @param port 目标端口
		 * @param options 客户端配置；tlsContext 非空则走 HTTPS
		 */
		HttpClient(boost::asio::io_context& ioCtx, std::string host, uint16_t port, HttpClientOptions options = {});

		/**
		 * @brief 取消旧请求并向所属事件循环投递清理；事件循环须存活至清理完成
		 */
		~HttpClient();

		// 一个实例持有一条连接，不允许拷贝或移动。
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
		 * @return 请求结果；网络错误在 result.error 中
		 */
		Awaitable<HttpClientResult> get(std::string_view target, const HeaderMap& headers = {});

		/**
		 * @brief 便捷 POST
		 * @param target 请求目标
		 * @param body 请求体
		 * @param contentType Content-Type，默认 application/json
		 * @return 请求结果；网络错误在 result.error 中
		 */
		Awaitable<HttpClientResult> post(std::string_view target,
										 std::string_view body,
										 std::string_view contentType = "application/json");

		/**
		 * @brief 连接当前是否可用（可被复用）
		 * @return true 表示连接已建立且没关
		 */
		[[nodiscard]] bool isAlive() const noexcept;

		/**
		 * @brief 取消当前代次并关闭连接；随后新请求可以重新建连
		 * 清理投递到所属事件循环，调用线程不直接操作 socket。
		 */
		void close();

		/**
		 * @brief 获取目标地址标识
		 * @return "host:port"；不包含 TLS 身份和协议配置，不能单独作为池的复用依据
		 */
		[[nodiscard]] std::string_view key() const noexcept;

	private:
		friend class HttpClientPool;
		struct Impl;
		struct OwnedRequest;

		/** 所属 loop 的容量快照；已分配但尚未发送的 H2 流也计入 admitted。 */
		struct PoolCapacity
		{
			bool connected {false};
			bool connecting {false};
			HttpProtocol protocol {HttpProtocol::hHttp1};
			bool settingsSeen {false};
			bool accepting {false};
			uint32_t localLimit {0};
			uint32_t peerLimit {0};
			size_t admitted {0};
			size_t available {0};
		};

		/// 仅由连接池在所属 loop 查询，不创建协议对象；未连接与 H1 的 H2 字段均为零。
		[[nodiscard]] PoolCapacity poolCapacity() const noexcept;
		/// 在所属 loop 设置通知；回调不能抛异常，只能 post 调度，并通过弱引用访问池。
		void setPoolCapacityChanged(std::function<void()> callback);
		/// 包含已返回但尚未启动的 awaitable，供连接池判断用户请求是否仍占有连接。
		[[nodiscard]] bool hasPendingRequests() const noexcept;
		static Awaitable<HttpClientResult> requestOwned(std::shared_ptr<Impl> impl,
														OwnedRequest request,
														uint64_t generation);
		/// 连接池可传入本次调用的绝对请求期限，不复用旧连接的请求时限配置。
		Awaitable<HttpClientResult> requestUntil(HttpMethod method,
												 std::string_view target,
												 std::string_view body,
												 const HeaderMap& headers,
												 std::chrono::steady_clock::time_point deadline);
		std::shared_ptr<Impl> impl_;
	};

} // namespace hical
