# Clang 20 Debug + ASan/UBSan — 与 CI ubuntu-24.04/clang 矩阵对齐
FROM ubuntu:24.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive

# 替换为清华镜像源（国内加速）
RUN sed -i 's|http://archive.ubuntu.com|http://mirrors.tuna.tsinghua.edu.cn|g' /etc/apt/sources.list.d/ubuntu.sources \
    && sed -i 's|http://security.ubuntu.com|http://mirrors.tuna.tsinghua.edu.cn|g' /etc/apt/sources.list.d/ubuntu.sources

# 添加 LLVM 20 APT 源
RUN apt-get update && apt-get install -y --no-install-recommends wget gnupg ca-certificates \
    && wget -qO- https://apt.llvm.org/llvm-snapshot.gpg.key | tee /etc/apt/trusted.gpg.d/llvm.asc \
    && echo "deb https://apt.llvm.org/noble/ llvm-toolchain-noble-20 main" > /etc/apt/sources.list.d/llvm-20.list \
    && apt-get update && apt-get install -y --no-install-recommends \
        clang-20 libclang-rt-20-dev \
        cmake ninja-build \
        libboost-all-dev libssl-dev libgtest-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

# cmake 参数复刻 ci.yml Clang Sanitizer 配置
# 注：Ubuntu 24.04 自带 Boost 1.83，不满足 DB 中间件所需的 >= 1.85
RUN cmake -B build -G Ninja \
        -DCMAKE_BUILD_TYPE=Debug \
        -DCMAKE_C_COMPILER=clang-20 \
        -DCMAKE_CXX_COMPILER=clang++-20 \
        -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
        -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined" \
        -DHICAL_DISABLE_IO_URING=ON \
    && cmake --build build -j$(nproc)

# 测试在运行时执行，超时 120s，跳过 RouterPerfTest（与 CI 一致）
CMD ["ctest", "--test-dir", "build", "--output-on-failure", "--timeout", "120", "-j4", "-E", "RouterPerfTest"]
