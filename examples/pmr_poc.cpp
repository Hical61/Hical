#include "core/MemoryPool.h"
#include "core/PmrBuffer.h"
#include <iostream>
#include <chrono>
#include <vector>
#include <thread>
#include <memory_resource>

using namespace hical;

/**
 * @brief pmr 内存池性能验证 PoC
 *
 * 场景一：复用缓冲区（模拟 HTTP 请求反复读写同一个 buffer）
 * 场景二：单调池批量分配（模拟一次请求生命周期内的多次分配）
 * 场景三：PmrBuffer 功能验证
 * 场景四：多线程并发
 */

// 防止编译器优化掉结果
volatile char gSink = 0;

// 场景一：复用缓冲区 — 写入/读出循环
void benchmarkReuseDefault(int iterations)
{
    auto start = std::chrono::high_resolution_clock::now();

    std::vector<char> buffer;
    buffer.reserve(4096);
    for (int i = 0; i < iterations; ++i)
    {
        buffer.resize(2048);
        buffer[0] = static_cast<char>(i);
        gSink = buffer[0];
        buffer.clear();
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
    std::cout << "  std::vector:     " << us << " us\n";
}

void benchmarkReusePmrThreadLocal(int iterations)
{
    auto allocator = MemoryPool::instance().threadLocalAllocator();
    auto start = std::chrono::high_resolution_clock::now();

    std::pmr::vector<char> buffer(allocator);
    buffer.reserve(4096);
    for (int i = 0; i < iterations; ++i)
    {
        buffer.resize(2048);
        buffer[0] = static_cast<char>(i);
        gSink = buffer[0];
        buffer.clear();
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
    std::cout << "  pmr(thread):     " << us << " us\n";
}

// 场景二：单调池批量分配 — 模拟一次 HTTP 请求中分配多个对象后整体释放
void benchmarkBatchDefault(int iterations)
{
    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < iterations; ++i)
    {
        // 模拟一次请求：分配 header + body + json
        std::vector<char> header(128);
        std::vector<char> body(2048);
        std::vector<char> json(512);
        header[0] = 'H';
        body[0] = 'B';
        json[0] = 'J';
        gSink = header[0] + body[0] + json[0];
        // 请求结束，三个 vector 析构各自释放
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
    std::cout << "  std::allocator:  " << us << " us\n";
}

void benchmarkBatchMonotonic(int iterations)
{
    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < iterations; ++i)
    {
        // 单调池：一次请求共享一块内存，结束后整体释放
        auto pool = MemoryPool::instance().createRequestPool(4096);
        std::pmr::polymorphic_allocator<char> alloc(pool.get());

        std::pmr::vector<char> header(128, alloc);
        std::pmr::vector<char> body(2048, alloc);
        std::pmr::vector<char> json(512, alloc);
        header[0] = 'H';
        body[0] = 'B';
        json[0] = 'J';
        gSink = header[0] + body[0] + json[0];
        // pool 析构 -> 整块内存一次性释放，无需逐个 free
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
    std::cout << "  pmr(monotonic):  " << us << " us\n";
}

// 场景三：PmrBuffer 功能测试
void testPmrBuffer()
{
    std::cout << "\n[PmrBuffer]\n";

    auto allocator = MemoryPool::instance().threadLocalAllocator();
    PmrBuffer buffer(allocator);

    buffer.append("Hello, ");
    buffer.append("pmr world!");
    std::cout << "  write: " << buffer.readableBytes() << " bytes -> \""
              << std::string(buffer.peek(), buffer.readableBytes()) << "\"\n";

    auto data = buffer.read(7);
    std::cout << "  read 7: \"" << data << "\", remaining: "
              << buffer.readableBytes() << " bytes\n";

    buffer.append("\r\nNext line");
    auto crlf = buffer.findCRLF();
    std::cout << "  findCRLF: " << (crlf ? "found" : "not found")
              << " at offset " << (crlf ? crlf - buffer.peek() : -1) << "\n";
}

// 场景四：多线程并发
void testMultiThread()
{
    std::cout << "\n[MultiThread]\n";

    const int numThreads = 4;
    const int iterations = 50000;

    auto start = std::chrono::high_resolution_clock::now();

    std::vector<std::thread> threads;
    for (int t = 0; t < numThreads; ++t)
    {
        threads.emplace_back([t, iterations]() {
            auto alloc = MemoryPool::instance().threadLocalAllocator();
            PmrBuffer buf(alloc);
            for (int i = 0; i < iterations; ++i)
            {
                buf.append("Thread " + std::to_string(t) + " msg");
                buf.retrieveAll();
            }
        });
    }

    for (auto& th : threads)
    {
        th.join();
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    auto stats = MemoryPool::instance().getStats();
    std::cout << "  " << numThreads << " threads x " << iterations << " ops\n";
    std::cout << "  time: " << ms << " ms\n";
    std::cout << "  total allocations: " << stats.totalAllocations << "\n";
    std::cout << "  peak bytes: " << stats.peakBytesAllocated << "\n";
}

int main()
{
    const int N = 1000000;

    std::cout << "========== hical pmr PoC ==========\n\n";

    std::cout << "[Reuse buffer] " << N << " iterations:\n";
    benchmarkReuseDefault(N);
    benchmarkReusePmrThreadLocal(N);

    std::cout << "\n[Batch alloc per-request] " << N / 10 << " iterations:\n";
    benchmarkBatchDefault(N / 10);
    benchmarkBatchMonotonic(N / 10);

    testPmrBuffer();
    testMultiThread();

    std::cout << "\n========== PoC done ==========\n";
    return 0;
}
