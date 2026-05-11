#!/bin/bash
# 框架压测脚本（数组驱动，支持多框架、多场景）
# 用法:
#   BENCH_MODE=cpp        docker compose --profile cpp exec wrk bash /bench/run_bench.sh
#   BENCH_MODE=cross-lang docker compose --profile cross-lang exec wrk bash /bench/run_bench.sh
#   BENCH_MODE=all        bash benchmark/run_bench.sh
#   CONNECTIONS=500 DURATION=60s BENCH_MODE=cpp bash benchmark/run_bench.sh

set -euo pipefail

# ==================== 默认参数 ====================
DURATION="${DURATION:-30s}"
THREADS="${THREADS:-4}"
CONNECTIONS="${CONNECTIONS:-100}"
JSON_BODY='{"name":"Alice","age":30,"email":"alice@example.com"}'
RESULT_FILE="${RESULT_FILE:-/bench/results.md}"

# ==================== 运行模式 ====================
# cpp:        Hical / Drogon / Crow / Oat++（默认，10 场景含中间件和高并发）
# cross-lang: Hical / Gin / Actix-web（4 基础场景，与旧脚本兼容）
# all:        全部 6 个框架，10 场景
BENCH_MODE="${BENCH_MODE:-cpp}"

# ==================== 框架 Host 定义 ====================
HICAL_HOST="${HICAL_HOST:-hical:8080}"
DROGON_HOST="${DROGON_HOST:-drogon:8083}"
CROW_HOST="${CROW_HOST:-crow:8084}"
OATPP_HOST="${OATPP_HOST:-oatpp:8085}"
CPPHTTPLIB_HOST="${CPPHTTPLIB_HOST:-cpphttplib:8086}"
CINATRA_HOST="${CINATRA_HOST:-cinatra:8087}"
GIN_HOST="${GIN_HOST:-gin:8081}"
ACTIX_HOST="${ACTIX_HOST:-actix:8082}"
FIBER_HOST="${FIBER_HOST:-fiber:8089}"

# ==================== 根据模式组装框架列表 ====================
FRAMEWORKS=()

case "$BENCH_MODE" in
    cpp)
        FRAMEWORKS=(
            "Hical|${HICAL_HOST}"
            "Drogon|${DROGON_HOST}"
            "Crow|${CROW_HOST}"
            "Oat++|${OATPP_HOST}"
            "cpp-httplib|${CPPHTTPLIB_HOST}"
            "Cinatra|${CINATRA_HOST}"
        )
        ;;
    cross-lang)
        FRAMEWORKS=(
            "Hical|${HICAL_HOST}"
            "Gin|${GIN_HOST}"
            "Fiber|${FIBER_HOST}"
            "Actix-web|${ACTIX_HOST}"
        )
        ;;
    all)
        FRAMEWORKS=(
            "Hical|${HICAL_HOST}"
            "Drogon|${DROGON_HOST}"
            "Crow|${CROW_HOST}"
            "Oat++|${OATPP_HOST}"
            "cpp-httplib|${CPPHTTPLIB_HOST}"
            "Cinatra|${CINATRA_HOST}"
            "Gin|${GIN_HOST}"
            "Fiber|${FIBER_HOST}"
            "Actix-web|${ACTIX_HOST}"
        )
        ;;
    *)
        echo "[错误] 未知 BENCH_MODE: $BENCH_MODE（可选: cpp / cross-lang / all）"
        exit 1
        ;;
esac

# ==================== 根据模式组装测试场景 ====================
# 基础 4 场景（所有模式共用）
BASE_SCENES=(
    "hello|测试 1: Hello World|/|GET|"
    "json|测试 2: JSON 响应|/api/status|GET|"
    "echo|测试 3: JSON Echo|/api/echo|POST|"
    "param|测试 4: 路径参数|/users/42|GET|"
)

# 中间件 + 高并发场景（仅 cpp / all 模式）
EXTRA_SCENES=(
    "mw0|测试 5: 中间件 0 层|/middleware/0|GET|"
    "mw3|测试 6: 中间件 3 层（原生机制）|/middleware/3|GET|"
    "mw10|测试 7: 中间件 10 层（原生机制）|/middleware/10|GET|"
    "smw3|测试 8: 中间件 3 层（Hical SyncMW）|/sync-middleware/3|GET|"
    "smw10|测试 9: 中间件 10 层（Hical SyncMW）|/sync-middleware/10|GET|"
    "c100|测试 10: 高并发 100|/|GET|100"
    "c1000|测试 11: 高并发 1000|/|GET|1000"
    "c10000|测试 12: 高并发 10000|/|GET|10000"
)

SCENES=("${BASE_SCENES[@]}")
if [ "$BENCH_MODE" != "cross-lang" ]; then
    SCENES+=("${EXTRA_SCENES[@]}")
fi

# ==================== 结果存储 ====================
declare -A QPS_DATA
declare -A RAW_OUTPUT

# ==================== 辅助函数 ====================

extract_qps() {
    grep "Requests/sec" | awk '{print $2}'
}

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

# ==================== 压测执行 ====================

# run_bench <显示名> <host:port> <路径> <方法> <连接数> <scene_id>
run_bench() {
    local name=$1
    local host=$2
    local path=$3
    local method=${4:-GET}
    local conns=${5:-$CONNECTIONS}
    local scene=$6

    local header="--- [$name] $method http://$host$path (c=${conns}) ---"
    echo "$header"

    local output
    if [ "$method" = "POST" ]; then
        output=$(wrk -t"$THREADS" -c"$conns" -d"$DURATION" \
            -s /bench/post_echo.lua \
            "http://$host$path")
    else
        output=$(wrk -t"$THREADS" -c"$conns" -d"$DURATION" "http://$host$path")
    fi

    echo "$output"
    echo ""

    local qps
    qps=$(echo "$output" | extract_qps)
    QPS_DATA["${scene}|${name}"]="$qps"
    RAW_OUTPUT["${scene}|${name}"]="${header}
${output}"
}

# ==================== 等待服务启动 ====================

echo "============================================"
echo "  框架压测（模式: $BENCH_MODE）"
echo "  线程: $THREADS  默认连接: $CONNECTIONS  时长: $DURATION"
echo "  框架数量: ${#FRAMEWORKS[@]}  场景数量: ${#SCENES[@]}"
echo "============================================"
echo ""

echo "[*] 等待服务启动..."
for fw in "${FRAMEWORKS[@]}"; do
    IFS='|' read -r fw_name fw_host <<< "$fw"
    for i in $(seq 1 30); do
        if curl -s -o /dev/null "http://$fw_host/" 2>/dev/null; then
            echo "    $fw_host ($fw_name) 就绪"
            break
        fi
        if [ "$i" -eq 30 ]; then
            echo "    [警告] $fw_host ($fw_name) 等待超时，跳过"
        fi
        sleep 1
    done
done
echo ""

# ==================== 执行所有场景 ====================

for scene_def in "${SCENES[@]}"; do
    IFS='|' read -r scene_id scene_title scene_path scene_method scene_conns <<< "$scene_def"

    # 连接数：优先使用场景覆盖值，否则使用全局默认
    local_conns="${scene_conns:-$CONNECTIONS}"

    echo "========== ${scene_title} (${scene_method} ${scene_path}) =========="
    echo ""

    for fw in "${FRAMEWORKS[@]}"; do
        IFS='|' read -r fw_name fw_host <<< "$fw"
        run_bench "$fw_name" "$fw_host" "$scene_path" "$scene_method" "$local_conns" "$scene_id"
    done
done

echo "============================================"
echo "  压测完成！"
echo "============================================"
echo ""

# ==================== 写入结果文档 ====================

# 输出单个场景的详细块（遍历框架数组）
write_scene() {
    local title=$1
    local scene=$2

    echo "### ${title}"
    echo ""

    for fw in "${FRAMEWORKS[@]}"; do
        IFS='|' read -r fw_name fw_host <<< "$fw"
        local key="${scene}|${fw_name}"
        local raw="${RAW_OUTPUT[$key]:-}"
        local qps="${QPS_DATA[$key]:-N/A}"

        echo "#### ${fw_name}"
        echo ""
        if [ -n "$raw" ]; then
            echo '```'
            echo "$raw" | tail -n +2
            echo '```'
        else
            echo "_无数据_"
        fi
        echo ""
    done

    echo "**QPS 对比**"
    echo ""
    echo "| 框架 | Requests/sec |"
    echo "| --- | ---: |"
    for fw in "${FRAMEWORKS[@]}"; do
        IFS='|' read -r fw_name fw_host <<< "$fw"
        echo "| ${fw_name} | ${QPS_DATA[${scene}|${fw_name}]:-N/A} |"
    done
    echo ""
}

# 动态生成 QPS 汇总表表头
build_summary_header() {
    local header="| 场景 |"
    local sep="| --- |"
    for fw in "${FRAMEWORKS[@]}"; do
        IFS='|' read -r fw_name fw_host <<< "$fw"
        header+=" ${fw_name} |"
        sep+=" ---: |"
    done
    echo "$header"
    echo "$sep"
}

# 动态生成 QPS 汇总表每行
build_summary_row() {
    local scene_label=$1
    local scene_id=$2
    local row="| ${scene_label} |"
    for fw in "${FRAMEWORKS[@]}"; do
        IFS='|' read -r fw_name fw_host <<< "$fw"
        row+=" ${QPS_DATA[${scene_id}|${fw_name}]:-N/A} |"
    done
    echo "$row"
}

{
    echo "# 框架压测结果"
    echo ""
    echo "## 测试环境"
    echo ""
    echo "| 项目 | 值 |"
    echo "| --- | --- |"
    echo "| 测试时间 | $(date '+%Y-%m-%d %H:%M:%S') |"
    echo "| wrk 线程 | ${THREADS} |"
    echo "| 默认并发连接 | ${CONNECTIONS} |"
    echo "| 持续时间 | ${DURATION} |"
    echo "| 参与框架 | $(IFS=','; fw_names=(); for fw in "${FRAMEWORKS[@]}"; do IFS='|' read -r n h <<< "$fw"; fw_names+=("$n"); done; echo "${fw_names[*]}") |"
    echo ""

    echo "---"
    echo ""
    echo "## 详细结果"
    echo ""

    for scene_def in "${SCENES[@]}"; do
        IFS='|' read -r scene_id scene_title scene_path scene_method scene_conns <<< "$scene_def"
        local_conns="${scene_conns:-$CONNECTIONS}"
        write_scene "${scene_title} (${scene_method} ${scene_path}, c=${local_conns})" "$scene_id"
    done

    echo "---"
    echo ""
    echo "## QPS 汇总"
    echo ""
    build_summary_header
    for scene_def in "${SCENES[@]}"; do
        IFS='|' read -r scene_id scene_title scene_path scene_method scene_conns <<< "$scene_def"
        local_conns="${scene_conns:-$CONNECTIONS}"
        build_summary_row "${scene_title} (c=${local_conns})" "$scene_id"
    done
    echo ""
} > "$RESULT_FILE"

echo "结果已写入: $RESULT_FILE"
