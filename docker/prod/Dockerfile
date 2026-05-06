# ============ 构建阶段 ============
FROM ubuntu:24.04 AS builder

RUN apt-get update && apt-get install -y --no-install-recommends \
    cmake make g++ python3 python3-pip pipx ca-certificates curl \
    && rm -rf /var/lib/apt/lists/*

# Conan 2 包管理器
RUN pipx install conan && pipx ensurepath
ENV PATH="/root/.local/bin:${PATH}"
RUN conan profile detect

WORKDIR /src

# 分离依赖层（conanfile.py + CMakeLists.txt 不常变，利用 Docker 缓存）
COPY conanfile.py CMakeLists.txt ./
COPY src/ src/
COPY cmake/ cmake/

RUN conan install . --build=missing \
    -s build_type=Release \
    -s compiler.cppstd=20 \
    -o '*:with_database=True' 2>/dev/null || \
    conan install . --build=missing \
    -s build_type=Release \
    -s compiler.cppstd=20

# 复制剩余源码（业务代码变化只重编译，不重装依赖）
COPY docker/prod/ docker/prod/

# 编译 prod_server（启用数据库 + LTO）
RUN cmake --preset conan-release \
    -DHICAL_BUILD_TESTS=OFF \
    -DHICAL_BUILD_EXAMPLES=OFF \
    -DHICAL_BUILD_DEPLOY=ON \
    -DHICAL_WITH_DATABASE=ON \
    -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON \
    && cmake --build --preset conan-release --target prod_server -j$(nproc)

# ============ 运行阶段 ============
FROM ubuntu:24.04

RUN apt-get update && apt-get install -y --no-install-recommends \
    libssl3 ca-certificates curl \
    && rm -rf /var/lib/apt/lists/* \
    && groupadd -r hical && useradd -r -g hical -s /usr/sbin/nologin hical \
    && mkdir -p /app/public /var/log/hical \
    && chown -R hical:hical /app /var/log/hical

# 从构建阶段复制二进制
COPY --from=builder /src/build/Release/prod_server /app/server

USER hical
WORKDIR /app

EXPOSE 8080

HEALTHCHECK --interval=10s --timeout=3s --start-period=15s --retries=3 \
    CMD curl -sf http://localhost:8080/health || exit 1

CMD ["/app/server"]
