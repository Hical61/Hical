cmake_minimum_required(VERSION 3.28)
project(PackageTest CXX)

set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

find_package(hical REQUIRED)

add_executable(example src/example.cpp)
target_link_libraries(example PRIVATE hical::hical_core)
target_compile_features(example PRIVATE cxx_std_20)

foreach(feature HTTP_CLIENT HTTP2)
    if(NOT DEFINED HICAL_TEST_EXPECT_${feature})
        message(FATAL_ERROR "Set HICAL_TEST_EXPECT_${feature} from the package build options")
    endif()
    target_compile_definitions(example PRIVATE
        HICAL_TEST_EXPECT_${feature}=$<BOOL:${HICAL_TEST_EXPECT_${feature}}>)
endforeach()
