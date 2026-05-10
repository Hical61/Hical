FROM ubuntu:24.04 AS builder

RUN apt-get update && apt-get install -y --no-install-recommends \
    cmake g++ make wget ca-certificates libboost-dev libssl-dev \
    && rm -rf /var/lib/apt/lists/*

# 下载 Crow 单头文件 v1.2.0
RUN wget -q https://github.com/CrowCpp/Crow/releases/download/v1.2.0/crow_all.h \
    -O /usr/local/include/crow_all.h

WORKDIR /src
COPY main.cpp CMakeLists.txt ./
RUN cmake -B build -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build -j$(nproc) \
    && strip /src/build/bench_server

# --- 运行阶段 ---
FROM ubuntu:24.04

RUN apt-get update && apt-get install -y --no-install-recommends \
    libssl3 \
    && rm -rf /var/lib/apt/lists/*

COPY --from=builder /src/build/bench_server /server
EXPOSE 8084
CMD ["/server"]
