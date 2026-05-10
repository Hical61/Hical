FROM ubuntu:24.04 AS builder

RUN apt-get update && apt-get install -y --no-install-recommends \
    cmake make g++ git ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY main.cpp CMakeLists.txt ./

# FetchContent 在 configure 阶段拉取 oatpp，Release 编译
RUN cmake -B build -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build -j$(nproc) \
    && strip /src/build/bench_server

# --- 运行阶段 ---
FROM ubuntu:24.04

# oatpp 静态链接，无外部运行时依赖
COPY --from=builder /src/build/bench_server /server
EXPOSE 8085
CMD ["/server"]
