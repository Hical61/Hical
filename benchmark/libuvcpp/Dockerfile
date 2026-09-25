FROM ubuntu:24.04 AS builder

RUN apt-get update && apt-get install -y --no-install-recommends \
    cmake make g++ git ca-certificates libuv1-dev \
    && rm -rf /var/lib/apt/lists/*

# 克隆 libuvcpp v1.3.0
RUN git clone --depth 1 --branch v1.3.0 https://github.com/Antruly/libuvcpp.git /libuvcpp

# 先把库编出来装到 /usr/local（旋钮照它 release.yml 里那条 Linux 发布线抄）：
#   BUILD_SHARED_LIBS=OFF 让 FetchContent 的 llhttp 编成静态库，uvcpp 才拿得到
#     llhttp::llhttp 这个别名；
#   uvcpp 自己编共享库（UVCPP_BUILD_SHARED），因为静态库的目标被 install(EXPORT)
#     导出时会把 PRIVATE 依赖 llhttp::llhttp 一起写进链接接口，消费者侧解析不到
#     这个名字会直接报错；
#   CMAKE_POSITION_INDEPENDENT_CODE=ON：BUILD_SHARED_LIBS=OFF 会把 PIC 默认关掉，
#     静态 llhttp 不带 -fPIC 就链不进 uvcpp.so（release.yml 里踩过这条）。
# 不启用 zlib / OpenSSL / nghttp2，默认就是关的，自动压缩不会介入。
RUN cmake -S /libuvcpp -B /libuvcpp/build \
        -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_SHARED_LIBS=OFF \
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
        -DUVCPP_BUILD_SHARED=ON \
        -DUVCPP_BUILD_STATIC=OFF \
        -DUVCPP_BUILD_WEB=ON \
        -DUVCPP_BUILD_WEBAPP=ON \
        -DUVCPP_BUILD_TESTS=OFF \
        -DUVCPP_BUILD_EXAMPLES=OFF \
        -DUVCPP_BUILD_BENCH=OFF \
    && cmake --build /libuvcpp/build -j$(nproc) \
    && cmake --install /libuvcpp/build

# 编译 benchmark server 并 strip
WORKDIR /src
COPY main.cpp CMakeLists.txt ./
RUN cmake -B build -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build -j$(nproc) \
    && strip /src/build/bench_server

# --- 运行阶段 ---
FROM ubuntu:24.04

# libuv1：uvcpp.so 动态链了系统 libuv（它自己的依赖都静态编进去了）
RUN apt-get update && apt-get install -y --no-install-recommends \
    libuv1 \
    && rm -rf /var/lib/apt/lists/*

COPY --from=builder /usr/local/lib/libuvcpp.so /usr/local/lib/libuvcpp.so
RUN ldconfig

COPY --from=builder /src/build/bench_server /server
EXPOSE 8088
CMD ["/server"]
