/**
 * @file client_probe.cpp
 * @brief HTTP/2 互操作客户端，使用公开客户端接口并校验响应
 */

#include "InteropCommon.h"
#include "asio/HttpClient.h"
#include "asio/HttpClientPool.h"
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/steady_timer.hpp>
#include <algorithm>
#include <exception>
#include <memory>
#include <vector>

namespace
{
	struct ClientConfig
	{
		std::string host;
		std::string target;
		std::string mode;
		std::string driver;
		std::string expectedProtocol;
		std::string runId;
		std::string expected;
		std::string body;
		std::filesystem::path report;
		hical::HttpMethod method {hical::HttpMethod::hGet};
		hical::HttpClientOptions options;
		uint16_t port {0};
		int requests {8};
		int concurrency {8};
		int expectedStatus {200};
		int timeoutMilliseconds {20000};
	};

	/**
	 * @brief 从参数中读取 TLS 信任和协议配置。
	 */
	void configureTransport(const interop::Arguments& args, ClientConfig& result)
	{
		const auto tls = args.get("--tls", "on");
		if (tls != "on" && tls != "off")
		{
			throw std::invalid_argument("--tls 只能为 on/off");
		}
		if (tls == "on")
		{
			result.options.tlsContext = std::make_shared<hical::SslContext>(boost::asio::ssl::context::tls_client);
			result.options.tlsContext->loadCaCertificate(args.require("--ca"));
			result.options.verifyPeer = true;
		}
#ifdef HICAL_HAS_HTTP2
		if (result.mode == "h2")
		{
			result.options.protocolMode = hical::HttpProtocolMode::hHttp2Only;
		}
		else if (result.mode == "negotiate")
		{
			result.options.protocolMode = hical::HttpProtocolMode::hNegotiateHttp2;
		}
		else if (result.mode != "h1")
		{
			throw std::invalid_argument("--mode 只能为 h1/h2/negotiate");
		}
		result.options.http2.maxConnectionsPerOrigin = 1;
#else
		if (result.mode != "h1")
		{
			throw std::invalid_argument("此客户端未编译 H2，不能请求 H2 或协商模式");
		}
#endif
	}

	/**
	 * @brief 读取请求配置；正文和预期响应都从文件复制到配置中。
	 */
	ClientConfig configuration(const interop::Arguments& args)
	{
		args.allow({"--host",
					"--port",
					"--target",
					"--mode",
					"--driver",
					"--expect-protocol",
					"--run-id",
					"--expected",
					"--body",
					"--report",
					"--method",
					"--ca",
					"--tls",
					"--requests",
					"--concurrency",
					"--expect-status",
					"--timeout-ms"});
		ClientConfig result;
		result.host = args.get("--host", "127.0.0.1");
		result.port = static_cast<uint16_t>(args.number("--port", 0, 1, 65535));
		result.target = args.get("--target", "/");
		result.mode = args.get("--mode", "h2");
		result.driver = args.get("--driver", "shared");
		result.expectedProtocol = args.get("--expect-protocol", result.mode == "h1" ? "h1" : "h2");
		result.runId = args.require("--run-id");
		result.report = args.require("--report");
		result.expected = interop::readBinary(args.require("--expected"));
		if (!args.get("--body").empty())
		{
			result.body = interop::readBinary(args.get("--body"));
		}
		const auto method = args.get("--method", "GET");
		if ((method != "GET" && method != "POST")
			|| (result.driver != "shared" && result.driver != "pool" && result.driver != "pool-lease")
			|| (result.expectedProtocol != "h1" && result.expectedProtocol != "h2"))
		{
			throw std::invalid_argument("method/driver/expect-protocol 参数非法");
		}
		result.method = method == "GET" ? hical::HttpMethod::hGet : hical::HttpMethod::hPost;
		result.requests = args.number("--requests", 8, 1, 256);
		result.concurrency = args.number("--concurrency", 8, 1, 64);
		result.expectedStatus = args.number("--expect-status", 200, 100, 599);
		result.timeoutMilliseconds = args.number("--timeout-ms", 20000, 100, 120000);
		result.options.connectTimeout = std::chrono::milliseconds(result.timeoutMilliseconds);
		result.options.requestTimeout = std::chrono::milliseconds(result.timeoutMilliseconds);
		configureTransport(args, result);
		return result;
	}

	/**
	 * @brief 将头字段转为数组，保留重复字段。
	 */
	boost::json::array headerReport(const hical::HeaderMap& headers)
	{
		boost::json::array result;
		for (const auto& [name, value] : headers)
		{
			result.push_back(boost::json::object {{"name", name}, {"value", value}});
		}
		return result;
	}

	/**
	 * @brief 保存本轮请求状态，只在同一个 io_context 线程读写。
	 */
	struct RunState
	{
		ClientConfig config;
		std::unique_ptr<hical::HttpClientPool> pool;
		std::shared_ptr<hical::HttpClient> client;
		std::vector<boost::json::object> results;
		std::vector<std::string> exceptions;
		size_t next {0};
		size_t workers {0};
		bool timedOut {false};
	};

	/**
	 * @brief 校验响应的协议、状态码和完整正文，生成报告记录。
	 */
	boost::json::object responseReport(size_t index, const hical::HttpClientResult& result, const ClientConfig& config)
	{
		std::string protocol = "h1";
#ifdef HICAL_HAS_HTTP2
		protocol = result.protocol == hical::HttpProtocol::hHttp2 ? "h2" : "h1";
#endif
		const auto matches = result.error.ok() && result.status == config.expectedStatus
							 && result.body == config.expected && protocol == config.expectedProtocol;
		boost::json::object report {{"index", index},
									{"completed", true},
									{"ok", matches},
									{"protocol", protocol},
									{"status", result.status},
									{"body_size", result.body.size()},
									{"body_sha256", interop::sha256(result.body)},
									{"body_matches", result.body == config.expected},
									{"error_code", static_cast<uint32_t>(result.error.code)},
									{"error", result.error.message},
									{"headers", headerReport(result.headers)}};
#ifdef HICAL_HAS_HTTP2
		report["trailers"] = headerReport(result.trailers);
		report["protocol_negotiation_failed"] = result.error.code == hical::ErrorCode::hProtocolNegotiationFailed;
		if (result.error.http2)
		{
			report["http2_error"] =
				boost::json::object {{"code", result.error.http2->code}, {"stream_id", result.error.http2->streamId}};
		}
#endif
		return report;
	}

	/**
	 * @brief 逐个执行请求；协程结束前一直持有 state。
	 */
	hical::Awaitable<void> worker(std::shared_ptr<RunState> state)
	{
		const auto& config = state->config;
		while (!state->timedOut && state->next < state->results.size())
		{
			const auto index = state->next++;
			hical::HeaderMap headers;
			headers.set("x-interop-run", config.runId);
			headers.set("x-interop-request", std::to_string(index));
			headers.set("accept-encoding", "identity");
			hical::HttpClientResult result;
			if (config.driver == "pool")
			{
				result = co_await state->pool->request(config.host,
													   config.port,
													   config.method,
													   config.target,
													   config.body,
													   headers,
													   config.options);
			}
			else
			{
				result = co_await state->client->request(config.method, config.target, config.body, headers);
			}
			state->results[index] = responseReport(index, result, config);
		}
	}

	/**
	 * @brief 在所属事件循环关闭客户端和连接池。
	 */
	void closeClients(RunState& state)
	{
		if (state.client)
		{
			state.client->close();
		}
		if (state.pool)
		{
			state.pool->shutdown();
		}
	}

	/**
	 * @brief 保存本轮结果，全部响应校验通过才返回 true。
	 */
	bool writeReport(RunState& state)
	{
		bool ok = !state.timedOut && state.exceptions.empty();
		boost::json::array results;
		for (size_t index = 0; index < state.results.size(); ++index)
		{
			auto& item = state.results[index];
			if (item.empty())
			{
				item = {{"index", index}, {"completed", false}, {"ok", false}};
			}
			ok = ok && item.at("ok").as_bool();
			results.push_back(std::move(item));
		}
		boost::json::array exceptions;
		for (const auto& error : state.exceptions)
		{
			exceptions.emplace_back(error);
		}
		interop::writeJson(state.config.report,
						   boost::json::object {{"schema", 1},
												{"ok", ok},
												{"driver", state.config.driver},
												{"mode", state.config.mode},
												{"run_id", state.config.runId},
												{"requests", state.config.requests},
												{"concurrency", state.config.concurrency},
												{"expected_body_size", state.config.expected.size()},
												{"expected_body_sha256", interop::sha256(state.config.expected)},
												{"timed_out", state.timedOut},
												{"exceptions", exceptions},
												{"results", results}});
		return ok;
	}

	/**
	 * @brief 保存工作协程的异常，最后一个协程结束时关闭资源并取消超时等待。
	 */
	void finishWorker(RunState& state, boost::asio::steady_timer& deadline, std::exception_ptr error)
	{
		if (error)
		{
			try
			{
				std::rethrow_exception(error);
			}
			catch (const std::exception& exception)
			{
				state.exceptions.emplace_back(exception.what());
			}
			catch (...)
			{
				state.exceptions.emplace_back("未知异常");
			}
		}
		if (--state.workers == 0)
		{
			closeClients(state);
			deadline.cancel();
		}
	}

	/**
	 * @brief 运行本轮请求并保存报告，全部校验通过才返回 true。
	 */
	bool run(ClientConfig config)
	{
		interop::ProcessDeadline nativeDeadline(std::chrono::milliseconds(config.timeoutMilliseconds + 5000));
		boost::asio::io_context io;
		auto state = std::make_shared<RunState>();
		state->config = std::move(config);
		state->results.resize(static_cast<size_t>(state->config.requests));
		if (state->config.driver == "shared")
		{
			state->client =
				std::make_shared<hical::HttpClient>(io, state->config.host, state->config.port, state->config.options);
		}
		else
		{
			state->pool = std::make_unique<hical::HttpClientPool>(io);
			if (state->config.driver == "pool-lease")
			{
				state->client = state->pool->acquire(state->config.host, state->config.port, state->config.options);
				if (!state->client)
				{
					throw std::runtime_error("连接池未返回租借客户端");
				}
			}
		}
		boost::asio::steady_timer deadline(io);
		deadline.expires_after(std::chrono::milliseconds(state->config.timeoutMilliseconds));
		deadline.async_wait(
			[state](const boost::system::error_code& error)
			{
				if (!error)
				{
					state->timedOut = true;
					closeClients(*state);
				}
			});
		state->workers = static_cast<size_t>(std::min(state->config.requests, state->config.concurrency));
		const auto workers = state->workers;
		for (size_t index = 0; index < workers; ++index)
		{
			boost::asio::co_spawn(io,
								  worker(state),
								  [state, &deadline](std::exception_ptr error)
								  {
									  finishWorker(*state, deadline, error);
								  });
		}
		io.run();
		return writeReport(*state);
	}
} // namespace

/**
 * @brief 运行互操作客户端。
 * @param argc 参数数量
 * @param argv 命令行参数
 * @return 校验通过为 0，配置错误为 1，响应失败为 2
 */
int main(int argc, char** argv)
{
	try
	{
		return run(configuration(interop::Arguments(argc, argv))) ? 0 : 2;
	}
	catch (const std::exception& error)
	{
		HICAL_LOG_ERROR("客户端探针失败：{}", error.what());
		return 1;
	}
}
