/**
 * @file http2_client.cpp
 * @brief 使用可信证书并发发起有限请求，演示单客户端和连接池的正常收尾
 */

#include "asio/HttpClient.h"
#include "asio/HttpClientPool.h"
#include "core/Log.h"

#include <boost/asio/steady_timer.hpp>

#include <charconv>
#include <chrono>
#include <cstdint>
#include <exception>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace
{
	constexpr unsigned int kMaximumPort = 65535;
	constexpr size_t kRequestCount = 2;
	constexpr auto kConnectTimeout = std::chrono::seconds {2};
	constexpr auto kRequestTimeout = std::chrono::seconds {5};
	constexpr auto kRunTimeout = std::chrono::seconds {8};
	constexpr std::string_view kEchoBody = R"({"message":"hello from the client"})";

	/**
	 * @brief 读取有效的目标端口。
	 * @param text 十进制端口
	 * @return 1 到 65535 之间的端口
	 */
	uint16_t parsePort(std::string_view text)
	{
		unsigned int value = 0;
		const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
		if (error != std::errc {} || end != text.data() + text.size() || value == 0 || value > kMaximumPort)
		{
			throw std::invalid_argument("端口必须在 1 到 65535 之间");
		}
		return static_cast<uint16_t>(value);
	}

	/**
	 * @brief 读取客户端协议策略。
	 * @param text h1、auto 或 h2
	 * @return 对应的协议策略
	 */
	hical::HttpProtocolMode parseMode(std::string_view text)
	{
		if (text == "h1")
		{
			return hical::HttpProtocolMode::hHttp1Only;
		}
		if (text == "auto")
		{
			return hical::HttpProtocolMode::hNegotiateHttp2;
		}
		if (text == "h2")
		{
			return hical::HttpProtocolMode::hHttp2Only;
		}
		throw std::invalid_argument("协议必须是 h1、auto 或 h2");
	}

	/**
	 * @brief 检查完整结果；HTTP 状态错误与传输错误分别记录。
	 * @param result 完整请求结果
	 * @param mode 本次协议策略
	 * @param expectedBody 预期响应体
	 * @return 响应成功、协议符合策略且 body 一致时为 true
	 */
	bool checkResult(const hical::HttpClientResult& result, hical::HttpProtocolMode mode, std::string_view expectedBody)
	{
		if (result.isNetworkError())
		{
			HICAL_LOG_ERROR("传输失败: {} ({})", hical::errorCodeToString(result.error.code), result.error.message);
			return false;
		}
		const bool isHttp2 = result.protocol == hical::HttpProtocol::hHttp2;
		const bool protocolMatches = mode == hical::HttpProtocolMode::hNegotiateHttp2
									 || isHttp2 == (mode == hical::HttpProtocolMode::hHttp2Only);
		HICAL_LOG_INFO("协议={}，状态={}，body={} 字节，尾字段={}",
					   isHttp2 ? "h2" : "http/1.1",
					   result.status,
					   result.body.size(),
					   result.trailers.size());
		return result.ok() && protocolMatches && result.body == expectedBody;
	}

	/**
	 * @brief 在所属事件循环关闭当前使用的客户端资源。
	 * @param client 单连接客户端；池模式下为空
	 * @param pool 连接池；单连接模式下为空
	 */
	void closeResources(hical::HttpClient* client, hical::HttpClientPool* pool)
	{
		if (client)
		{
			client->close();
		}
		if (pool)
		{
			pool->shutdown();
		}
	}

	/**
	 * @brief 在同一事件循环提交两个请求，关闭资源后让事件循环自然返回。
	 * @param host 连接和证书校验使用的目标主机
	 * @param port 目标端口
	 * @param options 已配置 CA、协议和期限的选项
	 * @param usePool 为 true 时使用连接池的高层调度入口
	 * @return 全部请求验证成功为 0，否则为 1
	 */
	int runRequests(const std::string& host, uint16_t port, const hical::HttpClientOptions& options, bool usePool)
	{
		boost::asio::io_context io;
		int exitCode = 0;
		{
			std::unique_ptr<hical::HttpClient> client;
			std::unique_ptr<hical::HttpClientPool> pool;
			if (usePool)
			{
				pool = std::make_unique<hical::HttpClientPool>(io);
			}
			else
			{
				client = std::make_unique<hical::HttpClient>(io, host, port, options);
			}
			boost::asio::steady_timer watchdog(io, kRunTimeout);
			watchdog.async_wait(
				[&](const boost::system::error_code& error)
				{
					if (!error)
					{
						HICAL_LOG_ERROR("示例运行超时，取消尚未完成的请求");
						exitCode = 1;
						closeResources(client.get(), pool.get());
					}
				});
			size_t remaining = kRequestCount;
			const auto submit = [&](hical::Awaitable<hical::HttpClientResult> operation, std::string expectedBody)
			{
				hical::coSpawn(io,
							   std::move(operation),
							   [&, expectedBody = std::move(expectedBody)](std::exception_ptr exception,
																		   hical::HttpClientResult result)
							   {
								   if (exception)
								   {
									   HICAL_LOG_ERROR("请求协程异常退出");
									   exitCode = 1;
								   }
								   else if (!checkResult(result, options.protocolMode, expectedBody))
								   {
									   HICAL_LOG_ERROR("响应内容、状态或协议不符合预期");
									   exitCode = 1;
								   }
								   if (--remaining == 0)
								   {
									   watchdog.cancel();
									   closeResources(client.get(), pool.get());
								   }
							   });
			};
			submit(usePool ? pool->get(host, port, "/hello", {}, options) : client->get("/hello"), "Hello, HTTP/2!\n");
			submit(usePool ? pool->post(host, port, "/echo", kEchoBody, "application/json", options)
						   : client->post("/echo", kEchoBody),
				   std::string(kEchoBody));
			io.run();
		}
		// 客户端析构还会投递清理回调，io 必须继续运行，不能提前析构或调用 stop()。
		io.restart();
		io.run();
		return exitCode;
	}
} // namespace

/**
 * @brief 连接 http2_server 示例，验证 GET 与 POST 的完整响应。
 * @param argc 参数数量
 * @param argv 主机、端口、CA 文件及可选协议和驱动方式
 * @return 验证成功为 0，参数或请求失败为 1
 */
int main(int argc, char* argv[])
{
	try
	{
		if (argc < 4 || argc > 6)
		{
			HICAL_LOG_ERROR("用法: http2_client <主机> <端口> <CA.pem> [h1|auto|h2=auto] [shared|pool=shared]");
			return 1;
		}
		const std::string_view driver = argc >= 6 ? argv[5] : "shared";
		if (driver != "shared" && driver != "pool")
		{
			throw std::invalid_argument("驱动方式必须是 shared 或 pool");
		}
		hical::HttpClientOptions options;
		options.tlsContext = std::make_shared<hical::SslContext>(boost::asio::ssl::context::tls_client);
		options.tlsContext->loadCaCertificate(argv[3]);
		options.verifyPeer = true;
		options.protocolMode = parseMode(argc >= 5 ? argv[4] : "auto");
		options.connectTimeout = kConnectTimeout;
		options.requestTimeout = kRequestTimeout;
		options.http2.maxConnectionsPerOrigin = 1;
		options.http2.maxQueuedRequestsPerOrigin = 8;
		return runRequests(argv[1], parsePort(argv[2]), options, driver == "pool");
	}
	catch (const std::exception& error)
	{
		HICAL_LOG_ERROR("HTTP/2 客户端示例失败: {}", error.what());
		return 1;
	}
}
