cmake_minimum_required(VERSION 3.20)
project(bench_oatpp)
set(CMAKE_CXX_STANDARD 20)

# 关闭 oatpp 测试构建，节省编译时间
set(OATPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)

include(FetchContent)
FetchContent_Declare(
    oatpp
    GIT_REPOSITORY https://github.com/oatpp/oatpp.git
    GIT_TAG        1.3.0
)
FetchContent_MakeAvailable(oatpp)

add_executable(bench_server main.cpp)
target_link_libraries(bench_server PRIVATE oatpp)
