# Repository Guidelines

## Project Structure & Module Organization

Hical is a C++20 HTTP framework built with CMake. Core implementation and public headers live in `src/`: `core/` contains HTTP, routing, middleware, logging, and server code; `asio/` contains event-loop and TCP primitives; `db/` is optional database middleware. Keep bundled dependencies isolated in `src/third_party/`. Tests are in `tests/`, examples in `examples/`, and longer guides in `docs/`. Docker and benchmark tooling live in `docker/` and `benchmark/`.

## Build, Test, and Development Commands

Configure a normal release build, compile it, then run all discovered GoogleTest tests:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
```

Use `-G Ninja` where Ninja is available. Enable optional features at configure time, for example `-DHICAL_WITH_DATABASE=ON`. For a CI-like Linux test matrix, run `cd docker/test && docker compose up --build --abort-on-container-exit`.

## Coding Style & Naming Conventions

Use `.clang-format`: four-space indentation, Allman braces, and a 120-column limit. Before committing, format touched C++ files with `clang-format -i path/to/file.cpp`. `.clang-tidy` checks naming: types use `CamelCase`, functions use `camelBack`, members end in `_`, globals start `g_`, constants start `k`, and macros are `UPPER_CASE`. Document public APIs with Doxygen-style `/** ... */` comments.

## Testing Guidelines

Add focused GoogleTest coverage in `tests/test_<feature>.cpp` and register it with `hical_add_test(test_<feature>)` in `tests/CMakeLists.txt`. Run the complete suite after behavior changes; use `ctest --test-dir build -R <name> --output-on-failure` while iterating. Preserve portability: CI covers GCC, Clang with ASan/UBSan, MSYS2, and MSVC. Avoid brittle timing assertions in performance tests.

## Commit & Pull Request Guidelines

Follow the existing short commit prefix convention: `[feat]`, `[fix]`, `[perf]`, `[refactor]`, `[docs]`, `[test]`, or `[chore]`, followed by an imperative summary (for example, `[fix] handle empty chunked body`). Create feature branches from `main`. PRs should explain the behavior change, link relevant issues, include tests, and add screenshots or request/response examples when UI or API behavior changes. Ensure formatting, build, and tests pass before requesting review.

## Security & Configuration

Do not commit credentials, certificates, or local environment files. Treat changes to TLS, JWT, static-file paths, headers, and database configuration as security-sensitive; update relevant tests and consult `SECURITY.md` for vulnerability reporting.
