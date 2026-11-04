FROM ubuntu:24.04 AS builder

RUN apt-get update && apt-get install -y --no-install-recommends \
    cmake g++ make git ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# 固定到某个提交，保证压测时大家跑的是同一份代码。
# 想测本地改动就先推到 GitHub 分支，再用 --build-arg CINATRA_REF=<分支名或 sha> 覆盖。
ARG CINATRA_REPO=https://github.com/qicosmos/cinatra.git
ARG CINATRA_REF=ec8358368c6791507658765f48cd109e9817cedf

WORKDIR /src

# 只拉目标提交那一层历史，比全量 clone 省不少流量。
# iguana 是 submodule，得单独 init 一遍，头文件才齐全。
RUN git init cinatra \
    && cd cinatra \
    && git remote add origin ${CINATRA_REPO} \
    && git fetch --depth 1 origin ${CINATRA_REF} \
    && git checkout FETCH_HEAD \
    && git submodule update --init --depth 1 iguana

COPY main.cpp CMakeLists.txt ./

# cinatra 是 header-only，这里只是把 bench_server 编出来
RUN cmake -B build -DCMAKE_BUILD_TYPE=Release -DCINATRA_ROOT=/src/cinatra \
    && cmake --build build -j$(nproc) \
    && strip /src/build/bench_server

# --- 运行阶段 ---
FROM ubuntu:24.04

# 不再需要 libssl3，cinatra 这边没开 SSL
RUN apt-get update && apt-get install -y --no-install-recommends \
    libstdc++6 \
    && rm -rf /var/lib/apt/lists/*

COPY --from=builder /src/build/bench_server /server
EXPOSE 8087
CMD ["/server"]
