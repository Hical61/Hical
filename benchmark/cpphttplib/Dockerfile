FROM ubuntu:24.04 AS builder

RUN apt-get update && apt-get install -y --no-install-recommends \
    cmake g++ make wget ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# 下载 cpp-httplib 单头文件 v0.18.3
RUN wget -q https://github.com/yhirose/cpp-httplib/raw/v0.18.3/httplib.h \
    -O /usr/local/include/httplib.h

# 下载 nlohmann/json 单头文件 v3.11.3
RUN wget -q https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp \
    -O /usr/local/include/json.hpp

WORKDIR /src
COPY main.cpp CMakeLists.txt ./
RUN cmake -B build -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build -j$(nproc) \
    && strip /src/build/bench_server

# --- 运行阶段 ---
FROM ubuntu:24.04

# cpp-httplib 静态链接，无外部运行时依赖
COPY --from=builder /src/build/bench_server /server
EXPOSE 8086
CMD ["/server"]
