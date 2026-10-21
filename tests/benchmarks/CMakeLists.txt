# Hical 微基准测试（Google Benchmark）
# 仅在 HICAL_WITH_BENCHMARK=ON 时编译

# 路由微基准
add_executable(bench_router bench_router.cpp bench_main.cpp)
target_link_libraries(bench_router PRIVATE hical_core benchmark::benchmark)

# 完美哈希路由器微基准
add_executable(bench_perfect_hash bench_perfect_hash.cpp bench_main.cpp)
target_link_libraries(bench_perfect_hash PRIVATE hical_core benchmark::benchmark)

# 请求热路径：参数路由、嵌套 DTO、首次表单解析
add_executable(bench_hot_paths bench_hot_paths.cpp bench_main.cpp)
target_link_libraries(bench_hot_paths PRIVATE hical_core benchmark::benchmark)

# 读缓冲借还与可写区域复用
add_executable(bench_read_buffer bench_read_buffer.cpp bench_main.cpp)
target_link_libraries(bench_read_buffer PRIVATE hical_core benchmark::benchmark)
target_compile_definitions(bench_read_buffer PRIVATE HICAL_BENCH_READ_BUFFER_REUSE)
