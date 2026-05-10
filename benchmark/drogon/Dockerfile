FROM ubuntu:24.04 AS builder

RUN apt-get update && apt-get install -y --no-install-recommends \
    cmake make g++ git ca-certificates \
    libjsoncpp-dev uuid-dev zlib1g-dev libssl-dev \
    && rm -rf /var/lib/apt/lists/*

# 克隆 Drogon v1.9.8
RUN git clone --depth 1 --branch v1.9.8 https://github.com/drogonframework/drogon.git /drogon \
    && cd /drogon && git submodule update --init

# 编译并安装 Drogon（静态库）
RUN cmake -S /drogon -B /drogon/build \
        -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_SHARED_LIBS=OFF \
        -DBUILD_EXAMPLES=OFF \
        -DBUILD_CTL=OFF \
        -DBUILD_DOC=OFF \
        -DBUILD_TESTING=OFF \
    && cmake --build /drogon/build -j$(nproc) \
    && cmake --install /drogon/build

# 编译 benchmark server 并 strip
WORKDIR /src
COPY main.cpp CMakeLists.txt ./
RUN cmake -B build -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build -j$(nproc) \
    && strip /src/build/bench_server

# --- 运行阶段 ---
FROM ubuntu:24.04

RUN apt-get update && apt-get install -y --no-install-recommends \
    libjsoncpp25 uuid-runtime zlib1g libssl3 \
    && rm -rf /var/lib/apt/lists/*

COPY --from=builder /src/build/bench_server /server
EXPOSE 8083
CMD ["/server"]
