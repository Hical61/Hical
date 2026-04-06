# Hical 核心库源码

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
    asio/AsioEventLoop.cpp
    asio/AsioTimer.cpp
    asio/GenericConnection.cpp
    asio/EventLoopPool.cpp
    asio/TcpServer.cpp
)

target_include_directories(hical_core
    PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(hical_core
    PUBLIC
    Boost::system
    Boost::json
    OpenSSL::SSL
    OpenSSL::Crypto
)
