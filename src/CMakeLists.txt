# Hical 核心库源码

# 从 CMake 版本号生成 Version.h（唯一版本号来源）
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/core/Version.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/core/Version.h
    @ONLY
)

# 核心库实现
add_library(hical_core
    core/InetAddress.cpp
    core/Error.cpp
    core/MemoryPool.cpp
    core/SslContext.cpp
    core/HttpRequest.cpp
    core/HttpResponse.cpp
    core/Router.cpp
    core/Middleware.cpp
    core/HttpServer.cpp
    core/WebSocket.cpp
    core/Multipart.cpp
    core/Session.cpp
    asio/AsioEventLoop.cpp
    asio/AsioTimer.cpp
    asio/GenericConnection.cpp
    asio/EventLoopPool.cpp
    asio/TcpServer.cpp
)

# Windows shared 库自动导出所有符号（生成 .lib 导入库）
set_target_properties(hical_core PROPERTIES
    WINDOWS_EXPORT_ALL_SYMBOLS ON
)

target_include_directories(hical_core
    PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
    $<INSTALL_INTERFACE:include/hical/core>
    $<INSTALL_INTERFACE:include/hical/asio>
)

target_link_libraries(hical_core
    PUBLIC
    Boost::system
    Boost::json
    OpenSSL::SSL
    OpenSSL::Crypto
)

# Windows 需要链接 Winsock
if(WIN32)
    target_link_libraries(hical_core PRIVATE ws2_32 mswsock)
endif()

# Linux 需要链接 liburing（Boost.Asio random_access_file 依赖 io_uring）
if(UNIX AND NOT APPLE)
    find_library(URING_LIB uring)
    if(URING_LIB)
        target_link_libraries(hical_core PRIVATE ${URING_LIB})
        target_compile_definitions(hical_core PUBLIC BOOST_ASIO_HAS_IO_URING)
    endif()
endif()

# 安装库和头文件
install(
    TARGETS hical_core
    EXPORT hicalTargets
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
    INCLUDES DESTINATION include
)

# ALIAS 目标，方便外部项目使用 hical::hical_core
add_library(hical::hical_core ALIAS hical_core)

install(
    DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/core/
    DESTINATION include/hical/core
    FILES_MATCHING PATTERN "*.h"
)

# 安装 CMake 生成的 Version.h
install(
    FILES ${CMAKE_CURRENT_BINARY_DIR}/core/Version.h
    DESTINATION include/hical/core
)

install(
    DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/asio/
    DESTINATION include/hical/asio
    FILES_MATCHING PATTERN "*.h"
)
