/**
 * @file http2_server.cpp
 * @brief 有限运行时间的 TLS HTTP/2 服务端，同时允许 HTTP/1.1 回退
 */

#include "core/HttpServer.h"
#include "core/Log.h"

#include <charconv>
#include <chrono>
#include <cstdint>
#include <exception>
#include <stdexcept>
#include <string>
#include <string_view>

namespace
{
	constexpr unsigned int kDefaultPort = 8443;
	constexpr unsigned int kMaximumPort = 65535;
	constexpr unsigned int kDefaultLifetimeSeconds = 30;
	constexpr unsigned int kMaximumLifetimeSeconds = 3600;
	constexpr size_t kKiB = 1024;
	constexpr size_t kMaximumConnections = 16;
	constexpr uint32_t kStreamsPerConnection = 8;
	constexpr size_t kMaximumApplicationTasks = 64;
	constexpr auto kSlowResponseDelay = std::chrono::milliseconds {50};

	/**
	 * @brief 读取范围内的正整数，输入无效时抛出异常。
	 * @param text 命令行参数
	 * @param maximum 允许的最大值
	 * @return 解析后的整数
	 */
	unsigned int positiveNumber(std::string_view text, unsigned int maximum)
	{
		unsigned int value = 0;
		const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
		if (error != std::errc {} || end != text.data() + text.size() || value == 0 || value > maximum)
		{
			throw std::invalid_argument("端口或运行秒数超出范围");
		}
		return value;
	}

	/**
	 * @brief 注册可同时通过 H1、H2 访问的路由。
	 * @param server 尚未启动的服务器
	 */
	void registerRoutes(hical::HttpServer& server)
	{
		server.router().get("/hello",
							[](const hical::HttpRequest& request) -> hical::HttpResponse
							{
								auto response = hical::HttpResponse::ok("Hello, HTTP/2!\n");
								const bool isHttp2 = request.protocol() == hical::HttpProtocol::hHttp2;
								response.setHeader("x-http-protocol", isHttp2 ? "h2" : "http/1.1");
								if (isHttp2)
								{
									response.setTrailer("example-trailer", "finished");
								}
								return response;
							});
		server.router().post("/echo",
							 [](const hical::HttpRequest& request) -> hical::HttpResponse
							 {
								 return hical::HttpResponse::ok(request.body());
							 });
		server.router().get("/slow",
							[](const hical::HttpRequest&) -> hical::Awaitable<hical::HttpResponse>
							{
								co_await hical::sleep(kSlowResponseDelay);
								co_return hical::HttpResponse::ok("slow response completed\n");
							});
	}

	/**
	 * @brief 为演示配置有限的流、任务和缓冲预算。
	 * @return 可直接交给 enableHttp2() 的配置
	 */
	hical::Http2Options http2Options()
	{
		hical::Http2Options options;
		options.maxConcurrentStreams = kStreamsPerConnection;
		options.maxApplicationTasks = kStreamsPerConnection;
		options.maxServerApplicationTasks = kMaximumApplicationTasks;
		options.maxBodyBytes = 64 * kKiB;
		options.maxConnectionReceiveBytes = 512 * kKiB;
		options.maxConnectionSendBytes = 512 * kKiB;
		options.maxOutputBatchBytes = 32 * kKiB;
		options.maxFilePrefetchBytes = 32 * kKiB;
		return options;
	}
} // namespace

/**
 * @brief 启动回环 TLS 服务，到期后排空并退出。
 * @param argc 参数数量
 * @param argv 证书、私钥、可选端口和运行秒数
 * @return 正常退出为 0，配置或启动失败为 1
 */
int main(int argc, char* argv[])
{
	try
	{
		if (argc < 3 || argc > 5)
		{
			HICAL_LOG_ERROR("用法: http2_server <证书.pem> <私钥.pem> [端口=8443] [运行秒数=30]");
			return 1;
		}
		const auto port = argc >= 4 ? positiveNumber(argv[3], kMaximumPort) : kDefaultPort;
		const auto lifetime = argc >= 5 ? positiveNumber(argv[4], kMaximumLifetimeSeconds) : kDefaultLifetimeSeconds;
		hical::HttpServer server(static_cast<uint16_t>(port), 1);
		server.listen("127.0.0.1", static_cast<uint16_t>(port));
		server.setMaxConnections(kMaximumConnections);
		server.setShutdownTimeout(2.0);
		server.enableSsl(argv[1], argv[2]);
		server.enableHttp2(http2Options());
		registerRoutes(server);
		// 周期任务由服务器管理；第一次触发就开始停机，收尾时会取消后续等待。
		server.addPeriodicTask("example-lifetime",
							   std::chrono::seconds {lifetime},
							   [&server]() -> hical::Awaitable<void>
							   {
								   HICAL_LOG_INFO("示例运行时间已到，开始排空连接");
								   server.gracefulStop();
								   co_return;
							   });
		HICAL_LOG_INFO("监听 https://127.0.0.1:{}，{} 秒后排空；路由 /hello、/echo、/slow", port, lifetime);
		server.start();
		HICAL_LOG_INFO("服务器已完成收尾");
		return 0;
	}
	catch (const std::exception& error)
	{
		HICAL_LOG_ERROR("HTTP/2 示例启动失败: {}", error.what());
		return 1;
	}
}
