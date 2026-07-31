# 测试程序

# ASan/UBSan 构建下 test binary 启动较慢，默认 5 秒 discovery timeout 不够，统一提高到 30 秒
include(GoogleTest)
set(HICAL_TEST_DISCOVERY_TIMEOUT 30)

# 使用 TestHttpClient.h 的测试需要 picohttpparser include 路径
# （picohttpparser 是 hical_core 的 PRIVATE 实现细节，测试直接引用需单独声明）
set(TESTS_NEEDING_PICOHTTPPARSER
    test_http_server
    test_websocket
    test_ws_advanced
    test_integration
    test_graceful_shutdown
    test_error_handler
    test_expect_continue
    test_chunked_sse
    test_http_client
    test_http_server_perf
    test_optimistic_write
)

# helper：封装三行样板（add_executable + target_link_libraries + gtest_discover_tests）
function(hical_add_test name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE hical_core GTest::gtest_main)
    gtest_discover_tests(${name} DISCOVERY_TIMEOUT ${HICAL_TEST_DISCOVERY_TIMEOUT})
endfunction()

# --- 核心 / 网络层 ---
hical_add_test(test_basic)                # 基础功能冒烟测试
hical_add_test(test_error)                # 错误码
hical_add_test(test_asio_event_loop)      # AsioEventLoop
hical_add_test(test_asio_tcp_connection)  # GenericConnection (PlainConnection)
hical_add_test(test_memory_pool)          # MemoryPool 和 PmrBuffer
hical_add_test(test_asio_timer)           # AsioTimer
hical_add_test(test_ssl_connection)       # SSL 连接
hical_add_test(test_coroutine)            # 协程工具
hical_add_test(test_tcp_server)           # TcpServer + EventLoopPool
hical_add_test(test_numa_topology)        # NUMA 拓扑检测与绑核验证

# --- HTTP 层 ---
hical_add_test(test_http_types)           # HTTP 类型
hical_add_test(test_router)               # 路由器
hical_add_test(test_route_group)          # 路由组
hical_add_test(test_middleware)           # 中间件
hical_add_test(test_http_server)          # HttpServer 集成
hical_add_test(test_websocket)            # WebSocket
hical_add_test(test_ws_advanced)          # WebSocket 高级功能（Binary/Close/Subprotocol/Context/Param/Hub/Heartbeat）
hical_add_test(test_router_perf)          # 路由性能
hical_add_test(test_perfect_hash_router)  # PerfectHashRouter 正确性和性能
hical_add_test(test_concepts)             # Concepts 编译期验证
hical_add_test(test_reflection)           # 反射层（MetaJson + MetaRoutes）
hical_add_test(test_integration)          # 端到端集成
hical_add_test(test_cookie)               # Cookie 解析与 Set-Cookie
hical_add_test(test_static_files)         # 静态文件服务、ETag、路径遍历防护
hical_add_test(test_multipart)            # multipart/form-data 上传解析
hical_add_test(test_session)              # Session 生命周期与线程安全
hical_add_test(test_query_params)         # 查询参数解析
hical_add_test(test_form_params)          # 表单参数解析
hical_add_test(test_redirect)             # 重定向便捷方法
hical_add_test(test_cors)                 # CORS 中间件
hical_add_test(test_rate_limiter)         # 速率限制（Token Bucket）
hical_add_test(test_helmet)              # 安全头中间件
hical_add_test(test_health)              # 健康检查端点
hical_add_test(test_config_loader)       # 配置加载器
hical_add_test(test_jwt_auth)           # JWT Auth 中间件
hical_add_test(test_wildcard_route)       # 通配路由 `*path`
hical_add_test(test_compression)          # 响应压缩（gzip）
hical_add_test(test_chunked_sse)          # Chunked Transfer-Encoding + SSE
hical_add_test(test_graceful_shutdown)    # 优雅关机与信号处理
hical_add_test(test_error_handler)        # 全局错误处理器
hical_add_test(test_expect_continue)      # Expect: 100-continue
hical_add_test(test_http_server_perf)     # HttpServer 吞量基准测试
hical_add_test(test_http_client)          # 测试客户端（链式调用）
hical_add_test(test_optimistic_write)     # 乐观同步写 + readBuf 提前归还
hical_add_test(test_compile_time_chain)  # 编译期中间件链与运行时 buildOptimizedChain 语义一致性

# --- 日志系统 ---
hical_add_test(test_log)                  # 日志系统主测试
hical_add_test(test_log_ndebug)           # 验证 TRACE 在 NDEBUG 下编译期消除
target_compile_definitions(test_log_ndebug PRIVATE NDEBUG)
hical_add_test(test_fixed_buffer)         # FixedBuffer 栈缓冲
hical_add_test(test_read_buffer_pool)     # ReadBufferPool 借还语义
hical_add_test(test_log_file)             # LogFile 文件轮转
hical_add_test(test_async_file_sink)      # AsyncFileSink 异步双缓冲
hical_add_test(test_log_formatter)        # TextFormatter + JsonFormatter
hical_add_test(test_log_channel)          # LogChannel 多通道
hical_add_test(test_log_middleware)       # LogMiddleware（trace-id 注入）
hical_add_test(test_log_admin)            # LogAdmin 动态级别端点

# --- OpenAPI（可选，仅当 HICAL_WITH_OPENAPI=ON）---
if(HICAL_WITH_OPENAPI)
    hical_add_test(test_openapi)          # OpenAPI 自动生成
endif()

# --- 数据库中间件（可选，仅当 HICAL_WITH_DATABASE=ON）---
if(HICAL_WITH_DATABASE)
    hical_add_test(test_db_pool)          # 连接池（Mock 连接，不依赖真实 DB）
    hical_add_test(test_db_middleware)    # DB 中间件（Mock 连接池）
    hical_add_test(test_stmt_cache)       # PreparedStatement LRU 缓存
    hical_add_test(test_db_query_log)     # 查询日志中间件
    hical_add_test(test_mysql_integration) # MySQL 集成（无实例时自动 SKIP）
endif()

# picohttpparser include 路径注入（TestHttpClient.h 直接引用 picohttpparser.h）
foreach(_test IN LISTS TESTS_NEEDING_PICOHTTPPARSER)
    target_include_directories(${_test} PRIVATE ${HICAL_PICOHTTPPARSER_INCLUDE_DIR})
endforeach()
