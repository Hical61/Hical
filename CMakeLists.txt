cmake_minimum_required(VERSION 3.20)
project(hical VERSION 0.1.0 LANGUAGES CXX)

# C++20 标准（C++26 反射特性待编译器完全支持后启用）
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 导出编译命令（用于 clangd/clang-tidy）
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 编译选项
if(MSVC)
    add_compile_options(/W4 /utf-8)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# 查找依赖库
find_package(Boost 1.70 REQUIRED COMPONENTS system json)
find_package(GTest REQUIRED)
find_package(OpenSSL REQUIRED)

# Boost.Asio 和 Boost.Beast 是 header-only，无需 COMPONENTS
# 但需要确保 Boost 版本足够新（Beast 在 1.70+ 可用）

# 包含目录
include_directories(
    ${CMAKE_SOURCE_DIR}/src
    ${Boost_INCLUDE_DIRS}
)

# 启用测试
enable_testing()

# 子目录
add_subdirectory(src)
add_subdirectory(tests)
add_subdirectory(examples)

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
