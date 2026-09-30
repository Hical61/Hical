#!/bin/bash
# Hical VM 本地一键压测脚本（调内核参数 → 起 bench_server → 跑五类场景 → 出报告）
# 用法:
#   bash benchmark/hical/vm_bench.sh
#   bash benchmark/hical/vm_bench.sh --quick
#   bash benchmark/hical/vm_bench.sh -d 60s -t 8 -c 200 -r 3 -o /tmp/result.md
#   bash benchmark/hical/vm_bench.sh --build --server /path/to/bench_server --no-sysctl
#

set -euo pipefail

# ==================== 路径与默认参数 ====================
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)

DURATION="60s"
THREADS=4
CONNECTIONS=100
ROUNDS=1
OUTPUT_FILE=""
SERVER_BIN=""
DO_BUILD=0
QUICK=0
NO_SYSCTL=0

SERVER_HOST="127.0.0.1"
SERVER_PORT="8080"
BASE_URL="http://${SERVER_HOST}:${SERVER_PORT}"

# 就绪探测最多等 15 秒，30 次 × 0.5s
READY_TIMEOUT_SECS=15
READY_TRIES=30
READY_INTERVAL="0.5"

# wrk 自己就要 c 个 fd，10000 并发时留一半余量才敢跑
MIN_FDS_FOR_10K=20000

JSON_BODY='{"name":"Hical","age":30,"email":"hical@example.com"}'

# ==================== 运行期状态 ====================
PROJECT_ROOT=""
# 记版本信息时用哪个仓库。A/B 对比时脚本可能在主目录、压的却是另一个 worktree
# 里的二进制，所以这个根要在 SERVER_BIN 定下来之后单独推一遍，默认跟 PROJECT_ROOT 走
VERSION_ROOT=""
SERVER_PID=""
SERVER_LOG=""
LUA_FILE=""
CRASH_INFO=""
CRASH_LOG_TAIL=""
ULIMIT_BEFORE=""
ULIMIT_AFTER=""
# 调内核参数用的 sudo 形式，setupSysctl 里定下来，restoreSysctl 原样复用。
# 取值："sudo"（认证过）/ ""（root 直调或压根没用上 sudo）
SUDO_CMD=""

declare -A ROUND_RAW ROUND_QPS ROUND_LAT ROUND_TRANSFER ROUND_NON2XX ROUND_SOCKERR ROUND_SOCKERR_FLAG
declare -A SCENE_QPS SCENE_LAT SCENE_TRANSFER SCENE_NON2XX SCENE_SOCKERR
declare -A SCENE_CONNS SCENE_DONE SCENE_LABEL SCENE_NAME
declare -A SKIP_REASON
SKIP_ORDER=()

declare -A SYSCTL_ORIG
SYSCTL_APPLIED=()

SYSCTL_KEYS=(
    "net.ipv4.ip_local_port_range=1024 65535"
    "net.core.somaxconn=65535"
    "net.ipv4.tcp_max_syn_backlog=65535"
    "net.ipv4.tcp_tw_reuse=1"
    "net.ipv4.tcp_fin_timeout=15"
)

# ==================== 用法 ====================

showUsage() {
    cat <<'EOF'
Hical VM 本地一键压测脚本

用法:
  bash benchmark/hical/vm_bench.sh [选项]

选项:
  -d, --duration <时长>       每轮压测时长，默认 60s
  -t, --threads <线程数>      wrk 线程数，默认 4
  -c, --connections <连接数>  默认并发连接数，默认 100
  -r, --rounds <轮数>         所有场景的轮数，默认 1
  -o, --output <路径>         报告输出路径，默认 benchmark/hical/benchmark-result.md
      --server <路径>         bench_server 二进制，默认 <项目根>/build/bench_server
      --build                 压测前自动跑 cmake 配置 + 编译
      --quick                 冒烟模式：时长 10s、轮数 1
      --no-sysctl             跳过内核参数调优（不碰 sudo）
  -h, --help                  打印本帮助

示例:
  bash benchmark/hical/vm_bench.sh --quick
  bash benchmark/hical/vm_bench.sh -d 60s -t 8 -c 200 -r 3
  bash benchmark/hical/vm_bench.sh --build --no-sysctl -o /tmp/hical-result.md

场景:
  基础测试    GET  /
  JSON 测试   GET  /api/status
  Echo 测试   POST /api/echo
  中间件链    GET  /middleware/0  /middleware/10
  高并发      GET  /                （c=1000 与 c=10000）

说明:
  开跑前会对每个端点探一次状态码，非 2xx 的整场跳过并在报告里写明原因。
  wrk 不校验状态码，压 404 也会打印一份看着很正常的 QPS，这种数据不能进对比。
EOF
}

# ==================== 参数解析 ====================

while [ $# -gt 0 ]; do
    case "$1" in
        -d|--duration)
            DURATION="${2:-}"
            if [ -z "$DURATION" ]; then
                echo "[错误] $1 需要一个参数"
                exit 1
            fi
            shift 2
            ;;
        -t|--threads)
            THREADS="${2:-}"
            if [ -z "$THREADS" ]; then
                echo "[错误] $1 需要一个参数"
                exit 1
            fi
            shift 2
            ;;
        -c|--connections)
            CONNECTIONS="${2:-}"
            if [ -z "$CONNECTIONS" ]; then
                echo "[错误] $1 需要一个参数"
                exit 1
            fi
            shift 2
            ;;
        -r|--rounds)
            ROUNDS="${2:-}"
            if [ -z "$ROUNDS" ]; then
                echo "[错误] $1 需要一个参数"
                exit 1
            fi
            shift 2
            ;;
        -o|--output)
            OUTPUT_FILE="${2:-}"
            if [ -z "$OUTPUT_FILE" ]; then
                echo "[错误] $1 需要一个参数"
                exit 1
            fi
            shift 2
            ;;
        --server)
            SERVER_BIN="${2:-}"
            if [ -z "$SERVER_BIN" ]; then
                echo "[错误] $1 需要一个参数"
                exit 1
            fi
            shift 2
            ;;
        --build)
            DO_BUILD=1
            shift
            ;;
        --quick)
            QUICK=1
            shift
            ;;
        --no-sysctl)
            NO_SYSCTL=1
            shift
            ;;
        -h|--help)
            showUsage
            exit 0
            ;;
        *)
            echo "[错误] 未知参数: $1"
            echo ""
            showUsage
            exit 1
            ;;
    esac
done

if [ "$QUICK" -eq 1 ]; then
    DURATION="10s"
    ROUNDS=1
fi

# ==================== 场景表 ====================
# 字段: id|分类|显示名|路径|方法|连接数|轮数
# 连接数留空表示沿用 -c 的全局值；高并发的两个场景写死，不跟着 -c 走
# 轮数留空表示沿用 -r 的全局值
SCENES=(
    "hello|基础测试|Hello World|/|GET||"
    "json|基础测试|JSON 响应|/api/status|GET||"
    "echo|基础测试|POST JSON Echo|/api/echo|POST||"
    "mw0|中间件链|无中间件基线|/middleware/0|GET||"
    "mw10|中间件链|10 层中间件|/middleware/10|GET||"
    "c1000|高并发|高并发 1000 连接|/|GET|1000|"
    "c10000|高并发|高并发 10000 连接|/|GET|10000|"
)

# ==================== 解析辅助函数 ====================

extractQps() {
    local raw="$1"
    printf '%s\n' "$raw" | grep -m1 "Requests/sec" | awk '{print $2}' || true
}

extractLatencyAvg() {
    local raw="$1"
    printf '%s\n' "$raw" | grep -m1 "Latency" | awk '{print $2}' || true
}

extractTransfer() {
    local raw="$1"
    printf '%s\n' "$raw" | grep -m1 "Transfer/sec" | awk '{print $2}' || true
}

extractNon2xx() {
    local raw="$1" line n
    line=$(printf '%s\n' "$raw" | grep -m1 "Non-2xx" || true)
    if [ -z "$line" ]; then
        echo "0"
        return 0
    fi
    n=$(printf '%s\n' "$line" | awk '{print $NF}')
    case "$n" in
        ''|*[!0-9]*) echo "0" ;;
        *) echo "$n" ;;
    esac
}

# 返回 "无" 或 "connect 0, read 1, ..." 原文
extractSocketErrors() {
    local raw="$1" line
    line=$(printf '%s\n' "$raw" | grep -m1 "Socket errors" || true)
    if [ -z "$line" ]; then
        echo "无"
        return 0
    fi
    printf '%s\n' "$line" | sed 's/.*Socket errors: *//'
}

hasSocketError() {
    local raw="$1" line detail
    line=$(printf '%s\n' "$raw" | grep -m1 "Socket errors" || true)
    if [ -z "$line" ]; then
        return 1
    fi
    detail=$(printf '%s\n' "$line" | sed 's/.*Socket errors: *//')
    # 全是 0 就等于没出错
    if printf '%s\n' "$detail" | grep -q '[1-9]'; then
        return 0
    fi
    return 1
}

# 从 stdin 收一串数字求平均，非数字行直接跳过
avgOf() {
    awk 'NF && $1 ~ /^[0-9.]+$/ { s += $1; n++ } END { if (n > 0) printf "%.2f", s / n; else printf "N/A" }'
}

# "197.84us" / "2.28ms" / "1.05s" 统一换算成微秒
toMicros() {
    printf '%s\n' "$1" | awk '{
        unit = $1
        sub(/^[0-9.]+/, "", unit)
        num = $1
        sub(/[A-Za-z\/]+$/, "", num)
        if (num !~ /^[0-9.]+$/) next
        if (unit == "us") print (num + 0)
        else if (unit == "ms") print (num + 0) * 1000
        else if (unit == "s") print (num + 0) * 1000000
        else print (num + 0)
    }'
}

formatMicros() {
    awk -v us="$1" 'BEGIN {
        if (us + 0 >= 1000000) printf "%.2fs", us / 1000000
        else if (us + 0 >= 1000) printf "%.2fms", us / 1000
        else printf "%.2fus", us
    }'
}

# "105.06MB" / "900.5KB" / "1.2GB" 换算成 MB
toMB() {
    printf '%s\n' "$1" | awk '{
        unit = $1
        sub(/^[0-9.]+/, "", unit)
        num = $1
        sub(/[A-Za-z\/]+$/, "", num)
        if (num !~ /^[0-9.]+$/) next
        if (unit == "KB") print (num + 0) / 1024
        else if (unit == "GB") print (num + 0) * 1024
        else print (num + 0)
    }'
}

# 报告里要写绝对路径，VM 上 realpath 不一定有，自己推
absPath() {
    local p="$1" dir
    dir="${p%/*}"
    if [ "$dir" = "$p" ]; then
        printf '%s/%s\n' "$(pwd)" "$p"
        return 0
    fi
    if [ -d "$dir" ]; then
        (cd "$dir" && printf '%s/%s\n' "$(pwd)" "${p##*/}")
        return 0
    fi
    printf '%s\n' "$p"
}

fdLimitEnough() {
    local cur="$1" need="$2"
    if [ "$cur" = "unlimited" ]; then
        return 0
    fi
    case "$cur" in
        ''|*[!0-9]*) return 1 ;;
    esac
    if [ "$cur" -ge "$need" ]; then
        return 0
    fi
    return 1
}

sysctlApplied() {
    local key="$1" k
    for k in "${SYSCTL_APPLIED[@]}"; do
        if [ "$k" = "$key" ]; then
            return 0
        fi
    done
    return 1
}

# ==================== 环境探测 ====================

cpuModel() {
    local model=""
    if [ -r /proc/cpuinfo ]; then
        model=$(awk -F': ' '/^model name/ {print $2; exit}' /proc/cpuinfo)
    fi
    echo "${model:-未知}"
}

memTotal() {
    local mem=""
    if [ -r /proc/meminfo ]; then
        mem=$(awk '/^MemTotal/ {printf "%.1f GB", $2 / 1024 / 1024}' /proc/meminfo)
    fi
    echo "${mem:-未知}"
}

nicInfo() {
    if ! command -v lspci >/dev/null 2>&1; then
        echo "未知（没装 lspci）"
        return 0
    fi
    local lines joined
    lines=$(lspci 2>/dev/null | grep -i ethernet || true)
    if [ -z "$lines" ]; then
        echo "未知"
        return 0
    fi
    joined=$(printf '%s\n' "$lines" | awk '{ if (out != "") out = out "<br>"; out = out $0 } END { print out }')
    if printf '%s\n' "$joined" | grep -qi virtio; then
        echo "${joined}（virtio 半虚拟化）"
    else
        echo "${joined}（非 virtio，若这是 VM 请换成 virtio-net）"
    fi
}

resolveProjectRoot() {
    local root=""
    if command -v git >/dev/null 2>&1; then
        root=$(git -C "$SCRIPT_DIR" rev-parse --show-toplevel 2>/dev/null || true)
    fi
    if [ -z "$root" ]; then
        root=$(cd "$SCRIPT_DIR/../.." && pwd)
    fi
    echo "$root"
}

# git 拿不到就当空字符串，别让 set -e 掀桌子
gitInfo() {
    # 版本信息以被压二进制所在仓库为准：A/B 对比时脚本在主目录、压的却是
    # worktree 里的二进制，按脚本位置记版本会把两份报告记成同一个版本
    local root="${VERSION_ROOT:-$PROJECT_ROOT}"
    if command -v git >/dev/null 2>&1 && [ -n "$root" ]; then
        git -C "$root" "$@" 2>/dev/null || true
    else
        echo ""
    fi
}

# ==================== 环境准备 ====================

checkDependencies() {
    local missing=()
    local tool
    for tool in curl wrk awk sed grep; do
        if ! command -v "$tool" >/dev/null 2>&1; then
            missing+=("$tool")
        fi
    done
    if [ "${#missing[@]}" -gt 0 ]; then
        echo "[错误] 缺少依赖工具: ${missing[*]}"
        echo "        wrk 没装的话: sudo apt-get install -y wrk"
        exit 1
    fi
}

buildIfRequested() {
    if [ "$DO_BUILD" -eq 0 ]; then
        return 0
    fi
    local jobs
    jobs=$(nproc 2>/dev/null || echo 4)
    echo "[*] 编译 bench_server（-DHICAL_BUILD_BENCH=ON）..."
    cmake -B "$PROJECT_ROOT/build" -DCMAKE_BUILD_TYPE=Release -DHICAL_BUILD_BENCH=ON
    cmake --build "$PROJECT_ROOT/build" -j"$jobs"
    echo "[*] 编译完成"
}

checkServerBinary() {
    if [ -x "$SERVER_BIN" ]; then
        return 0
    fi
    echo "[错误] 找不到可执行文件: $SERVER_BIN"
    echo "        先编译:"
    echo "          cmake -B build -DCMAKE_BUILD_TYPE=Release -DHICAL_BUILD_BENCH=ON && cmake --build build -j\$(nproc)"
    echo "        不想手敲就加 --build，或用 --server <path> 指定别的二进制"
    exit 1
}

checkPortFree() {
    if command -v ss >/dev/null 2>&1; then
        if ss -ltn 2>/dev/null | grep -qE "[:.]${SERVER_PORT}[[:space:]]"; then
            echo "[错误] ${SERVER_PORT} 端口已被占用（多半是上一轮残留的 bench_server）"
            echo "        先执行: pkill bench_server"
            exit 1
        fi
        return 0
    fi
    # 没装 ss 就探一下，有服务应答就当端口被占
    if curl -s -o /dev/null --max-time 2 "$BASE_URL/" 2>/dev/null; then
        echo "[错误] ${SERVER_PORT} 端口已有服务在应答（多半是上一轮残留的 bench_server）"
        echo "        先执行: pkill bench_server"
        exit 1
    fi
}

setupUlimit() {
    local desired=65535
    ULIMIT_BEFORE=$(ulimit -n)
    local hard
    hard=$(ulimit -Hn)

    local target="$desired"
    if [ "$hard" != "unlimited" ]; then
        case "$hard" in
            ''|*[!0-9]*)
                target="$ULIMIT_BEFORE"
                echo "[警告] ulimit -Hn 返回异常值「$hard」，不动文件描述符上限"
                ;;
            *)
                if [ "$hard" -lt "$desired" ]; then
                    target="$hard"
                    echo "[警告] 硬限制只有 $hard，达不到目标 $desired，只能设到硬限制"
                fi
                ;;
        esac
    fi

    if ulimit -n "$target" 2>/dev/null; then
        ULIMIT_AFTER=$(ulimit -n)
    else
        ULIMIT_AFTER="$ULIMIT_BEFORE"
        echo "[警告] ulimit -n $target 设置失败，沿用 $ULIMIT_BEFORE"
    fi
    echo "[*] 文件描述符上限: $ULIMIT_BEFORE -> $ULIMIT_AFTER"
}

setupSysctl() {
    if [ "$NO_SYSCTL" -eq 1 ]; then
        echo "[*] --no-sysctl：跳过内核参数调优"
        return 0
    fi

    if [ "$(id -u)" -ne 0 ]; then
        if ! command -v sudo >/dev/null 2>&1; then
            echo "[错误] 当前不是 root，系统里也没有 sudo，压测中止"
            echo "        装个 sudo 再跑，或者加 --no-sysctl 跳过内核参数调优"
            exit 1
        fi
        # 不探测免密也不看有没有终端了，直接要密码。密码在脚本一开头就问完，
        # 后面编译压测一路自动跑，不用中途回来输
        echo "[*] 压测前需要 sudo 权限调整内核参数，请输入密码（不想输就加 --no-sysctl 跳过）："
        if sudo -v; then
            SUDO_CMD="sudo"
        else
            echo "[错误] sudo 认证失败，压测中止"
            echo "        重跑一次再输一遍；不想输密码就加 --no-sysctl 跳过内核参数调优"
            exit 1
        fi
    fi

    echo "[*] 调整内核参数..."
    local entry key val orig
    for entry in "${SYSCTL_KEYS[@]}"; do
        IFS='=' read -r key val <<< "$entry"
        orig=$($SUDO_CMD sysctl -n "$key" 2>/dev/null || true)
        if [ -n "$orig" ]; then
            SYSCTL_ORIG["$key"]="$orig"
        fi
        if $SUDO_CMD sysctl -w "$key=$val" >/dev/null 2>&1; then
            SYSCTL_APPLIED+=("$key")
            echo "    $key = $val"
        else
            echo "    [警告] $key 设置失败，跳过"
        fi
    done
}

restoreSysctl() {
    if [ "${#SYSCTL_APPLIED[@]}" -eq 0 ]; then
        return 0
    fi
    local key orig
    for key in "${SYSCTL_APPLIED[@]}"; do
        orig="${SYSCTL_ORIG[$key]:-}"
        if [ -z "$orig" ]; then
            continue
        fi
        # 沿用 setupSysctl 定下来的 sudo 形式。交互式那种隔久了时间戳
        # 会过期，这儿再要一次密码总比恢复失败强
        if $SUDO_CMD sysctl -w "$key=$orig" >/dev/null 2>&1; then
            echo "[*] 已恢复 $key = $orig"
        else
            echo "[警告] $key 恢复失败（原值 $orig），需要手动恢复"
        fi
    done
}

ensureLuaScript() {
    if [ -n "$LUA_FILE" ] && [ -f "$LUA_FILE" ]; then
        return 0
    fi
    LUA_FILE=$(mktemp /tmp/hical-vm-bench-echo.XXXXXX.lua)
    cat > "$LUA_FILE" <<EOF
wrk.method = "POST"
wrk.body   = '${JSON_BODY}'
wrk.headers["Content-Type"] = "application/json"
EOF
}

stopServer() {
    if [ -n "$SERVER_PID" ] && kill -0 "$SERVER_PID" 2>/dev/null; then
        kill "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
        echo "[*] 已停止 bench_server (pid $SERVER_PID)"
    fi
    SERVER_PID=""
}

cleanTempFiles() {
    if [ -n "$LUA_FILE" ] && [ -f "$LUA_FILE" ]; then
        rm -f "$LUA_FILE" || true
    fi
}

# 退出时统一收尾：杀服务、删临时文件、还原 sysctl
onExit() {
    local rc=$?
    stopServer
    cleanTempFiles
    restoreSysctl
    exit "$rc"
}

# ==================== 启动与预检 ====================

startServer() {
    echo "[*] 启动 bench_server: $SERVER_BIN"
    SERVER_LOG=$(mktemp /tmp/hical-vm-bench-server.XXXXXX.log)
    "$SERVER_BIN" > "$SERVER_LOG" 2>&1 &
    SERVER_PID=$!
}

waitReadyOrDie() {
    local i
    for i in $(seq 1 "$READY_TRIES"); do
        if curl -s -o /dev/null --max-time 1 "$BASE_URL/" 2>/dev/null; then
            echo "[*] bench_server 就绪（探测 ${i} 次）"
            return 0
        fi
        if ! kill -0 "$SERVER_PID" 2>/dev/null; then
            echo "[错误] bench_server 启动后立刻退出了，日志开头:"
            sed -n '1,30p' "$SERVER_LOG" || true
            exit 1
        fi
        sleep "$READY_INTERVAL"
    done
    echo "[错误] bench_server ${READY_TIMEOUT_SECS} 秒内没就绪，日志开头:"
    sed -n '1,30p' "$SERVER_LOG" || true
    exit 1
}

probeEndpoint() {
    local path="$1" method="$2" code=""
    if [ "$method" = "POST" ]; then
        code=$(curl -o /dev/null -s -w '%{http_code}' \
            -X POST -H 'Content-Type: application/json' -d "$JSON_BODY" \
            --max-time 5 "${BASE_URL}${path}" 2>/dev/null || true)
    else
        code=$(curl -o /dev/null -s -w '%{http_code}' --max-time 5 "${BASE_URL}${path}" 2>/dev/null || true)
    fi
    echo "${code:-000}"
}

# wrk 不看状态码，404 也能压出漂亮的 QPS，所以先探一遍再决定压不压
preflight() {
    echo ""
    echo "[*] 端点预检..."
    local def id cat name path method connsIn rounds code
    for def in "${SCENES[@]}"; do
        IFS='|' read -r id cat name path method connsIn rounds <<< "$def"
        SCENE_LABEL["$id"]="${method} ${path}"
        SCENE_NAME["$id"]="$name"

        code=$(probeEndpoint "$path" "$method")
        case "$code" in
            2*)
                echo "    [OK]   ${method} ${path} -> HTTP ${code}"
                ;;
            *)
                SKIP_REASON["$id"]="HTTP ${code}，路由未在 docker/bench_main.cpp 注册"
                SKIP_ORDER+=("$id")
                echo "    [跳过] ${method} ${path} -> HTTP ${code}"
                ;;
        esac
    done

    # 10000 并发光连接就吃掉 10000 个 fd，不够就别硬上
    if ! fdLimitEnough "$ULIMIT_AFTER" "$MIN_FDS_FOR_10K"; then
        if [ -z "${SKIP_REASON[c10000]:-}" ]; then
            SKIP_REASON["c10000"]="ulimit -n 只有 ${ULIMIT_AFTER}，低于 ${MIN_FDS_FOR_10K}，跑不出真实并发"
            SKIP_ORDER+=("c10000")
            echo "    [跳过] GET / (c=10000) -> ulimit -n ${ULIMIT_AFTER} 不够"
        fi
    fi
}

# ==================== 压测执行 ====================

runWrk() {
    local path="$1" method="$2" conns="$3"
    local args=(-t"$THREADS" -c"$conns" -d"$DURATION")
    if [ "$method" = "POST" ]; then
        ensureLuaScript
        args+=(-s "$LUA_FILE")
    fi
    wrk "${args[@]}" "${BASE_URL}${path}" 2>&1
}

runAllScenes() {
    local def id cat name path method connsIn roundsIn
    for def in "${SCENES[@]}"; do
        IFS='|' read -r id cat name path method connsIn roundsIn <<< "$def"
        local conns="${connsIn:-$CONNECTIONS}"
        local roundsEach="${roundsIn:-$ROUNDS}"
        SCENE_CONNS["$id"]="$conns"
        SCENE_DONE["$id"]="0"
        SCENE_LABEL["$id"]="${method} ${path}"
        SCENE_NAME["$id"]="$name"

        if [ -n "${SKIP_REASON[$id]:-}" ]; then
            echo "[跳过] ${name} (${method} ${path}) — ${SKIP_REASON[$id]}"
            continue
        fi

        # 每轮场景前静默续一下 sudo 时间戳（默认 15 分钟就过期，-r 3 跑下来
        # 二十多分钟，不续的话脚本尾声恢复 sysctl 还得再要一次密码）。
        # -n 保证它绝不会弹提示卡住自己，过期了返回非 0 也无所谓，|| true 兜底
        if [ -n "$SUDO_CMD" ]; then
            sudo -n -v 2>/dev/null || true
        fi

        echo ""
        echo "========== ${cat} — ${name} (${method} ${path}, c=${conns}, ${roundsEach} 轮) =========="

        local qpsList=() latList=() transferList=()
        local non2xxSum=0 sockErrText="无" done=0 r
        for r in $(seq 1 "$roundsEach"); do
            echo ""
            echo "--- [${name}] 第 ${r}/${roundsEach} 轮 ---"

            local out rc=0
            out=$(runWrk "$path" "$method" "$conns") || rc=$?
            if [ "$rc" -ne 0 ]; then
                echo "[警告] wrk 退出码 ${rc}，本轮数据可能不完整"
            fi

            printf '%s\n' "$out"
            echo ""

            local key="${id}|${r}"
            ROUND_RAW["$key"]="$out"
            ROUND_QPS["$key"]=$(extractQps "$out")
            ROUND_LAT["$key"]=$(extractLatencyAvg "$out")
            ROUND_TRANSFER["$key"]=$(extractTransfer "$out")
            ROUND_NON2XX["$key"]=$(extractNon2xx "$out")
            ROUND_SOCKERR["$key"]=$(extractSocketErrors "$out")
            if hasSocketError "$out"; then
                ROUND_SOCKERR_FLAG["$key"]="1"
            else
                ROUND_SOCKERR_FLAG["$key"]="0"
            fi

            echo "    本轮 QPS: ${ROUND_QPS[$key]:-N/A}  Latency 平均: ${ROUND_LAT[$key]:-N/A}"

            qpsList+=("${ROUND_QPS[$key]}")
            latList+=("$(toMicros "${ROUND_LAT[$key]}")")
            transferList+=("$(toMB "${ROUND_TRANSFER[$key]}")")
            non2xxSum=$((non2xxSum + ${ROUND_NON2XX[$key]:-0}))
            if [ "${ROUND_SOCKERR_FLAG[$key]}" = "1" ]; then
                sockErrText="${ROUND_SOCKERR[$key]}"
            fi
            done=$((done + 1))

            if ! kill -0 "$SERVER_PID" 2>/dev/null; then
                CRASH_INFO="服务在「${name}」场景第 ${r} 轮时崩溃"
                # 服务崩了，日志尾部才是关键证据（OOM、断言、段错误前的最后一条），
                # 直接抓出来，省得事后拿着路径去 /tmp 翻
                CRASH_LOG_TAIL=$(tail -30 "$SERVER_LOG" 2>/dev/null || true)
                echo ""
                echo "[错误] ${CRASH_INFO}，中止后续场景"
                if [ -n "$CRASH_LOG_TAIL" ]; then
                    echo "--- bench_server 日志尾部（${SERVER_LOG}） ---"
                    printf '%s\n' "$CRASH_LOG_TAIL"
                    echo "--- 日志结束 ---"
                else
                    echo "    （日志是空的，服务多半是被信号直接干掉的，比如 OOM Killer）"
                fi
                break
            fi
        done

        SCENE_DONE["$id"]="$done"
        SCENE_QPS["$id"]=$(printf '%s\n' "${qpsList[@]}" | avgOf)

        local latAvg
        latAvg=$(printf '%s\n' "${latList[@]}" | avgOf)
        if [ "$latAvg" = "N/A" ]; then
            SCENE_LAT["$id"]="N/A"
        else
            SCENE_LAT["$id"]=$(formatMicros "$latAvg")
        fi

        local trAvg
        trAvg=$(printf '%s\n' "${transferList[@]}" | avgOf)
        if [ "$trAvg" = "N/A" ]; then
            SCENE_TRANSFER["$id"]="N/A"
        else
            SCENE_TRANSFER["$id"]=$(printf '%.2fMB' "$trAvg")
        fi

        SCENE_NON2XX["$id"]="$non2xxSum"
        SCENE_SOCKERR["$id"]="$sockErrText"

        if [ "$done" -gt 0 ]; then
            echo "    [${name}] 平均 QPS: ${SCENE_QPS[$id]}  平均 Latency: ${SCENE_LAT[$id]}"
        fi

        if [ -n "$CRASH_INFO" ]; then
            break
        fi
    done
}

# ==================== 报告 ====================

echoEnvTable() {
    echo "## 测试环境"
    echo ""
    echo "| 项目 | 值 |"
    echo "| --- | --- |"
    echo "| 测试时间 | $(date '+%Y-%m-%d %H:%M:%S') |"
    echo "| 主机名 | $(hostname 2>/dev/null || echo 未知) |"
    echo "| 内核 | $(uname -sr 2>/dev/null || echo 未知) |"
    echo "| CPU 型号 | $(cpuModel) |"
    echo "| vCPU 数 | $(nproc 2>/dev/null || echo 未知) |"
    echo "| 内存 | $(memTotal) |"
    echo "| 网卡 | $(nicInfo) |"
    echo "| bench_server 日志 | ${SERVER_LOG:-无} |"
    echo ""
}

echoVersionTable() {
    local branch sha tag statusOut clean describeOut
    branch=$(gitInfo rev-parse --abbrev-ref HEAD)
    sha=$(gitInfo rev-parse --short HEAD)
    tag=$(gitInfo describe --tags --abbrev=0)
    # worktree add --detach 出来的树是游离 HEAD，分支名只会是 HEAD，A/B 两份
    # 报告就分不清了。这种时候换成 describe 的结果，v2.7.0 / v2.7.0-3-g5f300b8
    # 一眼能认出压的是哪段代码
    if [ -z "$branch" ] || [ "$branch" = "HEAD" ]; then
        describeOut=$(gitInfo describe --tags --always)
        if [ -n "$describeOut" ]; then
            branch="（游离 HEAD）${describeOut}"
        else
            branch="游离 HEAD"
        fi
    fi
    # 只看已跟踪文件的改动。压测脚本和它生成的报告 md 都是新文件，
    # 算进来的话每次跑都报「脏」，这个警告很快就没人当真了
    statusOut=$(gitInfo status --porcelain --untracked-files=no)
    if [ -n "$statusOut" ]; then
        clean="⚠️ 含未提交改动，数据不可作为基线"
    else
        clean="干净"
    fi

    echo "## 被测版本"
    echo ""
    echo "| 项目 | 值 |"
    echo "| --- | --- |"
    echo "| git 分支 | ${branch:-未知} |"
    echo "| commit | ${sha:-未知} |"
    echo "| 最近 tag | ${tag:-无} |"
    echo "| 被测二进制 | ${SERVER_BIN} |"
    echo "| 工作区 | ${clean} |"
    echo ""
}

echoResourceTable() {
    echo "## 内核与资源参数"
    echo ""
    echo "| 参数 | 原值 | 本次生效值 |"
    echo "| --- | --- | --- |"
    echo "| ulimit -n | ${ULIMIT_BEFORE} | ${ULIMIT_AFTER} |"

    if [ "$NO_SYSCTL" -eq 1 ]; then
        echo "| sysctl | — | 已按 --no-sysctl 跳过 |"
    elif [ "${#SYSCTL_APPLIED[@]}" -eq 0 ]; then
        echo "| sysctl | — | 未调整（sudo 不可用或设置失败） |"
    else
        local entry key val orig
        for entry in "${SYSCTL_KEYS[@]}"; do
            IFS='=' read -r key val <<< "$entry"
            if sysctlApplied "$key"; then
                orig="${SYSCTL_ORIG[$key]:-未知}"
                # sysctl -n 读端口范围返回的是制表符分隔，塞进 markdown 表格前换成空格
                orig="${orig//$'\t'/ }"
                echo "| $key | $orig | $val |"
            fi
        done
    fi
    echo ""
}

echoBenchParams() {
    echo "## 压测参数"
    echo ""
    echo "| 项目 | 值 |"
    echo "| --- | --- |"
    echo "| wrk 线程数 | ${THREADS} |"
    echo "| 默认并发连接 | ${CONNECTIONS} |"
    echo "| 每轮时长 | ${DURATION} |"
    echo "| 每场景轮数 | ${ROUNDS} |"
    echo "| 服务地址 | ${BASE_URL} |"
    echo ""
}

echoSceneDetail() {
    local def="$1" id cat name path method connsIn roundsIn
    IFS='|' read -r id cat name path method connsIn roundsIn <<< "$def"
    local conns="${SCENE_CONNS[$id]:-$CONNECTIONS}"

    echo "### ${cat} — ${name}"
    echo ""

    if [ -n "${SKIP_REASON[$id]:-}" ]; then
        echo "**跳过**：${SKIP_REASON[$id]}"
        echo ""
        return 0
    fi

    local finished="${SCENE_DONE[$id]:-0}"
    if [ "$finished" -eq 0 ] && [ -n "$CRASH_INFO" ]; then
        echo "**未执行**：前序场景崩溃后中止。"
        echo ""
        return 0
    fi
    echo "请求 \`${method} ${path}\`，连接数 ${conns}，轮数 ${finished}/${roundsIn:-$ROUNDS}"
    echo ""

    local r key
    for r in $(seq 1 "$finished"); do
        key="${id}|${r}"
        echo "#### 第 ${r} 轮"
        echo ""
        echo '```'
        printf '%s\n' "${ROUND_RAW[$key]:-无数据}"
        echo '```'
        echo ""
        echo "- QPS: ${ROUND_QPS[$key]:-N/A}"
        echo "- Latency 平均: ${ROUND_LAT[$key]:-N/A}"
        echo "- Transfer/sec: ${ROUND_TRANSFER[$key]:-N/A}"
        echo ""
    done

    if [ "$finished" -gt 1 ]; then
        echo "**平均 QPS**: ${SCENE_QPS[$id]:-N/A} ／ **平均 Latency**: ${SCENE_LAT[$id]:-N/A}"
        echo ""
    fi
}

echoSkipTable() {
    echo "## 跳过的场景"
    echo ""
    echo "wrk 不校验状态码，压 404 也会吐出一份看着正常的 QPS，所以端点预检没过的一律不跑，"
    echo "免得假数据混进版本对比。"
    echo ""
    if [ "${#SKIP_ORDER[@]}" -eq 0 ]; then
        echo "无，所有场景端点预检通过。"
        echo ""
        return 0
    fi
    echo "| 场景 | 原因 |"
    echo "| --- | --- |"
    local id
    for id in "${SKIP_ORDER[@]}"; do
        echo "| ${SCENE_NAME[$id]:-未知}（\`${SCENE_LABEL[$id]:-未知}\`） | ${SKIP_REASON[$id]:-未知} |"
    done
    echo ""
}

echoSummaryTable() {
    echo "## QPS 汇总"
    echo ""
    echo "| 场景 | 连接数 | 轮数 | 平均 QPS | 平均 Latency | Transfer/sec | Socket errors | Non-2xx |"
    echo "| --- | ---: | ---: | ---: | ---: | ---: | --- | ---: |"

    local def id cat name path method connsIn rounds conns
    for def in "${SCENES[@]}"; do
        IFS='|' read -r id cat name path method connsIn rounds <<< "$def"
        conns="${SCENE_CONNS[$id]:-$CONNECTIONS}"

        if [ -n "${SKIP_REASON[$id]:-}" ]; then
            echo "| ${name} (${method} ${path}) | ${conns} | - | 跳过 | 跳过 | - | - | - |"
            continue
        fi

        local non2xx="${SCENE_NON2XX[$id]:-0}"
        local sock="${SCENE_SOCKERR[$id]:-无}"
        local nonMark="" sockMark=""
        if [ "$non2xx" != "0" ]; then
            nonMark=" ⚠️"
        fi
        if [ "$sock" != "无" ]; then
            sockMark=" ⚠️"
        fi
        echo "| ${name} (${method} ${path}) | ${conns} | ${SCENE_DONE[$id]:-0} | ${SCENE_QPS[$id]:-N/A} | ${SCENE_LAT[$id]:-N/A} | ${SCENE_TRANSFER[$id]:-N/A} | ${sock}${sockMark} | ${non2xx}${nonMark} |"
    done
    echo ""
}

echoMiddlewareDecay() {
    echo "## 中间件链开销"
    echo ""

    if [ -n "${SKIP_REASON[mw0]:-}" ]; then
        echo "基线 \`/middleware/0\` 被跳过，算不出衰减。"
        echo ""
        return 0
    fi

    local base="${SCENE_QPS[mw0]:-}"
    if [ -z "$base" ] || [ "$base" = "N/A" ]; then
        echo "基线 \`/middleware/0\` 没有有效数据，算不出衰减。"
        echo ""
        return 0
    fi

    local id q pct
    echo "| 场景 | 平均 QPS | 相对 /middleware/0 |"
    echo "| --- | ---: | ---: |"
    echo "| /middleware/0 | ${base} | 基准 |"
    for id in mw10; do
        q="${SCENE_QPS[$id]:-}"
        if [ -n "${SKIP_REASON[$id]:-}" ] || [ -z "$q" ] || [ "$q" = "N/A" ]; then
            echo "| /middleware/${id#mw} | 跳过 | - |"
            continue
        fi
        pct=$(awk -v a="$base" -v b="$q" 'BEGIN { if (a + 0 > 0) printf "%+.1f%%", (b - a) / a * 100; else printf "N/A" }')
        echo "| /middleware/${id#mw} | ${q} | ${pct} |"
    done
    echo ""
}

writeReport() {
    local backup
    if [ -f "$OUTPUT_FILE" ]; then
        backup="${OUTPUT_FILE%.md}-$(date '+%Y%m%d-%H%M%S').md"
        mv "$OUTPUT_FILE" "$backup"
        echo "[*] 旧报告已备份: $backup"
    fi

    mkdir -p "$(dirname "$OUTPUT_FILE")"

    {
        echo "# Hical VM 本地压测结果"
        echo ""
        echo "> 由 \`benchmark/hical/vm_bench.sh\` 自动生成"
        echo ""

        if [ -n "$CRASH_INFO" ]; then
            echo "> ⚠️ **压测被中断**：${CRASH_INFO}，后续场景未执行。"
            echo ""

            if [ -n "$CRASH_LOG_TAIL" ]; then
                echo "bench_server 崩溃前的日志尾部（完整日志：\`${SERVER_LOG}\`）："
                echo ""
                echo '```'
                printf '%s\n' "$CRASH_LOG_TAIL"
                echo '```'
                echo ""
            else
                echo "> bench_server 日志为空（服务可能是被信号直接杀掉的，比如 OOM Killer）。完整日志：\`${SERVER_LOG}\`"
                echo ""
            fi
        fi

        echoEnvTable
        echoVersionTable
        echoResourceTable
        echoBenchParams

        echo "---"
        echo ""
        echo "## 逐场景结果"
        echo ""

        local def
        for def in "${SCENES[@]}"; do
            echoSceneDetail "$def"
        done

        echo "---"
        echo ""
        echoSummaryTable
        echoMiddlewareDecay
        echoSkipTable
    } > "$OUTPUT_FILE"
}

# ==================== 主流程 ====================

trap onExit EXIT
trap 'exit 130' INT TERM

checkDependencies

PROJECT_ROOT=$(resolveProjectRoot)

if [ -z "$SERVER_BIN" ]; then
    SERVER_BIN="$PROJECT_ROOT/build/bench_server"
fi

# 从二进制路径往上找 git 仓库根；找不到（比如二进制在 /tmp）就退回脚本所在仓库
VERSION_ROOT="$PROJECT_ROOT"
derivedRoot=$(git -C "$(dirname "$SERVER_BIN")" rev-parse --show-toplevel 2>/dev/null || true)
if [ -n "$derivedRoot" ]; then
    VERSION_ROOT="$derivedRoot"
fi

if [ -z "$OUTPUT_FILE" ]; then
    OUTPUT_FILE="$SCRIPT_DIR/benchmark-result.md"
fi

echo "============================================"
echo "  Hical VM 本地压测"
echo "  服务:     $SERVER_BIN"
echo "  线程:     $THREADS   持续时间: $DURATION   轮数: $ROUNDS"
echo "  场景数:   ${#SCENES[@]}"
echo "============================================"
echo ""
echo "[*] 项目根: $PROJECT_ROOT"

# 内核参数调优需要密码，提到最前面来，一跑脚本就输完，后面全自动
setupSysctl

buildIfRequested
checkServerBinary
checkPortFree
setupUlimit

startServer
waitReadyOrDie
preflight
runAllScenes

echo ""
echo "[*] 生成报告..."
writeReport

echo ""
echo "============================================"
echo "  压测完成"
echo "  报告路径: $OUTPUT_FILE"
echo "  绝对路径: $(absPath "$OUTPUT_FILE")"
echo "============================================"
