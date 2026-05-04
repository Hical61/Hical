# Integration Guide: Using Hical as a Library

## Method 1: vcpkg Overlay Port (Recommended for local development)

### Step 1 — Clone the repository

```bash
git clone https://github.com/your-org/hical.git /path/to/hical
```

### Step 2 — Install via vcpkg with overlay

```bash
vcpkg install hical61-hical --overlay-ports=/path/to/hical/ports/hical61-hical
```

### Step 3 — Use in your CMakeLists.txt

```cmake
find_package(hical CONFIG REQUIRED)

add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE hical::hical_core)
```

### Step 4 — Configure with vcpkg toolchain

```bash
cmake -B build -DCMAKE_TOOLCHAIN_FILE=/path/to/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build
```

---

## Method 2: CMake FetchContent (No vcpkg required)

Add the following to your `CMakeLists.txt` before `add_executable`:

```cmake
include(FetchContent)

FetchContent_Declare(
    hical
    GIT_REPOSITORY https://github.com/Hical61/Hical.git
    GIT_TAG        main  # 或锁定到具体版本 tag（见 GitHub Releases）
)

# Disable tests and examples to speed up the build
set(HICAL_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(HICAL_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
# set(HICAL_WITH_DATABASE ON CACHE BOOL "" FORCE)  # 可选：启用数据库中间件

FetchContent_MakeAvailable(hical)

# Now link against hical
target_link_libraries(my_app PRIVATE hical::hical_core)
```

> **Note**: FetchContent requires Boost (asio, beast, system, json) and OpenSSL to be
> installed on the host machine and discoverable by CMake's `find_package`.

---

## Method 3: cmake --install + find_package (Pre-built)

### Build and install hical

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release \
      -DHICAL_BUILD_TESTS=OFF \
      -DHICAL_BUILD_EXAMPLES=OFF \
      -DCMAKE_INSTALL_PREFIX=/usr/local
cmake --build build
cmake --install build
```

### Use in your project

```cmake
find_package(hical CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE hical::hical_core)
```

---

## OpenAPI Integration (3 Steps)

Enable `HICAL_WITH_OPENAPI=ON` (default) and follow the three-step pattern:

### Step 1 — Define DTOs with `HICAL_JSON`

```cpp
#include <hical/core/MetaJson.h>
#include <hical/core/OpenApiSchema.h>

HICAL_SCHEMA_NAME(UserDTO, "User")

struct UserDTO
{
    HICAL_JSON(UserDTO, REQUIRED(id), name)
    int id;
    std::string name;
};
```

### Step 2 — Annotate routes and register with OpenAPI registry

```cpp
#include <hical/core/MetaRoutes.h>
#include <hical/core/OpenApiRegistry.h>

struct UserHandler
{
    HICAL_HANDLER(Get, "/api/users/{id}", getUser)
    hical::Awaitable<hical::HttpResponse> getUser(const hical::HttpRequest& req)
    {
        co_return hical::HttpResponse::json({{"id", req.param("id")}});
    }

    static std::vector<hical::RouteApiInfo> routeApiTable()
    {
        return {
            HICAL_API(
                hical::builder::summary("获取用户"),
                hical::builder::tag("用户管理"),
                hical::builder::response<UserDTO>(200)
            ),
        };
    }

    HICAL_ROUTES_WITH_API(UserHandler, getUser)
};
```

### Step 3 — Assemble document and expose endpoints

```cmake
# CMakeLists.txt — 无需额外依赖，HICAL_WITH_OPENAPI 默认已开启
target_link_libraries(my_app PRIVATE hical::hical_core)
```

```cpp
#include <hical/core/OpenApiDocument.h>
#include <hical/core/OpenApiEndpoint.h>

int main()
{
    hical::HttpServer server(8080);
    auto& router = server.router();

    auto registry = std::make_shared<hical::OpenApiRegistry>();
    hical::OpenApiConfig config{"My API", "1.0.0"};
    auto doc = std::make_shared<hical::OpenApiDocument>(config, *registry);

    UserHandler handler;
    hical::registerRoutesWithOpenApi(router, handler, *registry);

    std::map<std::string, boost::json::object> schemas;
    hical::collectSchemas<UserDTO>(schemas);
    doc->addSchemas(schemas);

    hical::serveOpenApi(router, doc);  // 注册 /openapi.json + /docs

    server.start();
}
```

Visit `http://localhost:8080/docs` to open Swagger UI, and `http://localhost:8080/openapi.json` for the OpenAPI 3.0 JSON spec.

---

## Log System Integration

The log system is part of the core module and requires no extra compile flags.

### Basic Setup

```cpp
#include <hical/core/Log.h>

int main()
{
    auto& logger = hical::Logger::instance();
    logger.setLevel(hical::LogLevel::EInfo);
    logger.addSink(std::make_shared<hical::StderrSink>());

    HICAL_LOG_INFO("Application started, port={}", 8080);

    // ... your application code ...
}
```

### File Logging with Rotation

```cpp
#include <hical/core/Log.h>
#include <hical/core/LogSink.h>
#include <hical/core/AsyncFileSink.h>

auto& logger = hical::Logger::instance();

// Async file sink (recommended for production)
// 100MB per file, keep 10 rotated files
auto asyncSink = std::make_shared<hical::AsyncFileSink>(
    "./logs/app.log", 100 * 1024 * 1024, 10);
logger.addSink(asyncSink);
```

### HTTP Middleware Integration

```cpp
#include <hical/core/LogMiddleware.h>
#include <hical/core/LogAdmin.h>

// Add log middleware (auto trace-id + access log)
server.use(hical::makeLogMiddleware());

// Add dynamic log level endpoints
hical::registerLogAdmin(server.router());
// GET  /admin/log-level — query current levels
// PUT  /admin/log-level — adjust levels at runtime
```

---

## CORS & Route Group Integration

### CORS Middleware

```cpp
#include <hical/core/Cors.h>

// Default: allow all origins
server.use(hical::makeCorsMiddleware());

// Or configure precisely
hical::CorsOptions opts;
opts.allowedOrigins = {"https://example.com"};
opts.allowCredentials = true;
server.use(hical::makeCorsMiddleware(opts));
```

### Route Groups

```cpp
#include <hical/core/RouteGroup.h>

auto api = server.router().group("/api/v1");
api.use(authMiddleware);  // Group-level middleware

api.get("/users", listUsers);        // → GET /api/v1/users
api.post("/users", createUser);      // → POST /api/v1/users
api.get("/users/{id}", getUser);     // → GET /api/v1/users/{id}

auto admin = api.group("/admin");    // Nested group
admin.get("/stats", getStats);       // → GET /api/v1/admin/stats
```

---

## Quick Example

```cpp
#include <hical/core/HttpServer.h>

int main()
{
    hical::HttpServer server(8080);

    server.router().get("/hello",
        [](const hical::HttpRequest& req) -> hical::HttpResponse {
            return hical::HttpResponse::ok("Hello from Hical!");
        });

    server.start();
}
```

```cmake
cmake_minimum_required(VERSION 3.20)
project(my_app CXX)
set(CMAKE_CXX_STANDARD 20)

find_package(hical CONFIG REQUIRED)

add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE hical::hical_core)
```
