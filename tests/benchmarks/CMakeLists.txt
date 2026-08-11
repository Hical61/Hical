# Hical 微基准测试（Google Benchmark）
# 仅在 HICAL_WITH_BENCHMARK=ON 时编译

# 路由微基准
add_executable(bench_router bench_router.cpp bench_main.cpp)
target_link_libraries(bench_router PRIVATE hical_core benchmark::benchmark)

# 完美哈希路由器微基准
add_executable(bench_perfect_hash bench_perfect_hash.cpp bench_main.cpp)
target_link_libraries(bench_perfect_hash PRIVATE hical_core benchmark::benchmark)
