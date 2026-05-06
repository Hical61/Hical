#!/bin/bash
# Hical vs Gin vs Actix-web 压测脚本
# 用法: docker compose exec wrk bash /bench/run_bench.sh
# 或在宿主机: bash benchmark/run_bench.sh

set -euo pipefail

DURATION=30s
THREADS=4
CONNECTIONS=100
JSON_BODY='{"name":"Alice","age":30,"email":"alice@example.com"}'
RESULT_FILE="${RESULT_FILE:-/bench/results.md}"

# 如果在容器内运行，用服务名；否则用 localhost
HICAL_HOST="${HICAL_HOST:-hical:8080}"
GIN_HOST="${GIN_HOST:-gin:8081}"
ACTIX_HOST="${ACTIX_HOST:-actix:8082}"

# 收集结果
declare -A QPS_DATA
declare -A RAW_OUTPUT

echo "============================================"
echo "  Hical vs Gin vs Actix-web 压测"
echo "  线程: $THREADS  连接: $CONNECTIONS  时长: $DURATION"
echo "============================================"
echo ""

# 从 wrk 输出中提取 Requests/sec
extract_qps() {
    grep "Requests/sec" | awk '{print $2}'
}

run_bench() {
    local name=$1
    local host=$2
    local path=$3
    local method=${4:-GET}
    local body=${5:-}
    local scene=$6

    local header="--- [$name] $method http://$host$path ---"
    echo "$header"

    local output
    if [ "$method" = "POST" ] && [ -n "$body" ]; then
        output=$(wrk -t$THREADS -c$CONNECTIONS -d$DURATION \
            -s /dev/stdin \
            "http://$host$path" <<SCRIPT
wrk.method = "POST"
wrk.body   = '$body'
wrk.headers["Content-Type"] = "application/json"
SCRIPT
        )
    else
        output=$(wrk -t$THREADS -c$CONNECTIONS -d$DURATION "http://$host$path")
    fi

    echo "$output"
    echo ""

    local qps
    qps=$(echo "$output" | extract_qps)
    QPS_DATA["${scene}|${name}"]="$qps"
    RAW_OUTPUT["${scene}|${name}"]="${header}
${output}"
}

# 等待服务启动
echo "[*] 等待服务启动..."
for host in $HICAL_HOST $GIN_HOST $ACTIX_HOST; do
    for i in $(seq 1 30); do
        if curl -s -o /dev/null "http://$host/" 2>/dev/null; then
            echo "    $host 就绪"
            break
        fi
        sleep 1
    done
done
echo ""

# ==================== 测试 1: Hello World ====================
echo "========== 测试 1: Hello World (GET /) =========="
echo ""
run_bench "Hical"     "$HICAL_HOST" "/"           "GET" "" "hello"
run_bench "Gin"       "$GIN_HOST"   "/"           "GET" "" "hello"
run_bench "Actix-web" "$ACTIX_HOST" "/"           "GET" "" "hello"

# ==================== 测试 2: JSON 响应 ====================
echo "========== 测试 2: JSON 响应 (GET /api/status) =========="
echo ""
run_bench "Hical"     "$HICAL_HOST" "/api/status" "GET" "" "json"
run_bench "Gin"       "$GIN_HOST"   "/api/status" "GET" "" "json"
run_bench "Actix-web" "$ACTIX_HOST" "/api/status" "GET" "" "json"

# ==================== 测试 3: JSON 反序列化+序列化 ====================
echo "========== 测试 3: JSON Echo (POST /api/echo) =========="
echo ""
run_bench "Hical"     "$HICAL_HOST" "/api/echo"   "POST" "$JSON_BODY" "echo"
run_bench "Gin"       "$GIN_HOST"   "/api/echo"   "POST" "$JSON_BODY" "echo"
run_bench "Actix-web" "$ACTIX_HOST" "/api/echo"   "POST" "$JSON_BODY" "echo"

# ==================== 测试 4: 路径参数 ====================
echo "========== 测试 4: 路径参数 (GET /users/42) =========="
echo ""
run_bench "Hical"     "$HICAL_HOST" "/users/42"   "GET" "" "param"
run_bench "Gin"       "$GIN_HOST"   "/users/42"   "GET" "" "param"
run_bench "Actix-web" "$ACTIX_HOST" "/users/42"   "GET" "" "param"

echo "============================================"
echo "  压测完成！"
echo "============================================"
echo ""

# ==================== 生成结果文档 ====================

# 辅助：从 wrk 输出提取字段
extract_field() {
    local raw="$1"
    local field="$2"
    echo "$raw" | grep "$field" | head -1 | sed "s/.*${field}[: ]*//" | awk '{print $1}'
}

extract_latency() {
    local raw="$1"
    echo "$raw" | grep "Latency" | head -1 | awk '{print $2, $3, $4, $5}'
}

extract_req_sec() {
    local raw="$1"
    echo "$raw" | grep "Req/Sec" | head -1 | awk '{print $2, $3, $4, $5}'
}

extract_transfer() {
    local raw="$1"
    echo "$raw" | grep "Transfer/sec" | awk '{print $2}'
}

extract_total_requests() {
    local raw="$1"
    echo "$raw" | grep "requests in" | awk '{print $1}'
}

extract_total_read() {
    local raw="$1"
    echo "$raw" | grep "requests in" | awk '{print $5}'
}

extract_socket_errors() {
    local raw="$1"
    local line
    line=$(echo "$raw" | grep "Socket errors" || true)
    if [ -n "$line" ]; then
        echo "$line" | sed 's/.*Socket errors: //'
    else
        echo "无"
    fi
}

extract_non2xx() {
    local raw="$1"
    local line
    line=$(echo "$raw" | grep "Non-2xx" || true)
    if [ -n "$line" ]; then
        echo "$line" | awk '{print $NF}'
    else
        echo "0"
    fi
}

# 写入一个场景的详细结果
write_scene() {
    local title=$1
    local scene=$2

    echo "### ${title}"
    echo ""

    for name in "Hical" "Gin" "Actix-web"; do
        local key="${scene}|${name}"
        local raw="${RAW_OUTPUT[$key]}"
        local qps="${QPS_DATA[$key]:-N/A}"
        local latency transfer total_req total_read sock_err non2xx

        latency=$(extract_latency "$raw")
        transfer=$(extract_transfer "$raw")
        total_req=$(extract_total_requests "$raw")
        total_read=$(extract_total_read "$raw")
        sock_err=$(extract_socket_errors "$raw")
        non2xx=$(extract_non2xx "$raw")

        echo "#### ${name}"
        echo ""
        echo '```'
        # 从 RAW_OUTPUT 中取 wrk 完整输出（跳过第一行 header）
        echo "$raw" | tail -n +2
        echo '```'
        echo ""
    done

    echo "**QPS 对比**"
    echo ""
    echo "| 框架 | Requests/sec |"
    echo "| --- | ---: |"
    for name in "Hical" "Gin" "Actix-web"; do
        echo "| ${name} | ${QPS_DATA[${scene}|${name}]:-N/A} |"
    done
    echo ""
}

{
    echo "# Hical vs Gin vs Actix-web 压测结果"
    echo ""
    echo "## 测试环境"
    echo ""
    echo "| 项目 | 值 |"
    echo "| --- | --- |"
    echo "| 测试时间 | $(date '+%Y-%m-%d %H:%M:%S') |"
    echo "| wrk 线程 | ${THREADS} |"
    echo "| 并发连接 | ${CONNECTIONS} |"
    echo "| 持续时间 | ${DURATION} |"
    echo "| 容器资源 | 4 CPU / 512MB per container |"
    echo "| Hical | v2.5.0 (C++20, GCC, Conan 2) |"
    echo "| Gin | v1.10 (Go 1.22) |"
    echo "| Actix-web | v4 (Rust latest stable) |"
    echo ""

    echo "---"
    echo ""
    echo "## 详细结果"
    echo ""

    write_scene "测试 1: Hello World (GET /)" "hello"
    write_scene "测试 2: JSON 响应 (GET /api/status)" "json"
    write_scene "测试 3: JSON Echo (POST /api/echo)" "echo"
    write_scene "测试 4: 路径参数 (GET /users/42)" "param"

    echo "---"
    echo ""
    echo "## QPS 汇总"
    echo ""
    echo "| 场景 | Hical | Gin | Actix-web |"
    echo "| --- | ---: | ---: | ---: |"
    echo "| Hello World (GET /) | ${QPS_DATA[hello|Hical]:-N/A} | ${QPS_DATA[hello|Gin]:-N/A} | ${QPS_DATA[hello|Actix-web]:-N/A} |"
    echo "| JSON 响应 (GET /api/status) | ${QPS_DATA[json|Hical]:-N/A} | ${QPS_DATA[json|Gin]:-N/A} | ${QPS_DATA[json|Actix-web]:-N/A} |"
    echo "| JSON Echo (POST /api/echo) | ${QPS_DATA[echo|Hical]:-N/A} | ${QPS_DATA[echo|Gin]:-N/A} | ${QPS_DATA[echo|Actix-web]:-N/A} |"
    echo "| 路径参数 (GET /users/42) | ${QPS_DATA[param|Hical]:-N/A} | ${QPS_DATA[param|Gin]:-N/A} | ${QPS_DATA[param|Actix-web]:-N/A} |"
} > "$RESULT_FILE"

echo "结果已写入: $RESULT_FILE"
