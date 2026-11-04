cmake_minimum_required(VERSION 3.20)
project(bench_cinatra)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# cinatra 独立版源码根目录（Docker 构建时由 -DCINATRA_ROOT 传入）
set(CINATRA_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/cinatra" CACHE PATH "cinatra source root")

find_package(Threads REQUIRED)

add_executable(bench_server main.cpp)
target_include_directories(bench_server PRIVATE
    ${CINATRA_ROOT}/include
    ${CINATRA_ROOT}/iguana)
target_compile_definitions(bench_server PRIVATE ASIO_STANDALONE)
# GCC 下协程 + slp vectorize 会链接失败，官方 build.cmake 也加了这一条
target_compile_options(bench_server PRIVATE
    $<$<CXX_COMPILER_ID:GNU>:-fno-tree-slp-vectorize>)
target_link_libraries(bench_server PRIVATE Threads::Threads)
