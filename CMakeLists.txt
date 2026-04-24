cmake_minimum_required(VERSION 3.20)
project(hical VERSION 2.1.0 LANGUAGES CXX)

# C++26 反射可选支持
option(HICAL_ENABLE_REFLECTION "Enable C++26 reflection (requires compatible compiler)" OFF)

# 构建控制选项（作为库分发时关闭以加快构建）
option(HICAL_BUILD_TESTS    "Build unit tests"       ON)
option(HICAL_BUILD_EXAMPLES "Build example programs" ON)

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

# 查找依赖库
find_package(Boost 1.78 REQUIRED COMPONENTS json)
if(NOT TARGET Boost::system)
    # Boost.System is header-only since 1.69; Conan may not provide this target
    add_library(Boost::system ALIAS Boost::headers)
endif()
find_package(OpenSSL REQUIRED)

# GTest 仅在构建测试时需要
if(HICAL_BUILD_TESTS)
    find_package(GTest REQUIRED)
endif()

# Boost.Asio 和 Boost.Beast 是 header-only，无需 COMPONENTS
# 但需要确保 Boost 版本足够新（Beast 在 1.70+ 可用）

# 子目录
add_subdirectory(src)

if(HICAL_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(HICAL_BUILD_EXAMPLES)
    add_subdirectory(examples)
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
