cmake_minimum_required(VERSION 3.28)
project(HicalApiContractExample LANGUAGES CXX)

set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

find_package(hical CONFIG REQUIRED)
add_executable(api_contract_server main.cpp app.cpp)
target_link_libraries(api_contract_server PRIVATE hical::hical_core)
if(MSVC)
    target_compile_options(api_contract_server PRIVATE /utf-8)
endif()
