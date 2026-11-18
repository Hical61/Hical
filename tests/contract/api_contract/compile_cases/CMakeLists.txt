cmake_minimum_required(VERSION 3.28)
project(HicalApiContractCompileCases LANGUAGES CXX)

set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

find_package(hical CONFIG REQUIRED)
add_executable(api_contract_compile_cases positive.cpp negative.cpp)
target_include_directories(api_contract_compile_cases PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../../examples/api_contract")
target_link_libraries(api_contract_compile_cases PRIVATE hical::hical_core)
if(MSVC)
    target_compile_options(api_contract_compile_cases PRIVATE /utf-8)
endif()

enable_testing()
add_test(NAME ApiContract.SyncDispatch COMMAND api_contract_compile_cases)
