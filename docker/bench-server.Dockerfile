# Benchmark Server — 仅编译并运行 bench_server（分离模式用）
FROM ubuntu:24.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive

RUN sed -i 's|http://archive.ubuntu.com|http://mirrors.tuna.tsinghua.edu.cn|g' /etc/apt/sources.list.d/ubuntu.sources \
    && sed -i 's|http://security.ubuntu.com|http://mirrors.tuna.tsinghua.edu.cn|g' /etc/apt/sources.list.d/ubuntu.sources

RUN apt-get update && apt-get install -y --no-install-recommends \
        gcc-14 g++-14 cmake ninja-build \
        libboost-all-dev libssl-dev libgtest-dev zlib1g-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

RUN cmake -B build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_COMPILER=gcc-14 \
        -DCMAKE_CXX_COMPILER=g++-14 \
        -DHICAL_BUILD_BENCH=ON \
        -DHICAL_DISABLE_IO_URING=ON \
    && cmake --build build -j$(nproc) \
    && strip build/bench_server

# --- 运行阶段（最小镜像） ---
FROM ubuntu:24.04

RUN apt-get update && apt-get install -y --no-install-recommends \
        libboost-json1.83.0 libssl3t64 zlib1g \
    && rm -rf /var/lib/apt/lists/*

COPY --from=builder /src/build/bench_server /bench_server

EXPOSE 8080

CMD ["/bench_server"]
