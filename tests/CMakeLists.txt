# 测试程序

# 基础测试
add_executable(test_basic test_basic.cpp)
target_link_libraries(test_basic PRIVATE hical_core GTest::gtest_main)

# 错误码测试
add_executable(test_error test_error.cpp)
target_link_libraries(test_error PRIVATE hical_core GTest::gtest_main)

# AsioEventLoop 测试
add_executable(test_asio_event_loop test_asio_event_loop.cpp)
target_link_libraries(test_asio_event_loop PRIVATE hical_core GTest::gtest_main)

# GenericConnection (PlainConnection) 测试
add_executable(test_asio_tcp_connection test_asio_tcp_connection.cpp)
target_link_libraries(test_asio_tcp_connection PRIVATE hical_core GTest::gtest_main)

# MemoryPool 和 PmrBuffer 测试
add_executable(test_memory_pool test_memory_pool.cpp)
target_link_libraries(test_memory_pool PRIVATE hical_core GTest::gtest_main)

# AsioTimer 测试
add_executable(test_asio_timer test_asio_timer.cpp)
target_link_libraries(test_asio_timer PRIVATE hical_core GTest::gtest_main)

# SSL 连接测试
add_executable(test_ssl_connection test_ssl_connection.cpp)
target_link_libraries(test_ssl_connection PRIVATE hical_core GTest::gtest_main)

# 协程工具测试
add_executable(test_coroutine test_coroutine.cpp)
target_link_libraries(test_coroutine PRIVATE hical_core GTest::gtest_main)

# HTTP 类型测试
add_executable(test_http_types test_http_types.cpp)
target_link_libraries(test_http_types PRIVATE hical_core GTest::gtest_main)

# 路由器测试
add_executable(test_router test_router.cpp)
target_link_libraries(test_router PRIVATE hical_core GTest::gtest_main)

# 路由组测试
add_executable(test_route_group test_route_group.cpp)
target_link_libraries(test_route_group PRIVATE hical_core GTest::gtest_main)

# TcpServer + EventLoopPool 测试
add_executable(test_tcp_server test_tcp_server.cpp)
target_link_libraries(test_tcp_server PRIVATE hical_core GTest::gtest_main)

# 中间件测试
add_executable(test_middleware test_middleware.cpp)
target_link_libraries(test_middleware PRIVATE hical_core GTest::gtest_main)

# HttpServer 集成测试
add_executable(test_http_server test_http_server.cpp)
target_link_libraries(test_http_server PRIVATE hical_core GTest::gtest_main)

# WebSocket 测试
add_executable(test_websocket test_websocket.cpp)
target_link_libraries(test_websocket PRIVATE hical_core GTest::gtest_main)

# 路由性能测试（阶段五）
add_executable(test_router_perf test_router_perf.cpp)
target_link_libraries(test_router_perf PRIVATE hical_core GTest::gtest_main)

# Concepts 编译期验证测试（阶段六）
add_executable(test_concepts test_concepts.cpp)
target_link_libraries(test_concepts PRIVATE hical_core GTest::gtest_main)

# 反射层测试（MetaJson + MetaRoutes）
add_executable(test_reflection test_reflection.cpp)
target_link_libraries(test_reflection PRIVATE hical_core GTest::gtest_main)

# 集成测试（阶段六）
add_executable(test_integration test_integration.cpp)
target_link_libraries(test_integration PRIVATE hical_core GTest::gtest_main)

# Cookie 测试
add_executable(test_cookie test_cookie.cpp)
target_link_libraries(test_cookie PRIVATE hical_core GTest::gtest_main)

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
gtest_discover_tests(test_route_group)
gtest_discover_tests(test_tcp_server)
gtest_discover_tests(test_middleware)
gtest_discover_tests(test_http_server)
gtest_discover_tests(test_websocket)
gtest_discover_tests(test_router_perf)
gtest_discover_tests(test_concepts)
gtest_discover_tests(test_reflection)
gtest_discover_tests(test_integration)
gtest_discover_tests(test_cookie)

# 静态文件服务测试
add_executable(test_static_files test_static_files.cpp)
target_link_libraries(test_static_files PRIVATE hical_core GTest::gtest_main)
gtest_discover_tests(test_static_files)

# 文件上传测试
add_executable(test_multipart test_multipart.cpp)
target_link_libraries(test_multipart PRIVATE hical_core GTest::gtest_main)
gtest_discover_tests(test_multipart)

# Session 测试
add_executable(test_session test_session.cpp)
target_link_libraries(test_session PRIVATE hical_core GTest::gtest_main)
gtest_discover_tests(test_session)

# 查询参数解析测试
add_executable(test_query_params test_query_params.cpp)
target_link_libraries(test_query_params PRIVATE hical_core GTest::gtest_main)
gtest_discover_tests(test_query_params)

# 表单参数解析测试
add_executable(test_form_params test_form_params.cpp)
target_link_libraries(test_form_params PRIVATE hical_core GTest::gtest_main)
gtest_discover_tests(test_form_params)

# 重定向便捷方法测试
add_executable(test_redirect test_redirect.cpp)
target_link_libraries(test_redirect PRIVATE hical_core GTest::gtest_main)
gtest_discover_tests(test_redirect)

# CORS 中间件测试
add_executable(test_cors test_cors.cpp)
target_link_libraries(test_cors PRIVATE hical_core GTest::gtest_main)
gtest_discover_tests(test_cors)

# 优雅关机与信号处理测试
add_executable(test_graceful_shutdown test_graceful_shutdown.cpp)
target_link_libraries(test_graceful_shutdown PRIVATE hical_core GTest::gtest_main)
gtest_discover_tests(test_graceful_shutdown)

# 全局错误处理器测试
add_executable(test_error_handler test_error_handler.cpp)
target_link_libraries(test_error_handler PRIVATE hical_core GTest::gtest_main)
gtest_discover_tests(test_error_handler)

# 数据库中间件测试（可选，仅当 HICAL_WITH_DATABASE=ON）
if (HICAL_WITH_DATABASE)
    # 连接池测试（使用 Mock 连接，不依赖真实数据库）
    add_executable(test_db_pool test_db_pool.cpp)
    target_link_libraries(test_db_pool PRIVATE hical_core GTest::gtest_main)
    gtest_discover_tests(test_db_pool)

    # DB 中间件测试（使用 Mock 连接池）
    add_executable(test_db_middleware test_db_middleware.cpp)
    target_link_libraries(test_db_middleware PRIVATE hical_core GTest::gtest_main)
    gtest_discover_tests(test_db_middleware)

    # PreparedStatement 缓存 LRU 测试
    add_executable(test_stmt_cache test_stmt_cache.cpp)
    target_link_libraries(test_stmt_cache PRIVATE hical_core GTest::gtest_main)
    gtest_discover_tests(test_stmt_cache)

    # 查询日志中间件测试
    add_executable(test_db_query_log test_db_query_log.cpp)
    target_link_libraries(test_db_query_log PRIVATE hical_core GTest::gtest_main)
    gtest_discover_tests(test_db_query_log)

    # MySQL 集成测试（需要真实 MySQL 实例，连接失败时自动 SKIP）
    add_executable(test_mysql_integration test_mysql_integration.cpp)
    target_link_libraries(test_mysql_integration PRIVATE hical_core GTest::gtest_main)
    gtest_discover_tests(test_mysql_integration)
endif()

