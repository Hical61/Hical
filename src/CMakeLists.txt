# Hical 核心库源码

# 从 CMake 版本号生成 Version.h（唯一版本号来源）
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/core/Version.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/core/Version.h
    @ONLY
)

# picohttpparser（H2O 出品的轻量 HTTP/1.1 解析器）
if(HICAL_USE_SYSTEM_PICOHTTPPARSER)
    find_path(PICOHTTPPARSER_INCLUDE_DIR picohttpparser.h REQUIRED)
    find_library(PICOHTTPPARSER_LIBRARY picohttpparser REQUIRED)
else()
    set(PICOHTTPPARSER_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/picohttpparser)
    if(NOT EXISTS "${PICOHTTPPARSER_DIR}/picohttpparser.c")
        message(FATAL_ERROR "bundled picohttpparser not found at ${PICOHTTPPARSER_DIR}")
    endif()
endif()

# 核心库实现（仅支持静态库，vcpkg/Conan 均强制 ONLY_STATIC_LIBRARY）
add_library(hical_core STATIC
    core/InetAddress.cpp
    core/Error.cpp
    core/MetaJsonError.cpp
    core/Log.cpp
    core/LogSink.cpp
    core/LogFile.cpp
    core/AsyncFileSink.cpp
    core/LogFormatter.cpp
    core/LogChannel.cpp
    core/LogMiddleware.cpp
    core/LogAdmin.cpp
    core/MemoryPool.cpp
    core/SslContext.cpp
    core/ChunkedBody.cpp
    core/HttpRequest.cpp
    core/HttpResponse.cpp
    core/Router.cpp
    core/RouteGroup.cpp
    core/Middleware.cpp
    core/HttpServer.cpp
    core/HttpSessionImpl.cpp
    core/ReadBufferPool.cpp
    core/IdleScanner.cpp
    core/WsDeflate.cpp
    core/WebSocket.cpp
    core/WsHub.cpp
    core/Multipart.cpp
    core/Session.cpp
    core/SseSession.cpp
    core/GzipCompression.cpp
    core/RateLimiter.cpp
    core/ConfigLoader.cpp
    core/Helmet.cpp
    core/HealthEndpoint.cpp
    core/JwtAuth.cpp
    core/NumaTopology.cpp
    asio/AsioEventLoop.cpp
    asio/AsioTimer.cpp
    asio/GenericConnection.cpp
    asio/EventLoopPool.cpp
    asio/TcpServer.cpp
)

# picohttpparser 条件集成
if(HICAL_USE_SYSTEM_PICOHTTPPARSER)
    target_include_directories(hical_core PRIVATE ${PICOHTTPPARSER_INCLUDE_DIR})
    target_link_libraries(hical_core PRIVATE ${PICOHTTPPARSER_LIBRARY})
    set(HICAL_PICOHTTPPARSER_INCLUDE_DIR "${PICOHTTPPARSER_INCLUDE_DIR}" CACHE INTERNAL "")
else()
    target_sources(hical_core PRIVATE ${PICOHTTPPARSER_DIR}/picohttpparser.c)
    target_include_directories(hical_core PRIVATE ${PICOHTTPPARSER_DIR})
    set(HICAL_PICOHTTPPARSER_INCLUDE_DIR "${PICOHTTPPARSER_DIR}" CACHE INTERNAL "")
endif()

# GCC: Boost.Asio awaitable_frame_base 使用 aligned_alloc/operator delete 导致误报
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set_source_files_properties(core/HttpSessionImpl.cpp PROPERTIES
        COMPILE_OPTIONS "-Wno-mismatched-new-delete"
    )
endif()

target_include_directories(hical_core
    PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
    $<INSTALL_INTERFACE:include/hical>
)

target_link_libraries(hical_core
    PUBLIC
    Boost::system
    Boost::json
    OpenSSL::SSL
    OpenSSL::Crypto
    ZLIB::ZLIB
)

# Windows 需要链接 Winsock
if(WIN32)
    target_link_libraries(hical_core PRIVATE ws2_32 mswsock)
endif()

# Linux 需要链接 liburing（Boost.Asio random_access_file 依赖 io_uring）
# GitHub Actions 自 2025-04 起封禁 io_uring syscall，CI 中需 -DHICAL_DISABLE_IO_URING=ON
if(UNIX AND NOT APPLE)
    option(HICAL_DISABLE_IO_URING "Disable io_uring backend (for environments where io_uring is blocked)" OFF)

    if(NOT HICAL_DISABLE_IO_URING)
        find_library(URING_LIB uring)
        if(URING_LIB)
            target_link_libraries(hical_core PRIVATE ${URING_LIB})
            target_compile_definitions(hical_core PUBLIC BOOST_ASIO_HAS_IO_URING)
        endif()
    else()
        message(STATUS "io_uring disabled via HICAL_DISABLE_IO_URING")
    endif()
endif()

# 内存池追踪统计编译定义（PUBLIC：头文件 TrackedResource 条件编译需全局一致，避免 ODR 违规）
if(HICAL_ENABLE_MEMORY_TRACKING)
    target_compile_definitions(hical_core PUBLIC HICAL_ENABLE_MEMORY_TRACKING)
endif()

# 中间件性能剖析编译定义
if(HICAL_ENABLE_MIDDLEWARE_PROFILING)
    target_compile_definitions(hical_core PUBLIC HICAL_ENABLE_MIDDLEWARE_PROFILING)
endif()

# 内存分配追踪（默认开启以保持兼容性；生产高性能场景可通过 -DHICAL_ENABLE_MEMORY_TRACKING=OFF 关闭）
# 开启时每个计数器 alignas(64) 独占 cache line，消除 false sharing
option(HICAL_ENABLE_MEMORY_TRACKING "Enable memory allocation statistics (alignas(64) per counter, disable for zero-overhead in production)" ON)
if(HICAL_ENABLE_MEMORY_TRACKING)
    target_compile_definitions(hical_core PUBLIC HICAL_ENABLE_MEMORY_TRACKING)
endif()

# 数据库中间件（可选）
# 分为两层：
#   1. DB 核心（连接池/中间件/查询日志）— 后端无关，仅依赖 DbConnection 抽象
#   2. MySQL 后端 — 依赖 boost::mysql（header-only）+ boost::charconv（需链接）
# 将来添加 PostgreSQL 后端时，只需在此处新增一个 if(HICAL_WITH_PGSQL) 块
if(HICAL_WITH_DATABASE)
    # DB 核心（后端无关）
    target_sources(hical_core PRIVATE
        db/DbConnectionPool.cpp
        db/DbQueryLog.cpp
    )
    target_compile_definitions(hical_core PUBLIC HICAL_HAS_DATABASE=1)

    # MySQL 后端
    target_sources(hical_core PRIVATE
        db/MysqlConnection.cpp
        db/StmtCache.cpp
    )
    # boost::mysql 的 client-side SQL formatting 依赖 boost::charconv
    target_link_libraries(hical_core PUBLIC Boost::charconv)
endif()

# OpenAPI 自动生成（可选，默认开启，选项在根 CMakeLists.txt 中定义）
if(HICAL_WITH_OPENAPI)
    target_sources(hical_core PRIVATE
        core/OpenApiRegistry.cpp
        core/OpenApiDocument.cpp
    )
    target_compile_definitions(hical_core PUBLIC HICAL_HAS_OPENAPI=1)
endif()

# NUMA 感知（可选，Linux + libnuma）
if(HICAL_WITH_NUMA)
    target_include_directories(hical_core PRIVATE ${NUMA_INCLUDE_DIR})
    target_link_libraries(hical_core PRIVATE ${NUMA_LIB})
    target_compile_definitions(hical_core PUBLIC HICAL_HAS_NUMA=1)
endif()

# mimalloc（可选，替代系统 malloc 作为 PMR upstream 分配器）
if(HICAL_WITH_MIMALLOC)
    find_package(mimalloc CONFIG QUIET)
    if(TARGET mimalloc-static)
        target_link_libraries(hical_core PRIVATE mimalloc-static)
    else()
        find_library(MIMALLOC_LIB NAMES mimalloc mimalloc-static)
        if(NOT MIMALLOC_LIB)
            message(FATAL_ERROR "mimalloc not found, install libmimalloc-dev or set HICAL_WITH_MIMALLOC=OFF")
        endif()
        target_link_libraries(hical_core PRIVATE ${MIMALLOC_LIB})
    endif()
    target_compile_definitions(hical_core PRIVATE HICAL_USE_MIMALLOC=1)
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
    FILES_MATCHING PATTERN "*.h" PATTERN "*.hci"
)

# 数据库头文件安装（可选）
if(HICAL_WITH_DATABASE)
    install(
        DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/db/
        DESTINATION include/hical/db
        FILES_MATCHING PATTERN "*.h"
    )
endif()
