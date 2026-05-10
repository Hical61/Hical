cmake_minimum_required(VERSION 3.20)
project(bench_cinatra)
set(CMAKE_CXX_STANDARD 20)

include(FetchContent)

# 拉取 yalantinglibs（包含 cinatra + iguana）
FetchContent_Declare(
    yalantinglibs
    GIT_REPOSITORY https://github.com/alibaba/yalantinglibs.git
    GIT_TAG v0.3.0
    GIT_SHALLOW ON
)

set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(BUILD_UNIT_TESTS OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(yalantinglibs)

find_package(Threads REQUIRED)

add_executable(bench_server main.cpp)
target_link_libraries(bench_server PRIVATE yalantinglibs::yalantinglibs Threads::Threads)
