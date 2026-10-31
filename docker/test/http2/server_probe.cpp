/**
 * @file server_probe.cpp
 * @brief HTTP/2 互操作服务端，通过公开接口启动并限时退出
 */

#include "InteropCommon.h"
#include "core/GzipCompression.h"
#include "core/HttpServer.h"
#include "core/MetaJson.h"
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <algorithm>
#include <functional>

namespace
{
	struct JsonPayload
	{
		std::string message {"hello"};
		int answer {42};
		HICAL_JSON(JsonPayload, message, answer)
	};

	struct ServerConfig
	{
		std::filesystem::path ready;
		std::filesystem::path stop;
		std::filesystem::path file;
		std::string cert;
		std::string key;
		std::string mode;
		bool tls {true};
		int lifetimeSeconds {180};
		int slowMilliseconds {50};
		size_t gzipBodyBytes {0};
	};

	/**
	 * @brief 读取并校验服务端参数。
	 */
	ServerConfig configuration(const interop::Arguments& args)
	{
		args.allow({"--ready",
					"--stop-file",
					"--file",
					"--cert",
					"--key",
					"--mode",
					"--tls",
					"--lifetime-seconds",
					"--slow-ms",
					"--gzip-body-bytes"});
		ServerConfig result;
		result.ready = args.require("--ready");
		result.stop = args.require("--stop-file");
		result.file = args.require("--file");
		result.cert = args.get("--cert");
		result.key = args.get("--key");
		result.mode = args.get("--mode", "h1");
		const auto tls = args.get("--tls", "on");
		if ((result.mode != "h1" && result.mode != "h2") || (tls != "on" && tls != "off"))
		{
			throw std::invalid_argument("--mode 只能为 h1/h2，--tls 只能为 on/off");
		}
		result.tls = tls == "on";
		if ((result.tls && (result.cert.empty() || result.key.empty())) || (!result.tls && result.mode == "h2"))
		{
			throw std::invalid_argument("TLS 必须提供证书和私钥；H2 必须启用 TLS");
		}
		result.lifetimeSeconds = args.number("--lifetime-seconds", 180, 5, 1800);
		result.slowMilliseconds = args.number("--slow-ms", 50, 1, 1000);
		result.gzipBodyBytes =
			static_cast<size_t>(args.number("--gzip-body-bytes", 0, 0, static_cast<int>(interop::kMaxFixtureBytes)));
		return result;
	}

	/**
	 * @brief 注册 Hello 和三类 JSON 路由，H1、H2 共用同一套处理函数。
	 */
	void registerBasicRoutes(hical::HttpServer& server)
	{
		auto hello = [](const hical::HttpRequest&)
		{
			return hical::HttpResponse::ok(std::string(interop::kHello));
		};
		server.router().get("/", hello);
		server.router().post("/", hello);
		server.router().get("/hello", hello);
		server.router().get("/json-dom",
							[](const hical::HttpRequest&)
							{
								return hical::HttpResponse::json({{"message", "hello"}, {"answer", 42}});
							});
		server.router().get("/json-dto",
							[](const hical::HttpRequest&)
							{
								return hical::HttpResponse::jsonFrom(JsonPayload {});
							});
		server.router().get(
			"/json-writer",
			[](const hical::HttpRequest&)
			{
				hical::HttpResponse response;
				{
					// 先析构 writer，再返回并移动 response。
					auto writer = response.jsonWriter();
					writer.beginObject().key("message").value("hello").key("answer").value(42).endObject();
				}
				return response;
			});
		server.router().post("/echo",
							 [](const hical::HttpRequest& request)
							 {
								 return hical::HttpResponse::ok(request.body(), "application/octet-stream");
							 });
	}

	/**
	 * @brief 注册文件、范围、尾字段、gzip 和慢处理路由。
	 */
	void registerPayloadRoutes(hical::HttpServer& server, const ServerConfig& config)
	{
		const auto fileSize = static_cast<int64_t>(std::filesystem::file_size(config.file));
		if (fileSize < 87718 || static_cast<uint64_t>(fileSize) > interop::kMaxFixtureBytes)
		{
			throw std::invalid_argument("文件夹具必须在 87718 字节到 16 MiB 之间");
		}
		server.router().get("/file.bin",
							[path = config.file, fileSize](const hical::HttpRequest&)
							{
								hical::HttpResponse response;
								response.setFileBody(path, 0, fileSize, "application/octet-stream");
								return response;
							});
		server.router().get("/range.bin",
							[path = config.file, fileSize](const hical::HttpRequest&)
							{
								constexpr int64_t kOffset = 717;
								constexpr int64_t kLength = 87001;
								hical::HttpResponse response;
								response.setStatus(hical::HttpStatusCode::hPartialContent);
								response.setFileBody(path, kOffset, kLength, "application/octet-stream");
								response.setHeader("Content-Range", "bytes 717-87717/" + std::to_string(fileSize));
								return response;
							});
		server.router().get("/trailer",
							[](const hical::HttpRequest&)
							{
								auto response = hical::HttpResponse::ok(std::string(interop::kHello));
#ifdef HICAL_HAS_HTTP2
								response.setTrailer("x-interop-checksum", interop::sha256(interop::kHello));
#endif
								return response;
							});
		auto gzipBody = interop::readBinary(config.file);
		if (config.gzipBodyBytes != 0 && gzipBody.size() > config.gzipBodyBytes)
		{
			gzipBody.resize(config.gzipBodyBytes);
		}
		server.router().get("/gzip",
							[gzipBody](const hical::HttpRequest&)
							{
								return hical::HttpResponse::ok(gzipBody, "text/plain");
							});
		server.router().get(
			"/slow",
			[delay = config.slowMilliseconds](const hical::HttpRequest&) -> hical::Awaitable<hical::HttpResponse>
			{
				co_await hical::sleep(static_cast<double>(delay) / 1000.0);
				co_return hical::HttpResponse::ok(std::string(interop::kHello));
			});
		hical::GzipCompressionOptions gzip;
		gzip.asyncThreads = 0;
		server.use(hical::makeGzipCompressionMiddleware(gzip));
	}

	/**
	 * @brief 在服务器所属事件循环中写入就绪文件。
	 */
	void publishReady(hical::HttpServer& server, const ServerConfig& config)
	{
		if (!server.isRunning() || server.port() == 0)
		{
			throw std::runtime_error("事件循环已运行但监听端口尚未发布");
		}
		const auto fileBody = interop::readBinary(config.file);
		const auto gzipBytes =
			config.gzipBodyBytes == 0 ? fileBody.size() : std::min(fileBody.size(), config.gzipBodyBytes);
		boost::json::object ready {
			{"schema", 1},
			{"ready", true},
			{"port", server.port()},
			{"host", "127.0.0.1"},
			{"mode", config.mode},
			{"tls", config.tls},
			{"io_callback_executed", true},
			{"file_sha256", interop::sha256(fileBody)},
			{"gzip_body_bytes", gzipBytes},
			{"gzip_body_sha256", interop::sha256(std::string_view(fileBody).substr(0, gzipBytes))}};
#ifdef HICAL_HAS_HTTP2
		ready["compiled_http2"] = true;
#else
		ready["compiled_http2"] = false;
#endif
		interop::writeJson(config.ready, ready);
	}

	/**
	 * @brief 启动服务器并限时运行，事件循环启动后才发布就绪文件。
	 */
	void run(const ServerConfig& config)
	{
		interop::ProcessDeadline watchdog(std::chrono::seconds(config.lifetimeSeconds + 10));
		hical::HttpServer server(0, 1);
		server.listen("127.0.0.1", 0);
		server.setMaxBodySize(1024 * 1024);
		server.setIdleTimeout(10);
		if (config.tls)
		{
			server.enableSsl(config.cert, config.key);
		}
		if (config.mode == "h2")
		{
#ifdef HICAL_HAS_HTTP2
			server.enableHttp2();
#else
			throw std::invalid_argument("此服务端未编译 HTTP/2，不能把 H1 基线冒充 H2");
#endif
		}
		registerBasicRoutes(server);
		registerPayloadRoutes(server, config);
		boost::asio::steady_timer lifetime(server.ioContext());
		boost::asio::steady_timer control(server.ioContext());
		std::function<void()> checkStop;
		checkStop = [&]()
		{
			if (std::filesystem::exists(config.stop))
			{
				lifetime.cancel();
				server.stop();
				return;
			}
			control.expires_after(std::chrono::milliseconds(50));
			control.async_wait(
				[&](const boost::system::error_code& error)
				{
					if (!error)
					{
						checkStop();
					}
				});
		};
		boost::asio::post(server.ioContext(),
						  [&]()
						  {
							  publishReady(server, config);
							  lifetime.expires_after(std::chrono::seconds(config.lifetimeSeconds));
							  lifetime.async_wait(
								  [&](const boost::system::error_code& error)
								  {
									  if (!error)
									  {
										  control.cancel();
										  server.stop();
									  }
								  });
							  checkStop();
						  });
		server.start();
	}
} // namespace

/**
 * @brief 运行互操作服务端。
 * @param argc 参数数量
 * @param argv 命令行参数
 * @return 正常停机为 0，配置或运行错误为 1，看门狗超时为 124
 */
int main(int argc, char** argv)
{
	try
	{
		run(configuration(interop::Arguments(argc, argv)));
		return 0;
	}
	catch (const std::exception& error)
	{
		HICAL_LOG_ERROR("服务端探针失败：{}", error.what());
		return 1;
	}
}
