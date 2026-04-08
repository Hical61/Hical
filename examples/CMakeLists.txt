# 示例程序

# 平台相关库
if(WIN32)
    set(PLATFORM_LIBS ws2_32 mswsock)
else()
    set(PLATFORM_LIBS "")
endif()

# Echo Server 示例（协程式，阶段一验证用）
add_executable(echo_server echo_server.cpp)
target_link_libraries(echo_server
    PRIVATE
    hical_core
    ${PLATFORM_LIBS}
)

# pmr 内存池 PoC（阶段一验证用）
add_executable(pmr_poc pmr_poc.cpp)
target_link_libraries(pmr_poc
    PRIVATE
    hical_core
    ${PLATFORM_LIBS}
)

# 压力测试工具
add_executable(benchmark benchmark.cpp)
target_link_libraries(benchmark
    PRIVATE
    hical_core
    ${PLATFORM_LIBS}
)

# HTTP Server 示例（阶段三验证）
add_executable(http_server http_server.cpp)
target_link_libraries(http_server
    PRIVATE
    hical_core
    ${PLATFORM_LIBS}
)

# HTTP 基准测试工具（阶段五）
add_executable(http_benchmark http_benchmark.cpp)
target_link_libraries(http_benchmark
    PRIVATE
    hical_core
    ${PLATFORM_LIBS}
)

# pmr 内存池基准测试（阶段五）
add_executable(pmr_benchmark pmr_benchmark.cpp)
target_link_libraries(pmr_benchmark
    PRIVATE
    hical_core
    ${PLATFORM_LIBS}
)

# 反射路由 + JSON 序列化示例
add_executable(reflection_server reflection_server.cpp)
target_link_libraries(reflection_server
    PRIVATE
    hical_core
    ${PLATFORM_LIBS}
)
