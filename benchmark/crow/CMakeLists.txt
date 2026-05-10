cmake_minimum_required(VERSION 3.20)
project(bench_crow)
set(CMAKE_CXX_STANDARD 20)

find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)

add_executable(bench_server main.cpp)
target_include_directories(bench_server PRIVATE /usr/local/include)

# Crow v1.2.0 需要 CROW_USE_BOOST 告知使用 Boost.Asio
target_compile_definitions(bench_server PRIVATE CROW_USE_BOOST)
target_link_libraries(bench_server PRIVATE Threads::Threads OpenSSL::SSL OpenSSL::Crypto)
