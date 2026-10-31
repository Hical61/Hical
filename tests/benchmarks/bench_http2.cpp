/**
 * @file bench_http2.cpp
 * @brief HTTP 客户端压测，校验响应并记录连接池队列
 */

#include "../../docker/test/http2/InteropCommon.h"
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
	using Clock = std::chrono::steady_clock;

	struct Configuration
	{
		std::string host;
		std::string target;
		std::string driver;
		std::string mode;
		std::string runId;
		std::string sourceIdentity;
		std::string expected;
		std::string body;
		std::filesystem::path report;
		hical::HttpClientOptions options;
		hical::HttpMethod method {hical::HttpMethod::hGet};
		uint16_t port {0};
		size_t requests {0};
		size_t concurrency {0};
		size_t connections {0};
		int timeoutMilliseconds {0};
	};

	/**
	 * @brief 读取压测参数、请求正文和预期响应。
	 */
	Configuration configuration(const interop::Arguments& args)
	{
		args.allow({"--host",
					"--port",
					"--target",
					"--driver",
					"--mode",
					"--run-id",
					"--source-identity",
					"--expected",
					"--body",
					"--report",
					"--method",
					"--ca",
					"--requests",
					"--concurrency",
					"--connections",
					"--streams",
					"--timeout-ms"});
		Configuration result;
		result.host = args.get("--host", "127.0.0.1");
		result.port = static_cast<uint16_t>(args.number("--port", 0, 1, 65535));
		result.target = args.get("--target", "/payload.bin");
		result.driver = args.get("--driver", "shared");
		result.mode = args.get("--mode", "h2");
		result.runId = args.require("--run-id");
		result.sourceIdentity = args.require("--source-identity");
		result.report = args.require("--report");
		result.expected = interop::readBinary(args.require("--expected"));
		result.requests = static_cast<size_t>(args.number("--requests", 1000, 1, 1000000));
		result.concurrency = static_cast<size_t>(args.number("--concurrency", 8, 1, 256));
		result.connections = static_cast<size_t>(args.number("--connections", 1, 1, 64));
		const auto streams = args.number("--streams", 8, 1, 256);
		result.timeoutMilliseconds = args.number("--timeout-ms", 30000, 100, 600000);
		const auto method = args.get("--method", "GET");
		if ((method != "GET" && method != "POST") || (result.driver != "shared" && result.driver != "pool"))
		{
			throw std::invalid_argument("method 仅支持 GET/POST，driver 仅支持 shared/pool");
		}
		result.method = method == "POST" ? hical::HttpMethod::hPost : hical::HttpMethod::hGet;
		if (result.method == hical::HttpMethod::hPost)
		{
			result.body = interop::readBinary(args.require("--body"));
		}
		result.options.tlsContext = std::make_shared<hical::SslContext>(boost::asio::ssl::context::tls_client);
		result.options.tlsContext->loadCaCertificate(args.require("--ca"));
		result.options.verifyPeer = true;
		result.options.connectTimeout = std::chrono::milliseconds(result.timeoutMilliseconds);
		result.options.requestTimeout = std::chrono::milliseconds(result.timeoutMilliseconds);
		if (result.mode == "h2")
		{
#ifdef HICAL_HAS_HTTP2
			result.options.protocolMode = hical::HttpProtocolMode::hHttp2Only;
#else
			throw std::invalid_argument("此二进制未编译 HTTP/2");
#endif
		}
		else if (result.mode != "h1")
		{
			throw std::invalid_argument("mode 仅支持 h1/h2");
		}
		result.options.http2.maxConcurrentStreams = static_cast<uint32_t>(streams);
		result.options.http2.maxConnectionsPerOrigin = result.connections;
		result.options.http2.maxQueuedRequestsPerOrigin = result.concurrency;
		result.options.http2.queueTimeout = std::chrono::milliseconds(result.timeoutMilliseconds);
		return result;
	}

	/**
	 * @brief 本轮压测状态，只在同一个 io_context 线程读写，由工作协程共同持有。
	 */
	struct State
	{
		Configuration config;
		std::unique_ptr<hical::HttpClientPool> pool;
		std::vector<std::shared_ptr<hical::HttpClient>> clients;
		std::vector<boost::json::object> requests;
		boost::json::array poolSamples;
		boost::json::array exceptions;
		Clock::time_point started;
		Clock::time_point finished;
		size_t next {0};
		size_t workers {0};
		size_t peakManaged {0};
		size_t peakActive {0};
		size_t peakQueued {0};
		bool timedOut {false};
	};

	/**
	 * @brief 返回 start 到 end 的微秒数，避免把墙钟调整计入请求延迟。
	 */
	int64_t microseconds(Clock::time_point start, Clock::time_point end)
	{
		return std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
	}

	/**
	 * @brief 在所属事件循环读取连接池统计，排队数取自池的公开接口。
	 */
	void samplePool(State& state)
	{
		if (!state.pool)
		{
			return;
		}
		const auto stats = state.pool->stats();
		state.peakManaged = std::max(state.peakManaged, stats.managedConnections);
		state.peakActive = std::max(state.peakActive, stats.activeRequests);
		state.peakQueued = std::max(state.peakQueued, stats.peakQueuedRequests);
		state.poolSamples.push_back(boost::json::object {{"elapsed_us", microseconds(state.started, Clock::now())},
														 {"managed_connections", stats.managedConnections},
														 {"connecting_connections", stats.connectingConnections},
														 {"active_requests", stats.activeRequests},
														 {"queued_requests", stats.queuedRequests},
														 {"lifetime_peak_queued_requests", stats.peakQueuedRequests}});
	}

	/**
	 * @brief 每毫秒采样一次连接池；run 会等待全部回调结束后再销毁 timer 和 state。
	 */
	void scheduleSample(boost::asio::steady_timer& timer, const std::shared_ptr<State>& state)
	{
		samplePool(*state);
		if (state->workers == 0 || state->timedOut)
		{
			return;
		}
		timer.expires_after(std::chrono::milliseconds(1));
		timer.async_wait(
			[state, &timer](const boost::system::error_code& error)
			{
				if (!error)
				{
					scheduleSample(timer, state);
				}
			});
	}

	/**
	 * @brief 在所属事件循环关闭客户端和连接池，让清理回调继续运行。
	 */
	void closeClients(State& state)
	{
		for (const auto& client : state.clients)
		{
			client->close();
		}
		if (state.pool)
		{
			state.pool->shutdown();
		}
	}

	/**
	 * @brief 记录响应和请求延迟；完整正文比较也计入测量时间。
	 */
	boost::json::object recordResponse(size_t index,
									   const hical::HttpClientResult& result,
									   const Configuration& config,
									   Clock::time_point start)
	{
		const auto protocol = result.protocol == hical::HttpProtocol::hHttp2 ? "h2" : "h1";
		const auto matches = result.body == config.expected;
		return {{"index", index},
				{"latency_us", microseconds(start, Clock::now())},
				{"ok", result.error.ok() && result.status == 200 && matches && protocol == config.mode},
				{"status", result.status},
				{"protocol", protocol},
				{"body_matches", matches},
				{"body_size", result.body.size()},
				{"error_code", static_cast<uint32_t>(result.error.code)},
				{"error", result.error.message}};
	}

	/**
	 * @brief 按 workerIndex 选择共享客户端，协程结束前一直持有 state。
	 */
	hical::Awaitable<void> worker(std::shared_ptr<State> state, size_t workerIndex)
	{
		const auto& config = state->config;
		hical::HeaderMap headers;
		headers.set("x-interop-run", config.runId);
		headers.set("accept-encoding", "identity");
		while (!state->timedOut && state->next < config.requests)
		{
			const auto index = state->next++;
			const auto started = Clock::now();
			hical::HttpClientResult result;
			if (state->pool)
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
				result = co_await state->clients[workerIndex % state->clients.size()]->request(config.method,
																							   config.target,
																							   config.body,
																							   headers);
			}
			state->requests[index] = recordResponse(index, result, config, started);
			state->requests[index]["started_us"] = microseconds(state->started, started);
			samplePool(*state);
		}
	}

	/**
	 * @brief 保存请求记录和连接池样本，全部请求校验通过才返回 true。
	 */
	bool writeReport(State& state)
	{
		bool ok = !state.timedOut && state.exceptions.empty();
		boost::json::array requests;
		for (auto& request : state.requests)
		{
			ok = ok && !request.empty() && request.at("ok").as_bool();
			requests.push_back(std::move(request));
		}
		const auto elapsed = microseconds(state.started, state.finished);
		const auto& config = state.config;
		interop::writeJson(
			config.report,
			boost::json::object {
				{"schema", 1},
				{"ok", ok},
				{"run_id", config.runId},
				{"source_identity", config.sourceIdentity},
				{"mode", config.mode},
				{"driver", config.driver},
				{"target", config.target},
				{"request_count", config.requests},
				{"concurrency", config.concurrency},
				{"configured_connections", config.connections},
				{"configured_streams_per_connection", config.options.http2.maxConcurrentStreams},
				{"elapsed_us", elapsed},
				{"requests_per_second", elapsed > 0 ? config.requests * 1000000.0 / elapsed : 0},
				{"expected_body_sha256", interop::sha256(config.expected)},
				{"timed_out", state.timedOut},
				{"peak_managed_connections_sampled", state.peakManaged},
				{"peak_active_requests_sampled", state.peakActive},
				{"peak_queued_requests", state.peakQueued},
				{"pool_samples", state.poolSamples},
				{"requests", requests},
				{"exceptions", state.exceptions},
				{"measurement_note",
				 "包含首次 TCP/TLS 建连、完整响应校验及测量记录成本；Pool peakQueued 来自公开 Stats 的生命周期峰值，连接和活动峰值为采样值；H1-only/共享 Client 不提供内部队列统计"}});
		return ok;
	}

	/**
	 * @brief 运行本轮压测并写报告；事件循环卡住时由独立看门狗结束进程。
	 */
	bool run(Configuration config)
	{
		interop::ProcessDeadline watchdog(std::chrono::milliseconds(config.timeoutMilliseconds + 5000));
		boost::asio::io_context io;
		auto state = std::make_shared<State>();
		state->config = std::move(config);
		state->requests.resize(state->config.requests);
		if (state->config.driver == "pool")
		{
			state->pool = std::make_unique<hical::HttpClientPool>(io);
		}
		else
		{
			for (size_t index = 0; index < state->config.connections; ++index)
			{
				state->clients.push_back(std::make_shared<hical::HttpClient>(io,
																			 state->config.host,
																			 state->config.port,
																			 state->config.options));
			}
		}
		boost::asio::steady_timer deadline(io);
		boost::asio::steady_timer sampler(io);
		deadline.expires_after(std::chrono::milliseconds(state->config.timeoutMilliseconds));
		deadline.async_wait(
			[state, &sampler](const boost::system::error_code& error)
			{
				if (!error)
				{
					state->timedOut = true;
					sampler.cancel();
					closeClients(*state);
				}
			});
		state->started = Clock::now();
		state->workers = std::min(state->config.requests, state->config.concurrency);
		for (size_t index = 0; index < state->workers; ++index)
		{
			boost::asio::co_spawn(io,
								  worker(state, index),
								  [state, &deadline, &sampler](std::exception_ptr error)
								  {
									  if (error)
									  {
										  try
										  {
											  std::rethrow_exception(error);
										  }
										  catch (const std::exception& exception)
										  {
											  state->exceptions.emplace_back(exception.what());
										  }
										  catch (...)
										  {
											  state->exceptions.emplace_back("未知工作协程异常");
										  }
									  }
									  if (--state->workers == 0)
									  {
										  state->finished = Clock::now();
										  samplePool(*state);
										  closeClients(*state);
										  deadline.cancel();
										  sampler.cancel();
									  }
								  });
		}
		scheduleSample(sampler, state);
		io.run();
		return writeReport(*state);
	}
} // namespace

/**
 * @brief argc/argv 指定测量参数；全部响应通过返回 0，校验失败返回 2，配置错误返回 1。
 */
int main(int argc, char** argv)
{
	try
	{
		return run(configuration(interop::Arguments(argc, argv))) ? 0 : 2;
	}
	catch (const std::exception& error)
	{
		HICAL_LOG_ERROR("HTTP 客户端测量失败：{}", error.what());
		return 1;
	}
}
