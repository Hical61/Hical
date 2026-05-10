FROM ubuntu:24.04 AS builder

RUN apt-get update && apt-get install -y --no-install-recommends \
    cmake g++ make git ca-certificates libssl-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY main.cpp CMakeLists.txt ./

# FetchContent 在 configure 阶段拉取 yalantinglibs（含 cinatra + iguana），Release 编译
RUN cmake -B build -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build -j$(nproc) \
    && strip /src/build/bench_server

# --- 运行阶段 ---
FROM ubuntu:24.04

RUN apt-get update && apt-get install -y --no-install-recommends \
    libssl3 \
    && rm -rf /var/lib/apt/lists/*

COPY --from=builder /src/build/bench_server /server
EXPOSE 8087
CMD ["/server"]
