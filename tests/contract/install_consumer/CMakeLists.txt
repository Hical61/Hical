cmake_minimum_required(VERSION 3.28)
project(HicalInstallConsumer LANGUAGES CXX)

if(NOT HICAL_EXPECT_NATIVE MATCHES "^[01]$" OR NOT HICAL_EXPECT_OPENAPI MATCHES "^[01]$")
    message(FATAL_ERROR "请将 HICAL_EXPECT_NATIVE 和 HICAL_EXPECT_OPENAPI 各设为 0 或 1")
endif()

if(NOT IS_ABSOLUTE "${hical_DIR}")
    message(FATAL_ERROR "请用 -Dhical_DIR 指向本次安装的包配置目录，并使用绝对路径")
endif()
file(REAL_PATH "${hical_DIR}" expected_directory)
set(expected_config "${expected_directory}/hicalConfig.cmake")
if(NOT EXISTS "${expected_config}" OR IS_DIRECTORY "${expected_config}")
    message(FATAL_ERROR "HICAL_INSTALL_CONFIG_MISSING: 本次安装目录缺少 hicalConfig.cmake：${expected_config}")
endif()
find_package(hical CONFIG REQUIRED
    CONFIGS hicalConfig.cmake
    PATHS "${expected_directory}"
    NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
file(REAL_PATH "${hical_CONFIG}" actual_config)
if(NOT actual_config STREQUAL expected_config)
    message(FATAL_ERROR "CMake 找到的 Hical 包配置是 ${actual_config}，本次应使用 ${expected_config}")
endif()

set(expected_standard cxx_std_20)
if(HICAL_EXPECT_NATIVE)
    set(expected_standard cxx_std_26)
endif()
get_target_property(features hical::hical_core INTERFACE_COMPILE_FEATURES)
if(NOT expected_standard IN_LIST features)
    message(FATAL_ERROR "安装目标应该导出 ${expected_standard}，实际导出的编译特性是 ${features}")
endif()

add_executable(hical_install_consumer main.cpp)
target_link_libraries(hical_install_consumer PRIVATE hical::hical_core)
target_compile_definitions(hical_install_consumer PRIVATE
    HICAL_EXPECT_NATIVE=${HICAL_EXPECT_NATIVE}
    HICAL_EXPECT_OPENAPI=${HICAL_EXPECT_OPENAPI})

enable_testing()
add_test(NAME HicalInstallConsumer.RoundTrip COMMAND hical_install_consumer)
