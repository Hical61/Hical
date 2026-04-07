# 测试程序

# 平台相关库
if(WIN32)
    set(PLATFORM_LIBS ws2_32 mswsock)
else()
    set(PLATFORM_LIBS "")
endif()

# 基础测试
add_executable(test_basic test_basic.cpp)
target_link_libraries(test_basic
    PRIVATE
    hical_core
    GTest::gtest_main
    ${PLATFORM_LIBS}
)

# 错误码测试
add_executable(test_error test_error.cpp)
target_link_libraries(test_error
    PRIVATE
    hical_core
    GTest::gtest_main
    ${PLATFORM_LIBS}
)

# AsioEventLoop 测试
add_executable(test_asio_event_loop test_asio_event_loop.cpp)
target_link_libraries(test_asio_event_loop
    PRIVATE
    hical_core
    GTest::gtest_main
    ${PLATFORM_LIBS}
)

# GenericConnection (PlainConnection) 测试
add_executable(test_asio_tcp_connection test_asio_tcp_connection.cpp)
target_link_libraries(test_asio_tcp_connection
    PRIVATE
    hical_core
    GTest::gtest_main
    ${PLATFORM_LIBS}
)

# MemoryPool 和 PmrBuffer 测试
add_executable(test_memory_pool test_memory_pool.cpp)
target_link_libraries(test_memory_pool
    PRIVATE
    hical_core
    GTest::gtest_main
    ${PLATFORM_LIBS}
)

# AsioTimer 测试
add_executable(test_asio_timer test_asio_timer.cpp)
target_link_libraries(test_asio_timer
    PRIVATE
    hical_core
    GTest::gtest_main
    ${PLATFORM_LIBS}
)

# SSL 连接测试
add_executable(test_ssl_connection test_ssl_connection.cpp)
target_link_libraries(test_ssl_connection
    PRIVATE
    hical_core
    GTest::gtest_main
    ${PLATFORM_LIBS}
)

# 协程工具测试
add_executable(test_coroutine test_coroutine.cpp)
target_link_libraries(test_coroutine
    PRIVATE
    hical_core
    GTest::gtest_main
    ${PLATFORM_LIBS}
)

# HTTP 类型测试
add_executable(test_http_types test_http_types.cpp)
target_link_libraries(test_http_types
    PRIVATE
    hical_core
    GTest::gtest_main
    ${PLATFORM_LIBS}
)

# 路由器测试
add_executable(test_router test_router.cpp)
target_link_libraries(test_router
    PRIVATE
    hical_core
    GTest::gtest_main
    ${PLATFORM_LIBS}
)

# TcpServer + EventLoopPool 测试
add_executable(test_tcp_server test_tcp_server.cpp)
target_link_libraries(test_tcp_server
    PRIVATE
    hical_core
    GTest::gtest_main
    ${PLATFORM_LIBS}
)

# 中间件测试
add_executable(test_middleware test_middleware.cpp)
target_link_libraries(test_middleware
    PRIVATE
    hical_core
    GTest::gtest_main
    ${PLATFORM_LIBS}
)

# HttpServer 集成测试
add_executable(test_http_server test_http_server.cpp)
target_link_libraries(test_http_server
    PRIVATE
    hical_core
    GTest::gtest_main
    ${PLATFORM_LIBS}
)

# WebSocket 测试
add_executable(test_websocket test_websocket.cpp)
target_link_libraries(test_websocket
    PRIVATE
    hical_core
    GTest::gtest_main
    ${PLATFORM_LIBS}
)

# 路由性能测试（阶段五）
add_executable(test_router_perf test_router_perf.cpp)
target_link_libraries(test_router_perf
    PRIVATE
    hical_core
    GTest::gtest_main
    ${PLATFORM_LIBS}
)

# Concepts 编译期验证测试（阶段六）
add_executable(test_concepts test_concepts.cpp)
target_link_libraries(test_concepts
    PRIVATE
    hical_core
    GTest::gtest_main
    ${PLATFORM_LIBS}
)

# 集成测试（阶段六）
add_executable(test_integration test_integration.cpp)
target_link_libraries(test_integration
    PRIVATE
    hical_core
    GTest::gtest_main
    ${PLATFORM_LIBS}
)

# 注册测试
include(GoogleTest)
gtest_discover_tests(test_basic)
gtest_discover_tests(test_error)
gtest_discover_tests(test_asio_event_loop)
gtest_discover_tests(test_asio_tcp_connection)
gtest_discover_tests(test_memory_pool)
gtest_discover_tests(test_asio_timer)
gtest_discover_tests(test_ssl_connection)
gtest_discover_tests(test_coroutine)
gtest_discover_tests(test_http_types)
gtest_discover_tests(test_router)
gtest_discover_tests(test_tcp_server)
gtest_discover_tests(test_middleware)
gtest_discover_tests(test_http_server)
gtest_discover_tests(test_websocket)
gtest_discover_tests(test_router_perf)
gtest_discover_tests(test_concepts)
gtest_discover_tests(test_integration)
