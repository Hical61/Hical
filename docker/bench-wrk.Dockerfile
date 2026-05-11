# Benchmark wrk — 仅运行 wrk 压测客户端（分离模式用）
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

RUN sed -i 's|http://archive.ubuntu.com|http://mirrors.tuna.tsinghua.edu.cn|g' /etc/apt/sources.list.d/ubuntu.sources \
    && sed -i 's|http://security.ubuntu.com|http://mirrors.tuna.tsinghua.edu.cn|g' /etc/apt/sources.list.d/ubuntu.sources

RUN apt-get update && apt-get install -y --no-install-recommends \
        wrk curl \
    && rm -rf /var/lib/apt/lists/*

# POST echo 用的 wrk lua 脚本
RUN cat > /bench_post.lua <<'LUA'
wrk.method = "POST"
wrk.headers["Content-Type"] = "application/json"
wrk.body = '{"name":"Alice","age":30,"email":"alice@example.com"}'
LUA

# 全场景压测脚本（通过 SERVER_HOST 环境变量连接远端 server）
RUN cat > /bench_run.sh <<'BASH'
#!/bin/bash
set -euo pipefail

DURATION="${DURATION:-30s}"
THREADS="${THREADS:-4}"
SERVER_HOST="${SERVER_HOST:-bench-server:8080}"
REPORT_FILE="${REPORT_FILE:-/output/benchmark-results.md}"

echo "============================================"
echo "  Hical 全场景 Benchmark（分离模式）"
echo "  目标: $SERVER_HOST"
echo "  线程: $THREADS  时长: $DURATION"
echo "============================================"
echo ""

# 等待 server 就绪
echo "[*] 等待 $SERVER_HOST 就绪..."
for i in $(seq 1 30); do
    if curl -s -o /dev/null "http://$SERVER_HOST/" 2>/dev/null; then
        echo "[*] $SERVER_HOST 已就绪"
        break
    fi
    if [ "$i" -eq 30 ]; then
        echo "[错误] $SERVER_HOST 等待超时"
        exit 1
    fi
    sleep 1
done
echo ""

# 汇总数组
declare -a S_LABEL S_PATH S_METHOD S_CONNS S_QPS S_LAT S_MAXLAT S_XFER
# 原始输出（用于生成详细结果）
declare -a S_RAW

run_wrk() {
    local title=$1 path=$2 method=$3 conns=${4:-100}
    echo "========== $title ($method $path, c=$conns, $DURATION) =========="

    local output
    if [ "$method" = "POST" ]; then
        output=$(wrk -t$THREADS -c$conns -d$DURATION -s /bench_post.lua "http://$SERVER_HOST$path" 2>&1)
    else
        output=$(wrk -t$THREADS -c$conns -d$DURATION "http://$SERVER_HOST$path" 2>&1)
    fi
    echo "$output"
    echo ""

    local qps avg_lat max_lat transfer
    qps=$(echo "$output" | grep "Requests/sec" | awk '{print $2}')
    avg_lat=$(echo "$output" | grep "Latency" | head -1 | awk '{print $2}')
    max_lat=$(echo "$output" | grep "Latency" | head -1 | awk '{print $4}')
    transfer=$(echo "$output" | grep "Transfer/sec" | awk '{print $2}')

    S_LABEL+=("$title")
    S_PATH+=("$path")
    S_METHOD+=("$method")
    S_CONNS+=("$conns")
    S_QPS+=("${qps:-N/A}")
    S_LAT+=("${avg_lat:-N/A}")
    S_MAXLAT+=("${max_lat:-N/A}")
    S_XFER+=("${transfer:-N/A}")
    S_RAW+=("$output")
}

# 预热
echo "========== 预热 =========="
wrk -t$THREADS -c100 -d5s "http://$SERVER_HOST/" > /dev/null 2>&1
echo "预热完成"
echo ""

# 基础场景
run_wrk "Hello World"        "/"            GET 100
run_wrk "JSON 响应"          "/api/status"  GET 100
run_wrk "JSON Echo"          "/api/echo"    POST 100
run_wrk "路径参数"           "/users/42"    GET 100

# 中间件场景
run_wrk "中间件 0 层"        "/middleware/0"         GET 100
run_wrk "中间件 3 层"        "/middleware/3"         GET 100
run_wrk "中间件 10 层"       "/middleware/10"        GET 100
run_wrk "同步中间件 3 层"    "/sync-middleware/3"    GET 100
run_wrk "同步中间件 10 层"   "/sync-middleware/10"   GET 100

# 高并发场景
run_wrk "高并发 c=100"       "/" GET 100
run_wrk "高并发 c=1000"      "/" GET 1000
run_wrk "高并发 c=10000"     "/" GET 10000

echo "============================================"
echo "  全部测试完成！"
echo "============================================"
echo ""

# ==================== 生成 benchmark-results.md ====================

mkdir -p "$(dirname "$REPORT_FILE")"

{
    echo "# Hical Benchmark Results"
    echo ""
    echo "## Environment"
    echo ""
    echo "- **OS**: Ubuntu 24.04 (Docker container)"
    echo "- **Compiler**: GCC 14, Release mode, \`-DHICAL_DISABLE_IO_URING=ON\`"
    echo "- **CPU**: $(nproc) cores (container limit)"
    echo "- **Tool**: wrk -t${THREADS} -d${DURATION}"
    echo "- **Server**: \`bench_server\` (4 threads, SO_REUSEPORT)"
    echo "- **Mode**: 分离模式（server 和 wrk 独立容器，Docker bridge 网络）"
    echo "- **Date**: $(date '+%Y-%m-%d %H:%M:%S')"
    echo ""
    echo "---"
    echo ""

    # 汇总表
    echo "## Results Summary"
    echo ""
    echo "| Endpoint | Connections | QPS | Avg Latency | Max Latency | Transfer/sec |"
    echo "| --- | ---: | ---: | --- | --- | --- |"
    for i in "${!S_LABEL[@]}"; do
        printf "| \`%s %s\` (%s) | %s | **%s** | %s | %s | %s |\n" \
            "${S_METHOD[$i]}" "${S_PATH[$i]}" "${S_LABEL[$i]}" \
            "${S_CONNS[$i]}" "${S_QPS[$i]}" "${S_LAT[$i]}" \
            "${S_MAXLAT[$i]}" "${S_XFER[$i]}"
    done
    echo ""
    echo "---"
    echo ""

    # 详细结果
    echo "## Detailed Results"
    echo ""
    for i in "${!S_LABEL[@]}"; do
        idx=$((i + 1))
        echo "### ${idx}. ${S_LABEL[$i]} — ${S_METHOD[$i]} ${S_PATH[$i]} (c=${S_CONNS[$i]})"
        echo ""
        echo '```'
        echo "${S_RAW[$i]}"
        echo '```'
        echo ""
    done

    # 中间件对比表
    echo "---"
    echo ""
    echo "## Middleware Comparison"
    echo ""
    echo "| Type | Count | QPS | Avg Latency |"
    echo "| --- | ---: | ---: | --- |"
    # 索引 4=mw0, 5=mw3, 6=mw10, 7=smw3, 8=smw10
    printf "| 无中间件 | 0 | %s | %s |\n" "${S_QPS[4]}" "${S_LAT[4]}"
    printf "| 异步中间件 | 3 | %s | %s |\n" "${S_QPS[5]}" "${S_LAT[5]}"
    printf "| 异步中间件 | 10 | %s | %s |\n" "${S_QPS[6]}" "${S_LAT[6]}"
    printf "| **同步中间件** | 3 | **%s** | %s |\n" "${S_QPS[7]}" "${S_LAT[7]}"
    printf "| **同步中间件** | 10 | **%s** | %s |\n" "${S_QPS[8]}" "${S_LAT[8]}"
    echo ""

} > "$REPORT_FILE"

echo "[*] 报告已生成: $REPORT_FILE"
echo ""
BASH

RUN chmod +x /bench_run.sh

CMD ["bash", "/bench_run.sh"]
