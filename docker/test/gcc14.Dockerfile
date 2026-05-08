# GCC 14 Release — 与 CI ubuntu-24.04/gcc 矩阵对齐
FROM ubuntu:24.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive

# 替换为清华镜像源（国内加速）
RUN sed -i 's|http://archive.ubuntu.com|http://mirrors.tuna.tsinghua.edu.cn|g' /etc/apt/sources.list.d/ubuntu.sources \
    && sed -i 's|http://security.ubuntu.com|http://mirrors.tuna.tsinghua.edu.cn|g' /etc/apt/sources.list.d/ubuntu.sources

# 添加 LLVM 20 APT 源（clang-format 需要 19+ 支持 BreakAfterReturnType）
RUN apt-get update && apt-get install -y --no-install-recommends wget gnupg ca-certificates \
    && wget -qO- https://apt.llvm.org/llvm-snapshot.gpg.key | tee /etc/apt/trusted.gpg.d/llvm.asc \
    && echo "deb https://apt.llvm.org/noble/ llvm-toolchain-noble-20 main" > /etc/apt/sources.list.d/llvm-20.list \
    && apt-get update && apt-get install -y --no-install-recommends \
        gcc-14 g++-14 cmake ninja-build clang-format-20 \
        libboost-all-dev libssl-dev libgtest-dev \
    && ln -sf /usr/bin/clang-format-20 /usr/bin/clang-format \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

# cmake 参数复刻 ci.yml GCC 配置（去掉 ccache，容器内无缓存意义）
# 注：Ubuntu 24.04 自带 Boost 1.83，不满足 DB 中间件所需的 >= 1.85，
# 因此 HICAL_WITH_DATABASE 保持关闭。DB 测试由 GitHub Actions CI 覆盖。
RUN cmake -B build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_COMPILER=gcc-14 \
        -DCMAKE_CXX_COMPILER=g++-14 \
        -DHICAL_DISABLE_IO_URING=ON \
    && cmake --build build -j$(nproc)

# 运行时：先检查格式，再跑测试
CMD ["sh", "-c", "find src tests examples -name '*.h' -o -name '*.cpp' | xargs clang-format --dry-run --Werror && ctest --test-dir build --output-on-failure --timeout 60 -j4"]
