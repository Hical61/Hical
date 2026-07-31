cmake_minimum_required(VERSION 3.20)
project(hical VERSION 2.6.7 LANGUAGES C CXX)

# C++26 反射可选支持
option(HICAL_ENABLE_REFLECTION "Enable C++26 reflection (requires compatible compiler)" OFF)

# 构建控制选项（作为库分发时关闭以加快构建）
option(HICAL_BUILD_TESTS    "Build unit tests"       ON)
option(HICAL_BUILD_EXAMPLES "Build example programs" ON)

# 内存池追踪统计（关闭后 TrackedResource 零开销透传，不记录分配/释放计数）
option(HICAL_ENABLE_MEMORY_TRACKING "Enable memory pool allocation/deallocation tracking statistics" ON)

# 中间件性能剖析（启用后对每个命名中间件记录调用次数和耗时统计）
option(HICAL_ENABLE_MIDDLEWARE_PROFILING "Enable per-middleware timing statistics" OFF)

# 使用系统安装的 picohttpparser 替代 bundled 版本（vcpkg 场景下启用）
option(HICAL_USE_SYSTEM_PICOHTTPPARSER "Use system-installed picohttpparser instead of bundled" OFF)

# 数据库中间件（启用后编译 DB 连接池和 MySQL 后端，需要 boost-mysql）
option(HICAL_WITH_DATABASE "Enable database middleware (requires Boost.MySQL)" OFF)

# OpenAPI 自动生成（无额外依赖，复用 Boost.JSON）
option(HICAL_WITH_OPENAPI "Enable OpenAPI 3.0 auto-generation module" ON)

# mimalloc 替代系统 malloc 作为 PMR 最底层分配器
option(HICAL_WITH_MIMALLOC "Use mimalloc as PMR upstream allocator" OFF)

# NUMA 感知内存分配（Linux 有 libnuma 时自动启用）
option(HICAL_WITH_NUMA "Enable NUMA-aware memory allocation (requires libnuma, Linux only)" OFF)

if(HICAL_ENABLE_REFLECTION)
    set(CMAKE_CXX_STANDARD 26)
else()
    set(CMAKE_CXX_STANDARD 20)
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 导出编译命令（用于 clangd/clang-tidy）
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 编译选项
if(MSVC)
    add_compile_options(/W4 /utf-8 /Zc:preprocessor /Zc:__cplusplus)
    add_compile_definitions(_WIN32_WINNT=0x0A00)  # Target Windows 10
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# 查找依赖库（Boost >= 1.82：框架核心所需的最低版本）
find_package(Boost 1.82 REQUIRED COMPONENTS json)
if(NOT TARGET Boost::system)
    # Boost.System is header-only since 1.69; Conan may not provide this target
    add_library(Boost::system ALIAS Boost::headers)
endif()
find_package(OpenSSL REQUIRED)
find_package(ZLIB REQUIRED)

# DB 依赖：boost::mysql any_connection(1.84+) + boost::charconv(1.85+)
if(HICAL_WITH_DATABASE)
    find_package(Boost 1.85 REQUIRED COMPONENTS charconv)
endif()

# NUMA 感知（可选，Linux + libnuma）
if(HICAL_WITH_NUMA)
    find_library(NUMA_LIB NAMES numa libnuma)
    if(NUMA_LIB)
        find_path(NUMA_INCLUDE_DIR numa.h)
        if(NUMA_INCLUDE_DIR)
            message(STATUS "Found NUMA library: ${NUMA_LIB}")
            message(STATUS "Found NUMA include: ${NUMA_INCLUDE_DIR}")
        else()
            message(STATUS "numa.h not found, NUMA disabled")
            set(HICAL_WITH_NUMA OFF)
        endif()
    else()
        message(STATUS "libnuma not found, NUMA disabled")
        set(HICAL_WITH_NUMA OFF)
    endif()
endif()

# mimalloc（可选，替代系统 malloc 作为 PMR upstream）
if(HICAL_WITH_MIMALLOC)
    find_package(mimalloc REQUIRED)
endif()

# GTest 仅在构建测试时需要
if(HICAL_BUILD_TESTS)
    find_package(GTest REQUIRED)
endif()

# Boost.Asio 是 header-only，无需 COMPONENTS
# 但需要确保 Boost 版本足够新（Asio 在 1.66+ 可用）

# 子目录
add_subdirectory(src)

if(HICAL_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(HICAL_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

# 压测服务器（Docker benchmark 用）
option(HICAL_BUILD_BENCH "Build benchmark server" OFF)
if(HICAL_BUILD_BENCH)
    add_executable(bench_server docker/bench_main.cpp)
    target_link_libraries(bench_server PRIVATE hical_core)
endif()

# TFB 压测服务器（TechEmpower Framework Benchmarks 专用）
option(HICAL_BUILD_TFB "Build TFB benchmark server" OFF)
if(HICAL_BUILD_TFB)
    add_executable(tfb_server docker/TFB/bench_main.cpp)
    target_link_libraries(tfb_server PRIVATE hical_core)
endif()

# HttpArena 压测服务器
option(HICAL_BUILD_HTTPARENA "Build HttpArena benchmark server" OFF)
if(HICAL_BUILD_HTTPARENA)
    add_executable(httparena_server docker/HttpArena/src/main.cpp)
    target_link_libraries(httparena_server PRIVATE hical_core)
endif()

# 生产部署服务器（docker/prod/ 目录）
option(HICAL_BUILD_DEPLOY "Build production deployment server" OFF)
if(HICAL_BUILD_DEPLOY)
    add_executable(prod_server docker/prod/prod_server.cpp)
    target_link_libraries(prod_server PRIVATE hical_core)
endif()

# CMake package export（支持 find_package(hical)）
include(CMakePackageConfigHelpers)

install(
    EXPORT hicalTargets
    FILE hicalTargets.cmake
    NAMESPACE hical::
    DESTINATION lib/cmake/hical
)

configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/hicalConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/hicalConfig.cmake
    INSTALL_DESTINATION lib/cmake/hical
)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/hicalConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(
    FILES
    ${CMAKE_CURRENT_BINARY_DIR}/hicalConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/hicalConfigVersion.cmake
    DESTINATION lib/cmake/hical
)
