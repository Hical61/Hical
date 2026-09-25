cmake_minimum_required(VERSION 3.20)
project(libuvcpp_bench LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)   # 该库是 C++11，bench 服务器不用更高

# 这里没有 FetchContent：libuvcpp 的 CMakeLists 假定自己是顶层工程，拿
# ${CMAKE_SOURCE_DIR}/src 去挂 include 目录（拉进来的话会指向本工程，头文件全找不到，
# file(READ) 版本头那一步更是直接 configure 失败），所以只能走它官方支持的
# find_package 路线：Dockerfile 里先把库编好装到 /usr/local，再从这里找。
find_package(uvcpp REQUIRED)
message(STATUS "libuvcpp: ${UVCPP_VERSION}")

add_executable(bench_server main.cpp)
target_link_libraries(bench_server PRIVATE uvcpp::uvcpp)
