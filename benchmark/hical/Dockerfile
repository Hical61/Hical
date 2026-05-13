FROM ubuntu:24.04 AS builder

RUN apt-get update && apt-get install -y --no-install-recommends \
    cmake ninja-build gcc-14 g++-14 ca-certificates git \
    libboost-all-dev libssl-dev zlib1g-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src

# 编译 mimalloc（参考 drogon TFB 做法：替换系统 malloc，减少碎片和锁竞争）
RUN git clone --depth 1 --branch v2.1.9 https://github.com/microsoft/mimalloc.git /tmp/mimalloc \
    && cmake -B /tmp/mimalloc/build -S /tmp/mimalloc -G Ninja \
        -DCMAKE_C_COMPILER=gcc-14 -DCMAKE_CXX_COMPILER=g++-14 \
        -DCMAKE_BUILD_TYPE=Release -DMI_BUILD_TESTS=OFF \
    && cmake --build /tmp/mimalloc/build \
    && cmake --install /tmp/mimalloc/build \
    && rm -rf /tmp/mimalloc

# 复制 Hical 源码（build context = 仓库根目录）
COPY CMakeLists.txt ./
COPY cmake/ ./cmake/
COPY src/ ./src/
COPY docker/bench_main.cpp ./docker/bench_main.cpp

# 编译 bench_server（静态链接 Boost，运行阶段零依赖）
RUN cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_COMPILER=gcc-14 \
        -DCMAKE_CXX_COMPILER=g++-14 \
        -DHICAL_BUILD_TESTS=OFF \
        -DHICAL_BUILD_EXAMPLES=OFF \
        -DHICAL_WITH_DATABASE=OFF \
        -DHICAL_WITH_OPENAPI=OFF \
        -DHICAL_BUILD_BENCH=ON \
        -DHICAL_DISABLE_IO_URING=ON \
        -DBoost_USE_STATIC_LIBS=ON \
        -DCMAKE_EXE_LINKER_FLAGS="-static-libgcc -static-libstdc++" \
    && cmake --build build -j$(nproc) --target bench_server \
    && strip build/bench_server

# --- 运行阶段 ---
FROM ubuntu:24.04
COPY --from=builder /src/build/bench_server /server
COPY --from=builder /usr/local/lib/libmimalloc.so* /usr/local/lib/

ENV LD_PRELOAD=/usr/local/lib/libmimalloc.so

EXPOSE 8080
CMD ["/server"]
