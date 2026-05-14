cmake_minimum_required(VERSION 3.20)
project(picohttpparser C)

add_library(picohttpparser picohttpparser.c)

target_include_directories(picohttpparser
    PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<INSTALL_INTERFACE:include>
)

install(TARGETS picohttpparser
    EXPORT picohttpparserTargets
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
)

install(FILES picohttpparser.h DESTINATION include)

install(EXPORT picohttpparserTargets
    FILE picohttpparserConfig.cmake
    NAMESPACE picohttpparser::
    DESTINATION lib/cmake/picohttpparser
)
