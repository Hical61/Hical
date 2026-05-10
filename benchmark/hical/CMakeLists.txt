cmake_minimum_required(VERSION 3.20)
project(bench_hical CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(hical REQUIRED)

add_executable(bench_server main.cpp)
target_link_libraries(bench_server PRIVATE hical::hical_core)
