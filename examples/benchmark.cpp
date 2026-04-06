#include <boost/asio.hpp>
#include <iostream>
#include <chrono>
#include <atomic>
#include <vector>
#include <thread>

using boost::asio::ip::tcp;

class BenchmarkClient {
public:
    BenchmarkClient(boost::asio::io_context& io,
                    const std::string& host,
                    const std::string& port,
                    int num_requests,
                    std::atomic<int>& completed,
                    std::atomic<int>& errors)
        : socket_(io),
          resolver_(io),
          num_requests_(num_requests),
          completed_(completed),
          errors_(errors) {

        auto endpoints = resolver_.resolve(host, port);
        boost::asio::async_connect(socket_, endpoints,
            [this](boost::system::error_code ec, tcp::endpoint) {
                if (!ec) {
                    send_request();
                } else {
                    errors_++;
                }
            });
    }

private:
    void send_request() {
        if (current_request_ >= num_requests_) {
            socket_.close();
            return;
        }

        std::string msg = "Benchmark test message " +
                         std::to_string(current_request_) + "\n";

        boost::asio::async_write(socket_,
            boost::asio::buffer(msg),
            [this, msg](boost::system::error_code ec, std::size_t) {
                if (!ec) {
                    receive_response(msg.length());
                } else {
                    errors_++;
                    socket_.close();
                }
            });
    }

    void receive_response(std::size_t expected_len) {
        boost::asio::async_read(socket_,
            boost::asio::buffer(buffer_, expected_len),
            [this](boost::system::error_code ec, std::size_t) {
                if (!ec) {
                    completed_++;
                    current_request_++;
                    send_request();
                } else {
                    errors_++;
                    socket_.close();
                }
            });
    }

    tcp::socket socket_;
    tcp::resolver resolver_;
    int num_requests_;
    int current_request_ = 0;
    std::atomic<int>& completed_;
    std::atomic<int>& errors_;
    char buffer_[1024];
};

int main(int argc, char* argv[]) {
    if (argc != 5) {
        std::cerr << "用法: benchmark <host> <port> <并发数> <每连接请求数>\n";
        std::cerr << "示例: benchmark localhost 8888 100 1000\n";
        return 1;
    }

    std::string host = argv[1];
    std::string port = argv[2];
    int num_clients = std::atoi(argv[3]);
    int requests_per_client = std::atoi(argv[4]);

    std::atomic<int> completed{0};
    std::atomic<int> errors{0};

    auto start = std::chrono::high_resolution_clock::now();

    boost::asio::io_context io;

    // 创建多个客户端
    std::vector<std::unique_ptr<BenchmarkClient>> clients;
    for (int i = 0; i < num_clients; ++i) {
        clients.push_back(std::make_unique<BenchmarkClient>(
            io, host, port, requests_per_client, completed, errors));
    }

    // 运行事件循环
    io.run();

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        end - start).count();

    int total_requests = num_clients * requests_per_client;
    double seconds = duration / 1000.0;
    double qps = completed.load() / seconds;

    std::cout << "\n========== 压力测试结果 ==========\n";
    std::cout << "总请求数: " << total_requests << "\n";
    std::cout << "成功请求: " << completed.load() << "\n";
    std::cout << "失败请求: " << errors.load() << "\n";
    std::cout << "总耗时: " << duration << " ms\n";
    std::cout << "QPS: " << static_cast<int>(qps) << " req/s\n";
    std::cout << "平均延迟: " << (duration * 1.0 / completed.load())
              << " ms\n";
    std::cout << "==================================\n";

    return 0;
}
