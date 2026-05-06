FROM ubuntu:24.04 AS builder

RUN apt-get update && apt-get install -y --no-install-recommends \
    cmake make g++ python3 python3-pip pipx ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# 安装 Conan 2
RUN pipx install conan && pipx ensurepath
ENV PATH="/root/.local/bin:${PATH}"

# 初始化 Conan 默认 profile
RUN conan profile detect

WORKDIR /src
COPY . .

# Conan 安装第三方依赖（Boost、OpenSSL）
RUN conan install . --build=missing -s build_type=Release -s compiler.cppstd=20

# 一次性构建 hical_core + bench_server
RUN cmake --preset conan-release \
    -DHICAL_BUILD_TESTS=OFF \
    -DHICAL_BUILD_EXAMPLES=OFF \
    -DHICAL_BUILD_BENCH=ON \
    && cmake --build --preset conan-release -j2

# --- 运行阶段 ---
FROM ubuntu:24.04
COPY --from=builder /src/build/Release/bench_server /server
EXPOSE 8080
CMD ["/server"]
