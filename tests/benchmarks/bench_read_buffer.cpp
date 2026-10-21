/**
 * @file bench_read_buffer.cpp
 * @brief HTTP 读缓冲区借还基准，模拟小请求写入 8 KB 可写缓冲区
 */

#include "core/ReadBufferPool.h"
#include <benchmark/benchmark.h>
#include <cstring>

using namespace hical;

/**
 * @brief 模拟每次请求借缓冲、准备可写区、接收 100 字节并归还
 * @param state Google Benchmark 迭代状态
 */
static void BM_ReadBufferRequest(benchmark::State& state)
{
	constexpr size_t kRequestBytes = 100;
	for (auto _ : state)
	{
#ifdef HICAL_BENCH_READ_BUFFER_REUSE
		auto handle = ReadBufferPool::acquireForRead(ReadBufferPool::kBufferSize);
#else
		auto handle = ReadBufferPool::acquire();
		handle.get().resize(ReadBufferPool::kBufferSize);
#endif
		std::memset(handle.get().data(), 'x', kRequestBytes);
		benchmark::DoNotOptimize(handle.get().data());
		benchmark::ClobberMemory();
	}
	state.SetItemsProcessed(state.iterations());
	state.SetBytesProcessed(state.iterations() * kRequestBytes);
}

BENCHMARK(BM_ReadBufferRequest);
