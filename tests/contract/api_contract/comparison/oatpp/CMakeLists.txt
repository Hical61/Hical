cmake_minimum_required(VERSION 3.28)
project(hical_contract_oatpp LANGUAGES CXX)

find_package(oatpp CONFIG REQUIRED)
find_package(oatpp-swagger CONFIG REQUIRED)

add_executable(oatpp_room_contract main.cpp)
target_compile_features(oatpp_room_contract PRIVATE cxx_std_20)
target_link_libraries(oatpp_room_contract PRIVATE oatpp::oatpp oatpp::oatpp-swagger)
