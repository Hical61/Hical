cmake_minimum_required(VERSION 3.28)
project(HicalApiContractManual LANGUAGES CXX)

set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
find_package(hical CONFIG REQUIRED)

set(example_dir "${CMAKE_CURRENT_LIST_DIR}/../../../../../examples/api_contract")
add_executable(hical_rooms_manual "${example_dir}/main.cpp" manual.cpp)
target_include_directories(hical_rooms_manual PRIVATE "${example_dir}")
target_link_libraries(hical_rooms_manual PRIVATE hical::hical_core)
if(MSVC)
    target_compile_options(hical_rooms_manual PRIVATE /utf-8)
endif()
