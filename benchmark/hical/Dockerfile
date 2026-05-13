FROM ubuntu:24.04 AS builder

RUN apt-get update && apt-get install -y --no-install-recommends \
    cmake make g++ ca-certificates \
    libboost-all-dev libssl-dev zlib1g-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src

# 复制 Hical 源码（build context = 仓库根目录）
COPY CMakeLists.txt ./
COPY cmake/ ./cmake/
COPY src/ ./src/
COPY docker/bench_main.cpp ./docker/bench_main.cpp

# 编译 bench_server（静态链接 Boost，运行阶段零依赖）
RUN cmake -B build -DCMAKE_BUILD_TYPE=Release \
        -DHICAL_BUILD_TESTS=OFF \
        -DHICAL_BUILD_EXAMPLES=OFF \
        -DHICAL_WITH_DATABASE=OFF \
        -DHICAL_WITH_OPENAPI=OFF \
        -DHICAL_BUILD_BENCH=ON \
        -DBoost_USE_STATIC_LIBS=ON \
        -DCMAKE_EXE_LINKER_FLAGS="-static-libgcc -static-libstdc++" \
    && cmake --build build -j$(nproc) --target bench_server \
    && strip build/bench_server

# --- 运行阶段 ---
FROM ubuntu:24.04
COPY --from=builder /src/build/bench_server /server
EXPOSE 8080
CMD ["/server"]
