# Clang 20 + clang-tidy 静态分析 — 与 CI clang-tidy 步骤对齐
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

# 替换为清华镜像源（国内加速）
RUN sed -i 's|http://archive.ubuntu.com|http://mirrors.tuna.tsinghua.edu.cn|g' /etc/apt/sources.list.d/ubuntu.sources \
    && sed -i 's|http://security.ubuntu.com|http://mirrors.tuna.tsinghua.edu.cn|g' /etc/apt/sources.list.d/ubuntu.sources

# 添加 LLVM 20 APT 源，安装 clang-tidy-20
RUN apt-get update && apt-get install -y --no-install-recommends wget gnupg ca-certificates \
    && wget -qO- https://apt.llvm.org/llvm-snapshot.gpg.key | tee /etc/apt/trusted.gpg.d/llvm.asc \
    && echo "deb https://apt.llvm.org/noble/ llvm-toolchain-noble-20 main" > /etc/apt/sources.list.d/llvm-20.list \
    && apt-get update && apt-get install -y --no-install-recommends \
        clang-20 clang-tidy-20 \
        cmake ninja-build \
        libboost-all-dev libssl-dev libgtest-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

# 编译生成 compile_commands.json 供 clang-tidy 使用
RUN cmake -B build -G Ninja \
        -DCMAKE_BUILD_TYPE=Debug \
        -DCMAKE_C_COMPILER=clang-20 \
        -DCMAKE_CXX_COMPILER=clang++-20 \
        -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
        -DHICAL_DISABLE_IO_URING=ON \
    && cmake --build build -j$(nproc)

# clang-tidy 检查（与 CI 命令一致）
CMD ["sh", "-c", "find src -name '*.cpp' | xargs clang-tidy-20 -p build"]
