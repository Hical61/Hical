# Benchmark Dockerfile — 编译 bench_server + wrk 一体化全场景测试
FROM ubuntu:24.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive

RUN sed -i 's|http://archive.ubuntu.com|http://mirrors.tuna.tsinghua.edu.cn|g' /etc/apt/sources.list.d/ubuntu.sources \
    && sed -i 's|http://security.ubuntu.com|http://mirrors.tuna.tsinghua.edu.cn|g' /etc/apt/sources.list.d/ubuntu.sources

RUN apt-get update && apt-get install -y --no-install-recommends \
        gcc-14 g++-14 cmake ninja-build \
        libboost-all-dev libssl-dev libgtest-dev zlib1g-dev \
        wrk \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

RUN cmake -B build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_COMPILER=gcc-14 \
        -DCMAKE_CXX_COMPILER=g++-14 \
        -DHICAL_BUILD_BENCH=ON \
        -DHICAL_DISABLE_IO_URING=ON \
    && cmake --build build -j$(nproc)

# POST echo 用的 wrk lua 脚本
RUN cat > /bench_post.lua <<'LUA'
wrk.method = "POST"
wrk.headers["Content-Type"] = "application/json"
wrk.body = '{"name":"Alice","age":30,"email":"alice@example.com"}'
LUA

# 全场景压测脚本
RUN cat > /bench_run.sh <<'BASH'
#!/bin/bash
set -euo pipefail

DURATION="${DURATION:-30s}"
THREADS="${THREADS:-4}"

echo "============================================"
echo "  Hical 全场景 Benchmark"
echo "  线程: $THREADS  时长: $DURATION"
echo "============================================"
echo ""

# 启动服务器
./build/bench_server &
SERVER_PID=$!
sleep 1

# 检查服务器是否启动
if ! kill -0 $SERVER_PID 2>/dev/null; then
    echo "[错误] bench_server 启动失败"
    exit 1
fi

echo "[*] bench_server (PID=$SERVER_PID) 已启动"
echo ""

# 预热（不计入结果）
echo "========== 预热 =========="
wrk -t$THREADS -c100 -d5s http://127.0.0.1:8080/ > /dev/null 2>&1
echo "预热完成"
echo ""

# QPS 汇总数组
declare -a SUMMARY_LABELS
declare -a SUMMARY_QPS
declare -a SUMMARY_LATENCY
declare -a SUMMARY_MAXLAT
declare -a SUMMARY_TRANSFER

extract() {
    local raw="$1" field="$2"
    echo "$raw" | grep "$field" | head -1 | awk "{print \$2}"
}

run_get() {
    local title=$1
    local path=$2
    local conns=${3:-100}
    echo "========== $title (GET $path, c=$conns, $DURATION) =========="
    local output
    output=$(wrk -t$THREADS -c$conns -d$DURATION "http://127.0.0.1:8080$path" 2>&1)
    echo "$output"
    echo ""

    local qps avg_lat max_lat transfer
    qps=$(echo "$output" | grep "Requests/sec" | awk '{print $2}')
    avg_lat=$(echo "$output" | grep "Latency" | head -1 | awk '{print $2}')
    max_lat=$(echo "$output" | grep "Latency" | head -1 | awk '{print $4}')
    transfer=$(echo "$output" | grep "Transfer/sec" | awk '{print $2}')

    SUMMARY_LABELS+=("$title|$path|GET|$conns")
    SUMMARY_QPS+=("${qps:-N/A}")
    SUMMARY_LATENCY+=("${avg_lat:-N/A}")
    SUMMARY_MAXLAT+=("${max_lat:-N/A}")
    SUMMARY_TRANSFER+=("${transfer:-N/A}")
}

run_post() {
    local title=$1
    local path=$2
    local conns=${3:-100}
    echo "========== $title (POST $path, c=$conns, $DURATION) =========="
    local output
    output=$(wrk -t$THREADS -c$conns -d$DURATION -s /bench_post.lua "http://127.0.0.1:8080$path" 2>&1)
    echo "$output"
    echo ""

    local qps avg_lat max_lat transfer
    qps=$(echo "$output" | grep "Requests/sec" | awk '{print $2}')
    avg_lat=$(echo "$output" | grep "Latency" | head -1 | awk '{print $2}')
    max_lat=$(echo "$output" | grep "Latency" | head -1 | awk '{print $4}')
    transfer=$(echo "$output" | grep "Transfer/sec" | awk '{print $2}')

    SUMMARY_LABELS+=("$title|$path|POST|$conns")
    SUMMARY_QPS+=("${qps:-N/A}")
    SUMMARY_LATENCY+=("${avg_lat:-N/A}")
    SUMMARY_MAXLAT+=("${max_lat:-N/A}")
    SUMMARY_TRANSFER+=("${transfer:-N/A}")
}

# 基础场景
run_get  "Hello World"        "/"
run_get  "JSON 响应"          "/api/status"
run_post "JSON Echo"          "/api/echo"
run_get  "路径参数"           "/users/42"

# 中间件场景
run_get  "中间件 0 层"        "/middleware/0"
run_get  "中间件 3 层"        "/middleware/3"
run_get  "中间件 10 层"       "/middleware/10"
run_get  "同步中间件 3 层"    "/sync-middleware/3"
run_get  "同步中间件 10 层"   "/sync-middleware/10"

# 高并发场景
run_get  "高并发 c=100"       "/" 100
run_get  "高并发 c=1000"      "/" 1000
run_get  "高并发 c=10000"     "/" 10000

echo "============================================"
echo "  全部测试完成！"
echo "============================================"
echo ""

# 输出 Markdown 汇总表（可直接复制到 benchmark-results.md）
echo "## Results Summary (Markdown)"
echo ""
echo "| Endpoint | Method | Connections | QPS | Avg Latency | Max Latency | Transfer/sec |"
echo "| --- | --- | ---: | ---: | --- | --- | --- |"
for i in "${!SUMMARY_LABELS[@]}"; do
    IFS='|' read -r label path method conns <<< "${SUMMARY_LABELS[$i]}"
    printf "| \`%s %s\` (%s) | %s | %s | **%s** | %s | %s | %s |\n" \
        "$method" "$path" "$label" "$method" "$conns" \
        "${SUMMARY_QPS[$i]}" "${SUMMARY_LATENCY[$i]}" \
        "${SUMMARY_MAXLAT[$i]}" "${SUMMARY_TRANSFER[$i]}"
done
echo ""

kill $SERVER_PID 2>/dev/null || true
wait $SERVER_PID 2>/dev/null || true
BASH

RUN chmod +x /bench_run.sh

CMD ["bash", "/bench_run.sh"]
