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

## Minimum Example

```cpp
#include <hical/core/HttpServer.h>

int main()
{
    hical::HttpServer server("0.0.0.0", 8080);

    server.get("/hello", [](const hical::HttpRequest& req) -> hical::Awaitable<hical::HttpResponse>
    {
        co_return hical::HttpResponse::ok("Hello from Hical!");
    });

    server.run();
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
