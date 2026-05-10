cmake_minimum_required(VERSION 3.20)
project(bench_cpphttplib)
set(CMAKE_CXX_STANDARD 17)

find_package(Threads REQUIRED)

add_executable(bench_server main.cpp)
target_include_directories(bench_server PRIVATE /usr/local/include)
target_link_libraries(bench_server PRIVATE Threads::Threads)
