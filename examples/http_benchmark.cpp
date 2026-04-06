#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <iostream>
#include <mutex>
#include <numeric>
#include <string>
#include <thread>
#include <vector>

namespace beast = boost::beast;
namespace http = beast::http;
using boost::asio::ip::tcp;

/**
 * @brief HTTP 压测客户端
 *
 * 每个实例在独立协程中完成指定数量的 HTTP 请求，
 * 记录每个请求的延迟。
 */
class HttpBenchClient
{
  public:
    HttpBenchClient(boost::asio::io_context& io,
                    const std::string& host,
                    const std::string& port,
                    const std::string& target,
                    http::verb method,
                    const std::string& body,
                    int numRequests,
                    std::atomic<int>& completed,
                    std::atomic<int>& errors,
                    std::vector<double>& latencies,
                    std::mutex& latencyMutex)
        : io_(io),
          host_(host),
          port_(port),
          target_(target),
          method_(method),
          body_(body),
          numRequests_(numRequests),
          completed_(completed),
          errors_(errors),
          latencies_(latencies),
          latencyMutex_(latencyMutex)
    {
        boost::asio::co_spawn(io_, run(), boost::asio::detached);
    }

  private:
    boost::asio::awaitable<void> run()
    {
        try
        {
            tcp::resolver resolver(io_);
            auto endpoints = co_await resolver.async_resolve(
                host_, port_, boost::asio::use_awaitable);

            beast::tcp_stream stream(io_);
            co_await stream.async_connect(
                endpoints, boost::asio::use_awaitable);

            beast::flat_buffer buffer;

            for (int i = 0; i < numRequests_; ++i)
            {
                http::request<http::string_body> req(method_, target_, 11);
                req.set(http::field::host, host_);
                req.set(http::field::connection, "keep-alive");
                if (!body_.empty())
                {
                    req.body() = body_;
                    req.set(http::field::content_type, "application/json");
                    req.prepare_payload();
                }

                auto start = std::chrono::high_resolution_clock::now();

                co_await http::async_write(
                    stream, req, boost::asio::use_awaitable);

                http::response<http::string_body> res;
                co_await http::async_read(
                    stream, buffer, res, boost::asio::use_awaitable);

                auto end = std::chrono::high_resolution_clock::now();
                double latencyMs =
                    std::chrono::duration<double, std::milli>(end - start)
                        .count();

                {
                    std::lock_guard<std::mutex> lock(latencyMutex_);
                    latencies_.push_back(latencyMs);
                }

                completed_.fetch_add(1, std::memory_order_relaxed);
                buffer.consume(buffer.size());
            }

            // 优雅关闭
            beast::error_code ec;
            stream.socket().shutdown(tcp::socket::shutdown_both, ec);
        }
        catch (const std::exception&)
        {
            errors_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    boost::asio::io_context& io_;
    std::string host_;
    std::string port_;
    std::string target_;
    http::verb method_;
    std::string body_;
    int numRequests_;
    std::atomic<int>& completed_;
    std::atomic<int>& errors_;
    std::vector<double>& latencies_;
    std::mutex& latencyMutex_;
};

/**
 * @brief 打印延迟直方图
 */
void printHistogram(std::vector<double>& latencies)
{
    if (latencies.empty())
    {
        return;
    }

    std::sort(latencies.begin(), latencies.end());

    auto percentile = [&](double p) -> double {
        auto idx = static_cast<size_t>(latencies.size() * p);
        if (idx >= latencies.size())
        {
            idx = latencies.size() - 1;
        }
        return latencies[idx];
    };

    double avg = std::accumulate(latencies.begin(), latencies.end(), 0.0)
                 / latencies.size();

    std::cout << "\n  延迟分布:\n";
    std::cout << "    平均:  " << avg << " ms\n";
    std::cout << "    P50:   " << percentile(0.50) << " ms\n";
    std::cout << "    P90:   " << percentile(0.90) << " ms\n";
    std::cout << "    P95:   " << percentile(0.95) << " ms\n";
    std::cout << "    P99:   " << percentile(0.99) << " ms\n";
    std::cout << "    最小:  " << latencies.front() << " ms\n";
    std::cout << "    最大:  " << latencies.back() << " ms\n";
}

int main(int argc, char* argv[])
{
    if (argc < 5)
    {
        std::cerr << "用法: http_benchmark <host> <port> <并发数> <每连接请求数> "
                     "[target] [method] [body]\n";
        std::cerr << "示例: http_benchmark localhost 8080 50 1000 /api/status GET\n";
        std::cerr << "示例: http_benchmark localhost 8080 50 1000 /api/echo POST "
                     "'{\"hello\":\"world\"}'\n";
        return 1;
    }

    std::string host = argv[1];
    std::string port = argv[2];
    int numClients = std::atoi(argv[3]);
    int requestsPerClient = std::atoi(argv[4]);
    std::string target = argc >= 6 ? argv[5] : "/";
    std::string methodStr = argc >= 7 ? argv[6] : "GET";
    std::string body = argc >= 8 ? argv[7] : "";

    http::verb method = http::string_to_verb(methodStr);
    if (method == http::verb::unknown)
    {
        std::cerr << "不支持的 HTTP 方法: " << methodStr << "\n";
        return 1;
    }

    int totalRequests = numClients * requestsPerClient;
    std::atomic<int> completed{0};
    std::atomic<int> errors{0};
    std::vector<double> latencies;
    std::mutex latencyMutex;
    latencies.reserve(totalRequests);

    std::cout << "========== hical HTTP 基准测试 ==========\n";
    std::cout << "目标: " << host << ":" << port << target << "\n";
    std::cout << "方法: " << methodStr << "\n";
    std::cout << "并发连接: " << numClients << "\n";
    std::cout << "每连接请求: " << requestsPerClient << "\n";
    std::cout << "总请求数: " << totalRequests << "\n";
    std::cout << "=========================================\n";
    std::cout << "运行中...\n";

    auto start = std::chrono::high_resolution_clock::now();

    // 多线程运行
    size_t threadCount = std::min(
        static_cast<size_t>(numClients),
        static_cast<size_t>(std::thread::hardware_concurrency()));
    if (threadCount == 0)
    {
        threadCount = 1;
    }

    boost::asio::io_context io;

    // 创建客户端
    std::vector<std::unique_ptr<HttpBenchClient>> clients;
    for (int i = 0; i < numClients; ++i)
    {
        clients.push_back(std::make_unique<HttpBenchClient>(
            io, host, port, target, method, body,
            requestsPerClient, completed, errors,
            latencies, latencyMutex));
    }

    // 多线程运行 io_context
    std::vector<std::thread> threads;
    for (size_t i = 1; i < threadCount; ++i)
    {
        threads.emplace_back([&io]() { io.run(); });
    }
    io.run();

    for (auto& t : threads)
    {
        if (t.joinable())
        {
            t.join();
        }
    }

    auto end = std::chrono::high_resolution_clock::now();
    double durationMs =
        std::chrono::duration<double, std::milli>(end - start).count();
    double durationSec = durationMs / 1000.0;

    int completedCount = completed.load();
    double qps = completedCount / durationSec;

    std::cout << "\n========== 测试结果 ==========\n";
    std::cout << "  总请求数:    " << totalRequests << "\n";
    std::cout << "  成功请求:    " << completedCount << "\n";
    std::cout << "  失败请求:    " << errors.load() << "\n";
    std::cout << "  总耗时:      " << durationMs << " ms\n";
    std::cout << "  QPS:         " << static_cast<int>(qps) << " req/s\n";

    printHistogram(latencies);

    std::cout << "==============================\n";

    return 0;
}
