cmake_minimum_required(VERSION 3.28)
project(PackageTest CXX)

set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

find_package(hical REQUIRED)

add_executable(example src/example.cpp)
target_link_libraries(example PRIVATE hical::hical_core)
target_compile_features(example PRIVATE cxx_std_20)
