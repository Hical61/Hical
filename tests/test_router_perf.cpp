#include "core/Router.h"
#include <gtest/gtest.h>
#include <chrono>
#include <iostream>
#include <string>

using namespace hical;

namespace
{

/**
 * @brief 创建指定数量的静态路由
 */
Router createRouterWithStaticRoutes(int count)
{
    Router router;
    for (int i = 0; i < count; ++i)
    {
        std::string path = "/api/v1/route" + std::to_string(i);
        router.get(path, [](const HttpRequest&) -> HttpResponse {
            return HttpResponse::ok("ok");
        });
    }
    return router;
}

/**
 * @brief 创建指定数量的参数路由
 */
Router createRouterWithParamRoutes(int count)
{
    Router router;
    for (int i = 0; i < count; ++i)
    {
        std::string path = "/api/v1/resource" + std::to_string(i) + "/{id}";
        router.get(path, [](const HttpRequest&) -> HttpResponse {
            return HttpResponse::ok("ok");
        });
    }
    return router;
}

class RouterPerfTest : public ::testing::Test
{
  protected:
    static constexpr int hWarmupIterations = 1000;
    static constexpr int hBenchIterations = 100000;
};

}  // namespace

/**
 * @brief 静态路由查找性能：命中首条
 */
TEST_F(RouterPerfTest, StaticRouteFirstHit)
{
    auto router = createRouterWithStaticRoutes(100);

    HttpRequest req;
    req.setMethod(HttpMethod::hGet);
    req.setTarget("/api/v1/route0");

    // 预热
    boost::asio::io_context io;
    for (int i = 0; i < hWarmupIterations; ++i)
    {
        boost::asio::co_spawn(
            io,
            [&]() -> Awaitable<void> {
                co_await router.dispatch(req);
            },
            boost::asio::detached);
    }
    io.run();
    io.restart();

    // 计时
    auto start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < hBenchIterations; ++i)
    {
        boost::asio::co_spawn(
            io,
            [&]() -> Awaitable<void> {
                co_await router.dispatch(req);
            },
            boost::asio::detached);
    }
    io.run();
    auto end = std::chrono::high_resolution_clock::now();

    double totalMs =
        std::chrono::duration<double, std::milli>(end - start).count();
    double nsPerOp = (totalMs * 1000000.0) / hBenchIterations;

    std::cout << "[静态路由-首条命中] " << hBenchIterations
              << " 次分发, 总耗时: " << totalMs
              << " ms, 每次: " << nsPerOp << " ns\n";

    // 性能断言：每次分发不应超过 10us（宽松）
    EXPECT_LT(nsPerOp, 10000.0);
}

/**
 * @brief 静态路由查找性能：命中末条
 */
TEST_F(RouterPerfTest, StaticRouteLastHit)
{
    auto router = createRouterWithStaticRoutes(100);

    HttpRequest req;
    req.setMethod(HttpMethod::hGet);
    req.setTarget("/api/v1/route99");

    boost::asio::io_context io;

    auto start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < hBenchIterations; ++i)
    {
        boost::asio::co_spawn(
            io,
            [&]() -> Awaitable<void> {
                co_await router.dispatch(req);
            },
            boost::asio::detached);
    }
    io.run();
    auto end = std::chrono::high_resolution_clock::now();

    double totalMs =
        std::chrono::duration<double, std::milli>(end - start).count();
    double nsPerOp = (totalMs * 1000000.0) / hBenchIterations;

    std::cout << "[静态路由-末条命中] " << hBenchIterations
              << " 次分发, 总耗时: " << totalMs
              << " ms, 每次: " << nsPerOp << " ns\n";

    // 哈希表查找：首条和末条应该性能接近
    EXPECT_LT(nsPerOp, 10000.0);
}

/**
 * @brief 静态路由查找性能：未命中
 */
TEST_F(RouterPerfTest, StaticRouteMiss)
{
    auto router = createRouterWithStaticRoutes(100);

    HttpRequest req;
    req.setMethod(HttpMethod::hGet);
    req.setTarget("/api/v1/nonexistent");

    boost::asio::io_context io;

    auto start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < hBenchIterations; ++i)
    {
        boost::asio::co_spawn(
            io,
            [&]() -> Awaitable<void> {
                co_await router.dispatch(req);
            },
            boost::asio::detached);
    }
    io.run();
    auto end = std::chrono::high_resolution_clock::now();

    double totalMs =
        std::chrono::duration<double, std::milli>(end - start).count();
    double nsPerOp = (totalMs * 1000000.0) / hBenchIterations;

    std::cout << "[静态路由-未命中]   " << hBenchIterations
              << " 次分发, 总耗时: " << totalMs
              << " ms, 每次: " << nsPerOp << " ns\n";

    EXPECT_LT(nsPerOp, 10000.0);
}

/**
 * @brief 参数路由查找性能
 */
TEST_F(RouterPerfTest, ParamRouteMatch)
{
    auto router = createRouterWithParamRoutes(20);

    HttpRequest req;
    req.setMethod(HttpMethod::hGet);
    req.setTarget("/api/v1/resource10/12345");

    boost::asio::io_context io;

    auto start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < hBenchIterations; ++i)
    {
        boost::asio::co_spawn(
            io,
            [&]() -> Awaitable<void> {
                co_await router.dispatch(req);
            },
            boost::asio::detached);
    }
    io.run();
    auto end = std::chrono::high_resolution_clock::now();

    double totalMs =
        std::chrono::duration<double, std::milli>(end - start).count();
    double nsPerOp = (totalMs * 1000000.0) / hBenchIterations;

    std::cout << "[参数路由-命中]     " << hBenchIterations
              << " 次分发, 总耗时: " << totalMs
              << " ms, 每次: " << nsPerOp << " ns\n";

    EXPECT_LT(nsPerOp, 50000.0);
}

/**
 * @brief 大量路由下的查找性能
 */
TEST_F(RouterPerfTest, LargeRouteSet)
{
    auto router = createRouterWithStaticRoutes(1000);

    HttpRequest req;
    req.setMethod(HttpMethod::hGet);
    req.setTarget("/api/v1/route500");

    boost::asio::io_context io;

    auto start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < hBenchIterations; ++i)
    {
        boost::asio::co_spawn(
            io,
            [&]() -> Awaitable<void> {
                co_await router.dispatch(req);
            },
            boost::asio::detached);
    }
    io.run();
    auto end = std::chrono::high_resolution_clock::now();

    double totalMs =
        std::chrono::duration<double, std::milli>(end - start).count();
    double nsPerOp = (totalMs * 1000000.0) / hBenchIterations;

    std::cout << "[1000路由-中间命中] " << hBenchIterations
              << " 次分发, 总耗时: " << totalMs
              << " ms, 每次: " << nsPerOp << " ns\n";

    // 哈希表：1000 路由和 100 路由应该性能相当
    EXPECT_LT(nsPerOp, 10000.0);
}
