FROM ubuntu:24.04 AS builder

RUN apt-get update && apt-get install -y --no-install-recommends \
    cmake make g++ ca-certificates wrk \
    libboost-all-dev libssl-dev zlib1g-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY CMakeLists.txt ./
COPY cmake/ ./cmake/
COPY src/ ./src/
COPY docker/bench_main.cpp ./docker/bench_main.cpp

# 第一阶段：插桩编译
RUN cmake -B build-gen -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_CXX_FLAGS="-fprofile-generate=/tmp/pgo" \
        -DHICAL_BUILD_TESTS=OFF -DHICAL_BUILD_EXAMPLES=OFF \
        -DHICAL_WITH_DATABASE=OFF -DHICAL_WITH_OPENAPI=OFF \
        -DHICAL_BUILD_BENCH=ON -DBoost_USE_STATIC_LIBS=ON \
        -DCMAKE_EXE_LINKER_FLAGS="-static-libgcc -static-libstdc++" \
    && cmake --build build-gen -j$(nproc) --target bench_server

# 采集 profile（构建阶段自压测，参数不用太大，够看到热路径就行）
RUN ./build-gen/bench_server & SERVER_PID=$! \
    && sleep 1 \
    && wrk -t2 -c50 -d15s http://localhost:8080/ \
    && wrk -t2 -c50 -d15s http://localhost:8080/api/status \
    ; kill $SERVER_PID 2>/dev/null; wait $SERVER_PID 2>/dev/null; true

# 第二阶段：用 profile 数据重新编译
RUN cmake -B build-use -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_CXX_FLAGS="-fprofile-use=/tmp/pgo -fprofile-correction" \
        -DHICAL_BUILD_TESTS=OFF -DHICAL_BUILD_EXAMPLES=OFF \
        -DHICAL_WITH_DATABASE=OFF -DHICAL_WITH_OPENAPI=OFF \
        -DHICAL_BUILD_BENCH=ON -DBoost_USE_STATIC_LIBS=ON \
        -DCMAKE_EXE_LINKER_FLAGS="-static-libgcc -static-libstdc++" \
    && cmake --build build-use -j$(nproc) --target bench_server \
    && strip build-use/bench_server

# --- 运行阶段 ---
FROM ubuntu:24.04
COPY --from=builder /src/build-use/bench_server /server
EXPOSE 8080
CMD ["/server"]
