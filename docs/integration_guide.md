# Integration Guide: Using Hical as a Library

## Quick Example

A minimal project using Hical:

**main.cpp**
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

**CMakeLists.txt**
```cmake
cmake_minimum_required(VERSION 3.20)
project(my_app CXX)
set(CMAKE_CXX_STANDARD 20)

find_package(hical CONFIG REQUIRED)

add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE hical::hical_core)
```

> **Include path convention**: After installation, use `#include <hical/core/X.h>` (e.g. `<hical/core/HttpServer.h>`, `<hical/core/Router.h>`). For database module: `<hical/db/X.h>`.

---

## Installation Methods

### Method 1: vcpkg (Recommended)

Hical is available in the official vcpkg registry.

```bash
vcpkg install hical61-hical
```

Optional features:

```bash
vcpkg install hical61-hical[database]   # Enable database middleware
```

Configure your project with vcpkg toolchain:

```bash
cmake -B build -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake
cmake --build build
```

### Method 2: CMake FetchContent

No package manager required. Add to your `CMakeLists.txt`:

```cmake
include(FetchContent)

FetchContent_Declare(
    hical
    GIT_REPOSITORY https://github.com/Hical61/Hical.git
    GIT_TAG        main  # 或锁定到具体版本 tag（见 GitHub Releases）
)

set(HICAL_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(HICAL_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
# set(HICAL_WITH_DATABASE ON CACHE BOOL "" FORCE)  # Optional: enable DB middleware

FetchContent_MakeAvailable(hical)

target_link_libraries(my_app PRIVATE hical::hical_core)
```

> **Note**: FetchContent requires Boost (asio, system, json), OpenSSL, and zlib to be
> installed on the host and discoverable by CMake's `find_package`.

### Method 3: cmake --install + find_package

Build and install Hical to a local prefix:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release \
      -DHICAL_BUILD_TESTS=OFF \
      -DHICAL_BUILD_EXAMPLES=OFF \
      -DCMAKE_INSTALL_PREFIX=/usr/local
cmake --build build
cmake --install build
```

Then in your project:

```cmake
find_package(hical CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE hical::hical_core)
```

### Method 4: Conan

Download the Conan source package from [GitHub Releases](https://github.com/Hical61/Hical/releases):

```bash
curl -LO https://github.com/Hical61/Hical/releases/download/vVERSION/hical-VERSION-conan-src.tar.gz
tar xzf hical-VERSION-conan-src.tar.gz
cd hical
conan export . --version=VERSION
conan install . --build=missing
```

---

## Feature Integration

### OpenAPI 3.0 (Auto-generated Swagger Docs)

Enable `HICAL_WITH_OPENAPI=ON` (default) and follow the three-step pattern:

**Step 1 — Define DTOs with `HICAL_JSON`**

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

**Step 2 — Annotate routes and register**

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
                hical::builder::summary("Get user by ID"),
                hical::builder::tag("Users"),
                hical::builder::response<UserDTO>(200)
            ),
        };
    }

    HICAL_ROUTES_WITH_API(UserHandler, getUser)
};
```

**Step 3 — Assemble document and expose endpoints**

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

    hical::serveOpenApi(router, doc);  // Registers /openapi.json + /docs

    server.start();
}
```

Visit `http://localhost:8080/docs` for Swagger UI, `http://localhost:8080/openapi.json` for the raw spec.

---

### Logging

The log system is part of the core module — no extra compile flags needed.

**Basic setup:**

```cpp
#include <hical/core/Log.h>

auto& logger = hical::Logger::instance();
logger.setLevel(hical::LogLevel::hInfo);
logger.addSink(std::make_shared<hical::StderrSink>());

HICAL_LOG_INFO("Application started, port={}", 8080);
```

**Async file logging with rotation (production):**

```cpp
#include <hical/core/AsyncFileSink.h>

hical::AsyncFileSink::Options asyncOpts;
asyncOpts.file.basePath = "./logs/app.log";
asyncOpts.file.maxFileSize = 100 * 1024 * 1024;  // 100MB per file
asyncOpts.file.maxFiles = 10;
logger.addSink(std::make_shared<hical::AsyncFileSink>(asyncOpts));
```

**HTTP middleware (auto trace-id + access log):**

```cpp
#include <hical/core/LogMiddleware.h>
#include <hical/core/LogAdmin.h>

server.use(hical::makeLogMiddleware());

// Dynamic log level management at runtime
hical::registerLogAdmin(server.router());
// GET  /admin/log-level — query current levels
// PUT  /admin/log-level — adjust levels at runtime
```

---

### CORS & Route Groups

**CORS Middleware:**

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

**Route Groups:**

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

### Database Middleware

Requires `HICAL_WITH_DATABASE=ON` (or vcpkg feature `database`).

```cpp
#include <hical/db/DbMiddleware.h>
#include <hical/db/MysqlConnection.h>

// Create connection factory
hical::db::DbConfig dbConfig;
dbConfig.host = "localhost";
dbConfig.port = 3306;
dbConfig.user = "root";
dbConfig.password = "secret";
dbConfig.database = "mydb";

auto factory = hical::db::MysqlConnection::makeFactory(dbConfig);
auto pool = std::make_shared<hical::db::DbConnectionPool>(factory, dbConfig);

// Register middleware (auto-acquire/release connection per request)
hical::db::DbMiddlewareOptions dbOpts;
dbOpts.autoTransaction = true;  // Auto commit/rollback
server.use(hical::db::makeDbMiddleware(pool, dbOpts));

// In route handler:
server.router().get("/users/{id}", [](const hical::HttpRequest& req)
    -> hical::Awaitable<hical::HttpResponse> {
    auto conn = hical::db::getDbConnection(req);
    auto result = co_await conn->query("SELECT * FROM users WHERE id = ?", {req.param("id")});
    co_return hical::HttpResponse::json({{"user", result.rows[0]}});
});
```

---

## CMake Options Reference

| Option                            | Default | Description                                         |
| --------------------------------- | ------- | --------------------------------------------------- |
| `HICAL_BUILD_TESTS`               | ON      | Build unit tests                                    |
| `HICAL_BUILD_EXAMPLES`            | ON      | Build example programs                              |
| `HICAL_WITH_DATABASE`             | OFF     | Enable database middleware (requires Boost >= 1.85) |
| `HICAL_WITH_OPENAPI`              | ON      | Enable OpenAPI 3.0 module                           |
| `HICAL_ENABLE_REFLECTION`         | OFF     | Enable C++26 native reflection                      |
| `HICAL_USE_SYSTEM_PICOHTTPPARSER` | OFF     | Use system picohttpparser instead of bundled        |
