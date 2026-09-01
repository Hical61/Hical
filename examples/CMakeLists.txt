# 示例程序

# Echo Server 示例（协程式，阶段一验证用）
add_executable(echo_server echo_server.cpp)
target_link_libraries(echo_server PRIVATE hical_core)

# pmr 内存池 PoC（阶段一验证用）
add_executable(pmr_poc pmr_poc.cpp)
target_link_libraries(pmr_poc PRIVATE hical_core)

# 压力测试工具
add_executable(benchmark benchmark.cpp)
target_link_libraries(benchmark PRIVATE hical_core)

# HTTP Server 示例（阶段三验证）
add_executable(http_server http_server.cpp)
target_link_libraries(http_server PRIVATE hical_core)

# HTTP 基准测试工具（阶段五）
add_executable(http_benchmark http_benchmark.cpp)
target_link_libraries(http_benchmark PRIVATE hical_core)

# pmr 内存池基准测试（阶段五）
add_executable(pmr_benchmark pmr_benchmark.cpp)
target_link_libraries(pmr_benchmark PRIVATE hical_core)

# 反射路由 + JSON 序列化示例
add_executable(reflection_server reflection_server.cpp)
target_link_libraries(reflection_server PRIVATE hical_core)

# 静态文件服务示例（集成 translate.js 多语言）
add_executable(static_server static_server/static_server.cpp)
target_link_libraries(static_server PRIVATE hical_core)
target_compile_definitions(static_server PRIVATE
    STATIC_SERVER_PUBLIC_DIR="${CMAKE_CURRENT_SOURCE_DIR}/static_server/public")

# OpenAPI 自动生成示例（可选，仅当 HICAL_WITH_OPENAPI=ON）
if (HICAL_WITH_OPENAPI)
    add_executable(openapi_server openapi_server.cpp)
    target_link_libraries(openapi_server PRIVATE hical_core)
endif()

# PostgreSQL 后端示例（可选，仅当 HICAL_WITH_PGSQL=ON）
if (HICAL_WITH_PGSQL)
    add_executable(pgsql_example pgsql_example.cpp)
    target_link_libraries(pgsql_example PRIVATE hical_core)
endif()

