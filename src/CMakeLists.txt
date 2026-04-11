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

target_include_directories(hical_core
    PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(hical_core
    PUBLIC
    Boost::system
    Boost::json
    OpenSSL::SSL
    OpenSSL::Crypto
)

# 安装库和头文件
install(
    TARGETS hical_core
    EXPORT hicalTargets
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
)

install(
    DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/core/
    DESTINATION include/hical/core
    FILES_MATCHING PATTERN "*.h"
)

install(
    DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/asio/
    DESTINATION include/hical/asio
    FILES_MATCHING PATTERN "*.h"
)
