/**
 * @file bench_main.cpp
 * @brief TechEmpower Framework Benchmarks 专用服务器
 * 仅注册 /json 和 /plaintext 端点，零中间件，走同步快速路径。
 * 环境变量 HICAL_THREADS 控制 IO 线程数（默认 hardware_concurrency）。
 */

#include "core/HttpServer.h"
#include <cstdlib>
#include <thread>

using namespace hical;

// TFB 固定响应体（编译期常量，零运行时构建开销）
static constexpr std::string_view kJsonBody = R"({"message":"Hello, World!"})";
static constexpr std::string_view kPlaintextBody = "Hello, World!";

int main()
{
    const char* threadEnv = std::getenv("HICAL_THREADS");
    size_t threads = threadEnv ? static_cast<size_t>(std::atoi(threadEnv))
                               : std::thread::hardware_concurrency();
    if (threads == 0)
    {
        threads = 1;
    }

    HttpServer server(8080, threads);

    // TFB /json
    server.router().get("/json",
                        [](const HttpRequest&) -> HttpResponse
                        {
                            HttpResponse res;
                            res.setStatus(HttpStatusCode::hOk);
                            res.native().body.assign(kJsonBody.data(), kJsonBody.size());
                            res.native().headers.set("Content-Type", "application/json");
                            return res;
                        });

    // TFB /plaintext
    server.router().get("/plaintext",
                        [](const HttpRequest&) -> HttpResponse
                        {
                            HttpResponse res;
                            res.setStatus(HttpStatusCode::hOk);
                            res.native().body.assign(kPlaintextBody.data(), kPlaintextBody.size());
                            res.native().headers.set("Content-Type", "text/plain");
                            return res;
                        });

    server.setMaxConnections(65535);
    server.setIdleTimeout(0);
    server.setGcInterval(0);

    server.start();
}
