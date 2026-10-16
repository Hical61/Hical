#!/bin/bash
# Hical VM 本地一键压测脚本（调内核参数 → 起 bench_server → 跑场景）
# 用法:
#   bash benchmark/hical/vm_bench.sh                      单版本冒烟：只打终端，不产出报告
#   bash benchmark/hical/vm_bench.sh --quick
#   bash benchmark/hical/vm_bench.sh -d 60s -t 8 -c 200 -r 3 --pin
#   bash benchmark/hical/vm_bench.sh --build --server /path/to/bench_server --no-sysctl
#   配对对比（同一个端口上 A/B 轮流起停，背靠背交替测，唯一出报告的跑法）:
#   bash benchmark/hical/vm_bench.sh --server build/bench_server --server-b /tmp/old/bench_server --pin -r 3
#

set -euo pipefail

# ==================== 路径与默认参数 ====================
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)

DURATION="60s"
THREADS=4
CONNECTIONS=100
ROUNDS=1
OUTPUT_FILE=""
# -o 是不是用户显式给的。冒烟模式下给了就提醒它会被忽略
OUTPUT_GIVEN=0
SERVER_BIN=""
SERVER_BIN_B=""
DO_BUILD=0
QUICK=0
NO_SYSCTL=0
PIN=0
ONLY=""

# bench_server 的 worker 数，必须跟 docker/bench_main.cpp 里 HttpServer(port, 4) 的 4 对上。
# EventLoopPool 会把第 i 个 worker 无条件绑到 CPU (i % nproc)，4 个 worker 就占了 CPU 0-3，
# wrk 绑核时要避开这一段，别跟 server 抢
SERVER_WORKERS=4

SERVER_HOST="127.0.0.1"
SERVER_PORT="8080"
BASE_URL="http://${SERVER_HOST}:${SERVER_PORT}"

# wrk 绑核的目标 CPU 列表（setupPinning 里算出来），空字符串表示不绑
PIN_CPUS=""
# --pin 被跳过时记的原因，报告里要用
PIN_SKIP_REASON=""

# 起服务失败时的重试次数与每次间隔，防反复起停偶发端口没释放
START_RETRIES=3
START_RETRY_INTERVAL="1"

# 就绪探测最多等 15 秒，30 次 × 0.5s
READY_TIMEOUT_SECS=15
READY_TRIES=30
READY_INTERVAL="0.5"

# wrk 自己就要 c 个 fd，10000 并发时留一半余量才敢跑
MIN_FDS_FOR_10K=20000

# 报告末尾「A/B 总结」判"需要追查"的门槛（百分点）。方向一致时中位差绝对值、
# 或者差值极差超过它就算异常。轻载档配对差值实测多落在 2 个点以内，5 个点已经明显出圈
PAIR_SIG_PCT=5

JSON_BODY='{"name":"Hical","age":30,"email":"hical@example.com"}'

# ==================== 运行期状态 ====================
PROJECT_ROOT=""
PAIRED=0
# 记版本信息时用哪个仓库。A/B 对比时脚本可能在主目录、压的却是另一个 worktree
# 里的二进制，所以这个根要在 SERVER_BIN 定下来之后单独推一遍，默认跟 PROJECT_ROOT 走
VERSION_ROOT=""
VERSION_ROOT_B=""
SERVER_PID=""
SERVER_LOG=""
SERVER_LOG_A=""
SERVER_LOG_B=""
LUA_FILE=""
CRASH_INFO=""
CRASH_LOG_TAIL=""
PAIR_CRASH_INFO=""
PAIR_CRASH_LOG_TAIL=""
ULIMIT_BEFORE=""
ULIMIT_AFTER=""
# 调内核参数用的 sudo 形式，setupSysctl 里定下来，restoreSysctl 原样复用。
# 取值："sudo"（认证过）/ ""（root 直调或压根没用上 sudo）
SUDO_CMD=""

declare -A ROUND_RAW ROUND_QPS ROUND_LAT ROUND_TRANSFER ROUND_NON2XX ROUND_SOCKERR ROUND_SOCKERR_FLAG
declare -A SCENE_QPS SCENE_LAT SCENE_TRANSFER SCENE_NON2XX SCENE_SOCKERR
declare -A SCENE_CONNS SCENE_DONE SCENE_LABEL SCENE_NAME

# 配对模式（给了 --server-b）另起一套数据，跟单侧那套 ROUND_*/SCENE_* 完全隔开，
# 免得手一抖改了单侧报告的取数。key 形如 "<场景id>|<对号>|a"
declare -A PAIR_RAW PAIR_QPS PAIR_LAT PAIR_TRANSFER PAIR_NON2XX PAIR_SOCKERR PAIR_SOCKERR_FLAG
declare -A PAIR_NAME PAIR_LABEL PAIR_METHOD PAIR_PATH PAIR_CONNS PAIR_PAIRS PAIR_SKIP
PAIR_SKIP_ORDER=()

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

两种模式:
  带 --server-b    配对对比：同一端口 A/B 背靠背轮流起停，产出对比报告
  不带 --server-b  单版本冒烟：只跑场景往终端打逐轮 QPS，不产出任何文件

选项:
  -d, --duration <时长>       每轮压测时长，默认 60s
  -t, --threads <线程数>      wrk 线程数，默认 4
  -c, --connections <连接数>  默认并发连接数，默认 100
  -r, --rounds <轮数>         所有场景的轮数，默认 1
                              配对模式下含义变成"配对数"
  -o, --output <路径>         报告输出路径，仅配对模式生效
                              默认 benchmark/hical/result-paired.md
      --server <路径>         bench_server 二进制（A 侧），默认 <项目根>/build/bench_server
      --server-b <路径>       B 侧 bench_server 二进制。给了它就进 A/B 配对对比模式
      --only <场景id,...>     只跑指定场景，逗号分隔。未知 id 直接报错退出
      --pin                   把 wrk 绑到 CPU (nproc-线程数)..(nproc-1)，避开 server 自绑的 0-3
      --build                 压测前自动跑 cmake 配置 + 编译
      --quick                 快速：时长 10s、轮数 1
      --no-sysctl             跳过内核参数调优（不碰 sudo）
  -h, --help                  打印本帮助

示例:
  bash benchmark/hical/vm_bench.sh --quick
  bash benchmark/hical/vm_bench.sh -d 60s -t 8 -c 200 -r 3 --pin
  bash benchmark/hical/vm_bench.sh --build --no-sysctl
  bash benchmark/hical/vm_bench.sh --only hello,mw10 --pin
  # A/B 配对对比：同端口轮流起停，背靠背交替测，出 paired 报告
  bash benchmark/hical/vm_bench.sh --server build/bench_server --server-b /tmp/v2.6.0/bench_server --pin -r 3

场景:
  基础测试    GET  /
  JSON 测试   GET  /api/status
  Echo 测试   POST /api/echo
  中间件链    GET  /middleware/0  /middleware/10
  高并发      GET  /                （c=1000 与 c=10000）

场景 id: hello / json / echo / mw0 / mw10 / c1000 / c10000

说明:
  开跑前会对每个端点探一次状态码，非 2xx 的整场跳过并在终端写明原因。
  wrk 不校验状态码，压 404 也会打印一份看着很正常的 QPS，这种数据不能进对比。
  单版本冒烟模式只把逐轮 QPS 打到终端，不写任何文件（-o 会被忽略），适合快速自测。
  配对模式会 A、B 两侧各探一次，任一侧没过就整体跳过该场景。
  配对模式的数据按"配对差值 (B-A)/A"的中位数判断，不靠跨 session 的平均值比。
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
            OUTPUT_GIVEN=1
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
        --server-b)
            SERVER_BIN_B="${2:-}"
            if [ -z "$SERVER_BIN_B" ]; then
                echo "[错误] $1 需要一个参数"
                exit 1
            fi
            shift 2
            ;;
        --only)
            ONLY="${2:-}"
            if [ -z "$ONLY" ]; then
                echo "[错误] $1 需要一个参数"
                exit 1
            fi
            shift 2
            ;;
        --pin)
            PIN=1
            shift
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

# 合法场景 id 列表，--only 校验和报错提示都用它
VALID_SCENE_IDS=""
for _def in "${SCENES[@]}"; do
    IFS='|' read -r _sid _ <<< "$_def"
    VALID_SCENE_IDS="${VALID_SCENE_IDS:+$VALID_SCENE_IDS,}$_sid"
done

# 场景是否被 --only 选中；没给 --only 就全选
sceneSelected() {
    local id="$1"
    if [ -z "$ONLY" ]; then
        return 0
    fi
    case ",$ONLY," in
        *",$id,"*) return 0 ;;
        *) return 1 ;;
    esac
}

# --only 里塞了不存在的 id 直接退出，别跑到一半才发现压了个空气
if [ -n "$ONLY" ]; then
    ONLY=$(printf '%s' "$ONLY" | tr -d '[:space:]')
    IFS=',' read -r -a _onlyIds <<< "$ONLY"
    for _oid in "${_onlyIds[@]}"; do
        [ -z "$_oid" ] && continue
        _found=0
        case ",$VALID_SCENE_IDS," in
            *",$_oid,"*) _found=1 ;;
        esac
        if [ "$_found" -eq 0 ]; then
            echo "[错误] --only 里有未知场景 id: ${_oid}"
            echo "        合法 id: ${VALID_SCENE_IDS}"
            exit 1
        fi
    done
fi

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

# 从 stdin 收一串数字求中位数，非数字行跳过；偶数个取中间两个的平均。
# 配对数一般就 3 个，插入排序够用，不折腾
medianOf() {
    awk '
        $1 ~ /^-?[0-9.]+$/ { a[++n] = $1 + 0 }
        END {
            if (n == 0) { print ""; exit }
            for (i = 2; i <= n; i++) {
                key = a[i]; j = i - 1
                while (j >= 1 && a[j] > key) { a[j + 1] = a[j]; j-- }
                a[j + 1] = key
            }
            if (n % 2 == 1) printf "%.6f", a[(n + 1) / 2]
            else printf "%.6f", (a[n / 2] + a[n / 2 + 1]) / 2
        }
    '
}

# 单对差值 (B-A)/A*100，调用前保证 a 是正数
pairDiffRaw() {
    awk -v a="$1" -v b="$2" 'BEGIN { printf "%.6f", (b - a) / a * 100 }'
}

isNum() {
    case "$1" in
        ''|*[!0-9.]*) return 1 ;;
        *) return 0 ;;
    esac
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
gitInfoIn() {
    local root="$1"
    shift
    if command -v git >/dev/null 2>&1 && [ -n "$root" ]; then
        git -C "$root" "$@" 2>/dev/null || true
    else
        echo ""
    fi
}

# 给某个仓库根算分支显示名。worktree add --detach 出来的树是游离 HEAD，
# 分支名只会是 HEAD，A/B 两份报告就分不清了，这时换成 describe 的结果。
# v2.7.0 / v2.7.0-3-g5f300b8 一眼能认出压的是哪段代码
branchDescFor() {
    local root="$1" branch describeOut
    branch=$(gitInfoIn "$root" rev-parse --abbrev-ref HEAD)
    if [ -z "$branch" ] || [ "$branch" = "HEAD" ]; then
        describeOut=$(gitInfoIn "$root" describe --tags --always)
        if [ -n "$describeOut" ]; then
            printf '（游离 HEAD）%s\n' "$describeOut"
        else
            printf '游离 HEAD\n'
        fi
        return 0
    fi
    printf '%s\n' "$branch"
}

# 只看已跟踪文件的改动。压测脚本和它生成的报告 md 都是新文件，算进来的话
# 每次跑都报「脏」，这个警告很快就没人当真了
cleanFlagFor() {
    local root="$1" statusOut
    statusOut=$(gitInfoIn "$root" status --porcelain --untracked-files=no)
    if [ -n "$statusOut" ]; then
        printf '⚠️ 含未提交改动，数据不可作为基线\n'
    else
        printf '干净\n'
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
    local bin="${1:-$SERVER_BIN}"
    if [ -x "$bin" ]; then
        return 0
    fi
    echo "[错误] 找不到可执行文件: $bin"
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

# 把 wrk 绑到 CPU (nproc-THREADS)..(nproc-1)，正好避开 bench_server 自绑的 0..3。
# 不绑的话 wrk 的线程在全部核上乱飘，一半时间跟 server 挤在 0-3 上，A/B 容易被噪声带偏。
# 注意只绑 wrk：对 server 进程下 taskset 没用，它自己会 pthread_setaffinity_np 把外层 mask 盖掉
setupPinning() {
    if [ "$PIN" -eq 0 ]; then
        return 0
    fi
    PIN_SKIP_REASON=""

    if ! command -v taskset >/dev/null 2>&1; then
        PIN_SKIP_REASON="taskset 命令不存在"
        echo "[警告] --pin 已跳过：${PIN_SKIP_REASON}，wrk 将在全部核上自由调度"
        return 0
    fi
    if ! printf '%s' "$THREADS" | grep -qE '^[0-9]+$'; then
        PIN_SKIP_REASON="wrk 线程数「${THREADS}」不是数字"
        echo "[警告] --pin 已跳过：${PIN_SKIP_REASON}"
        return 0
    fi

    local n
    n=$(nproc 2>/dev/null || echo "")
    if ! printf '%s' "$n" | grep -qE '^[0-9]+$'; then
        PIN_SKIP_REASON="nproc 取不到 CPU 数"
        echo "[警告] --pin 已跳过：${PIN_SKIP_REASON}，wrk 将在全部核上自由调度"
        return 0
    fi

    local first
    first=$((n - THREADS))
    if [ "$first" -lt "$SERVER_WORKERS" ]; then
        PIN_SKIP_REASON="CPU 数 ${n} 减去 wrk 线程数 ${THREADS} 只剩 ${first}，会和 bench_server 自绑的 CPU 0-$((SERVER_WORKERS - 1)) 重叠"
        echo "[警告] --pin 已跳过：${PIN_SKIP_REASON}，绑了反而跟 server 抢核"
        return 0
    fi

    PIN_CPUS="${first}-$((n - 1))"
    echo "[*] wrk 绑核: CPU ${PIN_CPUS}（bench_server 自绑 CPU 0-$((SERVER_WORKERS - 1))）"
}

# 报告里"绑核"一栏的文字
pinDescription() {
    if [ -n "$PIN_CPUS" ]; then
        printf '已绑核（wrk -> CPU %s，bench_server 自绑 CPU 0-%s）\n' "$PIN_CPUS" "$((SERVER_WORKERS - 1))"
    elif [ "$PIN" -eq 1 ]; then
        printf '未绑核（--pin 已跳过：%s）\n' "${PIN_SKIP_REASON:-原因未知}"
    else
        printf '未启用（未加 --pin）\n'
    fi
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

# 起服务。参数: <二进制路径> [侧标记 a|b]。侧标记只在配对模式用来分拣日志。
# 反复起停时旧进程的 socket 可能还没释放干净（SO_REUSEADDR 也救不了某些尾巴），
# 进程会秒退，所以给个窗口确认它还活着，退了就换份日志重试
startServer() {
    local bin="${1:-$SERVER_BIN}"
    local side="${2:-}"
    local attempt
    for attempt in $(seq 1 "$START_RETRIES"); do
        SERVER_LOG=$(mktemp /tmp/hical-vm-bench-server.XXXXXX.log)
        case "$side" in
            a) SERVER_LOG_A="$SERVER_LOG" ;;
            b) SERVER_LOG_B="$SERVER_LOG" ;;
        esac
        echo "[*] 启动 bench_server: $bin"
        "$bin" > "$SERVER_LOG" 2>&1 &
        SERVER_PID=$!

        sleep "$START_RETRY_INTERVAL"
        if kill -0 "$SERVER_PID" 2>/dev/null; then
            return 0
        fi
        echo "[警告] 第 ${attempt}/${START_RETRIES} 次启动失败（进程已退出），日志开头:"
        sed -n '1,5p' "$SERVER_LOG" 2>/dev/null || true
        SERVER_PID=""
    done
    echo "[错误] bench_server 连续 ${START_RETRIES} 次启动失败，日志开头:"
    sed -n '1,30p' "$SERVER_LOG" 2>/dev/null || true
    exit 1
}

# 参数: [URL]，默认 $BASE_URL。就绪返回 0，失败打印原因后返回 1
waitReadyOrDie() {
    local url="${1:-$BASE_URL}"
    local i
    for i in $(seq 1 "$READY_TRIES"); do
        if curl -s -o /dev/null --max-time 1 "${url}/" 2>/dev/null; then
            echo "[*] bench_server 就绪（探测 ${i} 次）"
            return 0
        fi
        if ! kill -0 "$SERVER_PID" 2>/dev/null; then
            echo "[错误] bench_server 启动后立刻退出了，日志开头:"
            sed -n '1,30p' "$SERVER_LOG" || true
            return 1
        fi
        sleep "$READY_INTERVAL"
    done
    echo "[错误] bench_server ${READY_TIMEOUT_SECS} 秒内没就绪，日志开头:"
    sed -n '1,30p' "$SERVER_LOG" || true
    return 1
}

probeEndpoint() {
    local path="$1" method="$2" url="${3:-$BASE_URL}" code=""
    if [ "$method" = "POST" ]; then
        code=$(curl -o /dev/null -s -w '%{http_code}' \
            -X POST -H 'Content-Type: application/json' -d "$JSON_BODY" \
            --max-time 5 "${url}${path}" 2>/dev/null || true)
    else
        code=$(curl -o /dev/null -s -w '%{http_code}' --max-time 5 "${url}${path}" 2>/dev/null || true)
    fi
    echo "${code:-000}"
}

# 单侧端点预检，结果写进 PAIR_SKIP。配对模式的 A/B 两侧和单版本冒烟都走它，
# 靠传入的标签文字区分是哪一侧。wrk 不看状态码，404 也能压出漂亮的 QPS，
# 所以先探一遍再决定压不压
preflightSide() {
    local url="$1" sideLabel="$2" def id cat name path method connsIn rounds code reason
    echo ""
    echo "[*] 端点预检（${sideLabel}）..."
    for def in "${SCENES[@]}"; do
        IFS='|' read -r id cat name path method connsIn rounds <<< "$def"
        if ! sceneSelected "$id"; then
            continue
        fi
        code=$(probeEndpoint "$path" "$method" "$url")
        case "$code" in
            2*)
                echo "    [OK]   ${method} ${path} -> HTTP ${code}"
                ;;
            *)
                reason="HTTP ${code}（${sideLabel}）"
                if [ -z "${PAIR_SKIP[$id]:-}" ]; then
                    PAIR_SKIP["$id"]="$reason"
                    PAIR_SKIP_ORDER+=("$id")
                else
                    PAIR_SKIP["$id"]="${PAIR_SKIP[$id]}；${reason}"
                fi
                echo "    [跳过] ${method} ${path} -> HTTP ${code}"
                ;;
        esac
    done
}

# 配对模式两侧各起一次探状态码，探完立刻停。B 侧可能是没注册路由的旧构建，
# wrk 又不看状态码，不先探一遍会静默压出 404 的漂亮 QPS 混进对比
preflightPaired() {
    startServer "$SERVER_BIN" "a"
    waitReadyOrDie "$BASE_URL"
    preflightSide "$BASE_URL" "A 侧"
    stopServer

    startServer "$SERVER_BIN_B" "b"
    waitReadyOrDie "$BASE_URL"
    preflightSide "$BASE_URL" "B 侧"
    stopServer

    if sceneSelected c10000 && ! fdLimitEnough "$ULIMIT_AFTER" "$MIN_FDS_FOR_10K"; then
        if [ -z "${PAIR_SKIP[c10000]:-}" ]; then
            PAIR_SKIP["c10000"]="ulimit -n 只有 ${ULIMIT_AFTER}，低于 ${MIN_FDS_FOR_10K}，跑不出真实并发"
            PAIR_SKIP_ORDER+=("c10000")
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
    if [ -n "$PIN_CPUS" ]; then
        taskset -c "$PIN_CPUS" wrk "${args[@]}" "${BASE_URL}${path}" 2>&1
    else
        wrk "${args[@]}" "${BASE_URL}${path}" 2>&1
    fi
}

# 单版本冒烟的执行循环：逐场景逐轮跑 wrk，往终端打本轮 QPS 和每场景平均。
# 不写任何文件，配对模式的报告是 writePairReport 那另一套
runSmokeScenes() {
    local def id cat name path method connsIn roundsIn
    for def in "${SCENES[@]}"; do
        IFS='|' read -r id cat name path method connsIn roundsIn <<< "$def"
        if ! sceneSelected "$id"; then
            continue
        fi
        local conns="${connsIn:-$CONNECTIONS}"
        local roundsEach="${roundsIn:-$ROUNDS}"
        SCENE_CONNS["$id"]="$conns"
        SCENE_DONE["$id"]="0"
        SCENE_LABEL["$id"]="${method} ${path}"
        SCENE_NAME["$id"]="$name"

        if [ -n "${PAIR_SKIP[$id]:-}" ]; then
            echo "[跳过] ${name} (${method} ${path}) — ${PAIR_SKIP[$id]}"
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

# ==================== 配对模式执行 ====================

# 配对模式单个 (场景, 对号, 侧) 的完整一轮：
# 起该侧服务 → 等就绪 → wrk → 记数据 → 停服务。一个端口永远只活着一个 server
pairRunOneSide() {
    local id="$1" p="$2" side="$3" path="$4" method="$5" conns="$6" name="$7"
    local bin url tag
    if [ "$side" = "a" ]; then
        bin="$SERVER_BIN"
        tag="A"
    else
        bin="$SERVER_BIN_B"
        tag="B"
    fi
    url="$BASE_URL"

    startServer "$bin" "$side"
    waitReadyOrDie "$url"

    local out rc=0
    out=$(runWrk "$path" "$method" "$conns") || rc=$?
    if [ "$rc" -ne 0 ]; then
        echo "[警告] wrk 退出码 ${rc}，本轮数据可能不完整"
    fi
    printf '%s\n' "$out"
    echo ""

    local key="${id}|${p}|${side}"
    PAIR_RAW["$key"]="$out"
    PAIR_QPS["$key"]=$(extractQps "$out")
    PAIR_LAT["$key"]=$(extractLatencyAvg "$out")
    PAIR_TRANSFER["$key"]=$(extractTransfer "$out")
    PAIR_NON2XX["$key"]=$(extractNon2xx "$out")
    PAIR_SOCKERR["$key"]=$(extractSocketErrors "$out")
    if hasSocketError "$out"; then
        PAIR_SOCKERR_FLAG["$key"]="1"
    else
        PAIR_SOCKERR_FLAG["$key"]="0"
    fi

    echo "    [${tag}] QPS: ${PAIR_QPS[$key]:-N/A}  Latency 平均: ${PAIR_LAT[$key]:-N/A}"

    if ! kill -0 "$SERVER_PID" 2>/dev/null; then
        PAIR_CRASH_INFO="服务在「${name}」场景第 ${p} 对 ${tag} 侧时崩溃"
        PAIR_CRASH_LOG_TAIL=$(tail -30 "$SERVER_LOG" 2>/dev/null || true)
        echo ""
        echo "[错误] ${PAIR_CRASH_INFO}，中止后续场景"
        if [ -n "$PAIR_CRASH_LOG_TAIL" ]; then
            echo "--- bench_server 日志尾部（${SERVER_LOG}） ---"
            printf '%s\n' "$PAIR_CRASH_LOG_TAIL"
            echo "--- 日志结束 ---"
        fi
    fi

    stopServer
}

# A/B 配对跑法：同一个端口上"起 A → 压 → 停 A → 起 B → 压 → 停 B"，两个 server 从不同时活着。
# 顺序 ABBA——奇数对 A 先、偶数对 B 先，把配对内的顺序残留也抵消掉
runAllPairs() {
    local def id cat name path method connsIn roundsIn
    for def in "${SCENES[@]}"; do
        IFS='|' read -r id cat name path method connsIn roundsIn <<< "$def"
        if ! sceneSelected "$id"; then
            continue
        fi
        local conns="${connsIn:-$CONNECTIONS}"
        PAIR_CONNS["$id"]="$conns"
        PAIR_NAME["$id"]="$name"
        PAIR_LABEL["$id"]="${method} ${path}"
        PAIR_METHOD["$id"]="$method"
        PAIR_PATH["$id"]="$path"
        PAIR_PAIRS["$id"]="0"

        if [ -n "${PAIR_SKIP[$id]:-}" ]; then
            echo "[跳过] ${name} (${method} ${path}) — ${PAIR_SKIP[$id]}"
            continue
        fi

        # 每场景前静默续一下 sudo 时间戳，跟单侧那边一个道理
        if [ -n "$SUDO_CMD" ]; then
            sudo -n -v 2>/dev/null || true
        fi

        echo ""
        echo "========== ${cat} — ${name} (${method} ${path}, c=${conns}, ${ROUNDS} 对) =========="

        local donePairs=0 p side order firstTag
        for p in $(seq 1 "$ROUNDS"); do
            if [ $((p % 2)) -eq 1 ]; then
                order=("a" "b")
                firstTag="A 先"
            else
                order=("b" "a")
                firstTag="B 先"
            fi
            echo ""
            echo "--- [${name}] 第 ${p}/${ROUNDS} 对（${firstTag}）---"

            for side in "${order[@]}"; do
                pairRunOneSide "$id" "$p" "$side" "$path" "$method" "$conns" "$name"
                if [ -n "$PAIR_CRASH_INFO" ]; then
                    break
                fi
            done
            donePairs=$((donePairs + 1))
            if [ -n "$PAIR_CRASH_INFO" ]; then
                break
            fi
        done
        PAIR_PAIRS["$id"]="$donePairs"

        if [ -n "$PAIR_CRASH_INFO" ]; then
            break
        fi
    done
}

# ==================== 报告 ====================

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

# ==================== 配对模式报告 ====================

echoPairEnvTable() {
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
    echo "| 绑核 | $(pinDescription) |"
    echo "| bench_server 日志 | A: ${SERVER_LOG_A:-无}<br>B: ${SERVER_LOG_B:-无} |"
    echo ""
}

echoPairVersionTable() {
    local branchA branchB shaA shaB tagA tagB
    branchA=$(branchDescFor "$VERSION_ROOT")
    branchB=$(branchDescFor "$VERSION_ROOT_B")
    shaA=$(gitInfoIn "$VERSION_ROOT" rev-parse --short HEAD)
    shaB=$(gitInfoIn "$VERSION_ROOT_B" rev-parse --short HEAD)
    tagA=$(gitInfoIn "$VERSION_ROOT" describe --tags --abbrev=0)
    tagB=$(gitInfoIn "$VERSION_ROOT_B" describe --tags --abbrev=0)

    echo "## 被测版本"
    echo ""
    echo "| 项目 | A | B |"
    echo "| --- | --- | --- |"
    echo "| git 分支 | ${branchA:-未知} | ${branchB:-未知} |"
    echo "| commit | ${shaA:-未知} | ${shaB:-未知} |"
    echo "| 最近 tag | ${tagA:-无} | ${tagB:-无} |"
    echo "| 被测二进制 | ${SERVER_BIN} | ${SERVER_BIN_B} |"
    echo "| 工作区 | $(cleanFlagFor "$VERSION_ROOT") | $(cleanFlagFor "$VERSION_ROOT_B") |"
    echo ""
}

echoPairBenchParams() {
    echo "## 压测参数"
    echo ""
    echo "| 项目 | 值 |"
    echo "| --- | --- |"
    echo "| wrk 线程数 | ${THREADS} |"
    echo "| 默认并发连接 | ${CONNECTIONS} |"
    echo "| 每轮时长 | ${DURATION} |"
    echo "| 配对数 | ${ROUNDS} |"
    echo "| 服务地址 | ${BASE_URL} |"
    echo "| 绑核情况 | $(pinDescription) |"
    echo "| 执行顺序 | ABBA（奇数对 A 先，偶数对 B 先） |"
    echo "| 场景筛选 | ${ONLY:-全部} |"
    echo ""
}

echoPairSummaryTable() {
    echo "| 场景 | A 中位 QPS | B 中位 QPS | 配对差值中位 | 差值范围 | 同号 | 判断 |"
    echo "| --- | ---: | ---: | ---: | ---: | ---: | --- |"
    local def id
    for def in "${SCENES[@]}"; do
        IFS='|' read -r id _ <<< "$def"
        if ! sceneSelected "$id"; then
            continue
        fi
        echoPairSummaryRow "$id"
    done
    echo ""
}

# 单场景的配对统计，「配对对比汇总」和末尾「A/B 总结」都调它，两处的数字永远一致。
# 输出一行 tab 分隔：n <TAB> medA <TAB> medB <TAB> medD <TAB> minD <TAB> maxD <TAB> pos
# 一对都算不出来（QPS 缺失或 A 侧不是正数）时输出空行，由调用方按"不可比"处理
pairStatsOf() {
    local id="$1"
    local donePairs="${PAIR_PAIRS[$id]:-0}"
    local aVals=() bVals=() diffs=()
    local p aq bq
    for p in $(seq 1 "$donePairs"); do
        aq="${PAIR_QPS["$id|$p|a"]:-}"
        bq="${PAIR_QPS["$id|$p|b"]:-}"
        if isNum "$aq" && isNum "$bq" && awk -v x="$aq" 'BEGIN { exit !(x + 0 > 0) }'; then
            aVals+=("$aq")
            bVals+=("$bq")
            diffs+=("$(pairDiffRaw "$aq" "$bq")")
        fi
    done

    local n="${#diffs[@]}"
    if [ "$n" -eq 0 ]; then
        echo ""
        return 0
    fi

    local medA medB medD minD maxD pos
    medA=$(printf '%s\n' "${aVals[@]}" | medianOf)
    medB=$(printf '%s\n' "${bVals[@]}" | medianOf)
    medD=$(printf '%s\n' "${diffs[@]}" | medianOf)
    read -r minD maxD <<< "$(printf '%s\n' "${diffs[@]}" | awk 'NR == 1 { mn = mx = $1 } { if ($1 < mn) mn = $1; if ($1 > mx) mx = $1 } END { printf "%.1f %.1f", mn, mx }')"
    pos=$(printf '%s\n' "${diffs[@]}" | awk '$1 + 0 > 0 { c++ } END { print c + 0 }')

    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$n" "$medA" "$medB" "$medD" "$minD" "$maxD" "$pos"
}

# 单场景一行核心对比。统计量全交给 pairStatsOf，这里只管排版。
# 判断只看方向一致性，不设 5%/10% 这种阈值——让读的人自己看"差值范围"的离散度
echoPairSummaryRow() {
    local id="$1"
    local name="${PAIR_NAME[$id]:-未知}" method="${PAIR_METHOD[$id]:-}" path="${PAIR_PATH[$id]:-}"

    if [ -n "${PAIR_SKIP[$id]:-}" ]; then
        echo "| ${name} (${method} ${path}) | 跳过 | 跳过 | - | - | - | 不可比 |"
        return 0
    fi

    local stats n medA medB medD minD maxD pos verdict
    stats=$(pairStatsOf "$id")
    if [ -z "$stats" ]; then
        echo "| ${name} (${method} ${path}) | - | - | - | - | - | 不可比 |"
        return 0
    fi
    IFS=$'\t' read -r n medA medB medD minD maxD pos <<< "$stats"

    if [ "$pos" -eq "$n" ]; then
        verdict="B 快（${n}/${n} 一致）"
    elif [ "$pos" -eq 0 ]; then
        verdict="A 快（${n}/${n} 一致）"
    else
        verdict="方向不一致"
    fi

    printf '| %s (%s %s) | %.2f | %.2f | %+.1f%% | %+.1f%% ~ %+.1f%% | %d/%d | %s |\n' \
        "$name" "$method" "$path" "$medA" "$medB" "$medD" "$minD" "$maxD" "$pos" "$n" "$verdict"
}

echoPairSummaryNotes() {
    echo "> **为什么看配对差值而不是两侧平均值**：两侧各自跑一整个 session 时，宿主环境"
    echo "> 会漂移（同二进制跨 session 实测能差 8~15%，高并发档 32%），这种漂移会直接盖过要"
    echo "> 测的差异。配对模式下 A、B 背靠背交替，漂移在配对内被约掉，剩下的才是真差异。"
    echo ">"
    echo "> **同号列**数的是差值为正的个数；\"一致\"只说明方向一致。配对数 N 小（比如 3 对）时"
    echo "> 只能看方向是否一边倒，看不出置信度——要更强的结论得加配对数。"
    echo ""
}

echoPairSceneDetail() {
    local def="$1" id cat name path method connsIn roundsIn
    IFS='|' read -r id cat name path method connsIn roundsIn <<< "$def"
    if ! sceneSelected "$id"; then
        return 0
    fi
    local conns="${connsIn:-$CONNECTIONS}"

    echo "### ${cat} — ${name}"
    echo ""
    if [ -n "${PAIR_SKIP[$id]:-}" ]; then
        echo "**跳过**：${PAIR_SKIP[$id]}"
        echo ""
        return 0
    fi

    local donePairs="${PAIR_PAIRS[$id]:-0}"
    if [ "$donePairs" -eq 0 ]; then
        echo "**未执行**：前序场景崩溃后中止。"
        echo ""
        return 0
    fi
    echo "请求 \`${method} ${path}\`，连接数 ${conns}，共 ${donePairs} 对"
    echo ""

    local p firstTag aq bq difftxt
    for p in $(seq 1 "$donePairs"); do
        if [ $((p % 2)) -eq 1 ]; then
            firstTag="A 先"
        else
            firstTag="B 先"
        fi
        echo "#### 第 ${p} 对（${firstTag}）"
        echo ""
        echo "**A 侧 wrk 原始输出**"
        echo ""
        echo '```'
        printf '%s\n' "${PAIR_RAW["$id|$p|a"]:-无数据}"
        echo '```'
        echo ""
        echo "**B 侧 wrk 原始输出**"
        echo ""
        echo '```'
        printf '%s\n' "${PAIR_RAW["$id|$p|b"]:-无数据}"
        echo '```'
        echo ""
        aq="${PAIR_QPS["$id|$p|a"]:-}"
        bq="${PAIR_QPS["$id|$p|b"]:-}"
        if isNum "$aq" && isNum "$bq" && awk -v x="$aq" 'BEGIN { exit !(x + 0 > 0) }'; then
            difftxt=$(printf '%+.1f%%' "$(pairDiffRaw "$aq" "$bq")")
        else
            difftxt="不可比"
        fi
        echo "- A QPS: ${aq:-N/A}"
        echo "- B QPS: ${bq:-N/A}"
        echo "- 配对差值 (B-A)/A: ${difftxt}"
        echo ""
    done
}

# 某场景某侧的中位 QPS，没数据返回空
sceneMedianQps() {
    local id="$1" side="$2"
    local donePairs="${PAIR_PAIRS[$id]:-0}"
    local list=() p q
    for p in $(seq 1 "$donePairs"); do
        q="${PAIR_QPS["$id|$p|$side"]:-}"
        if isNum "$q"; then
            list+=("$q")
        fi
    done
    if [ "${#list[@]}" -eq 0 ]; then
        echo ""
        return 0
    fi
    printf '%s\n' "${list[@]}" | medianOf
}

echoPairSideSummary() {
    local side="$1" tag
    if [ "$side" = "a" ]; then
        tag="A"
    else
        tag="B"
    fi
    echo "### ${tag} 侧"
    echo ""
    echo "| 场景 | 连接数 | 对数 | 中位 QPS | 中位 Latency | 中位 Transfer/sec | Socket errors | Non-2xx |"
    echo "| --- | ---: | ---: | ---: | ---: | ---: | --- | ---: |"
    local def id
    for def in "${SCENES[@]}"; do
        IFS='|' read -r id _ <<< "$def"
        if ! sceneSelected "$id"; then
            continue
        fi
        echoPairSideRow "$id" "$side"
    done
    echo ""
}

echoPairSideRow() {
    local id="$1" side="$2"
    local name="${PAIR_NAME[$id]:-未知}" method="${PAIR_METHOD[$id]:-}" path="${PAIR_PATH[$id]:-}"
    local conns="${PAIR_CONNS[$id]:-$CONNECTIONS}"

    if [ -n "${PAIR_SKIP[$id]:-}" ]; then
        echo "| ${name} (${method} ${path}) | ${conns} | - | 跳过 | 跳过 | 跳过 | - | - |"
        return 0
    fi

    local donePairs="${PAIR_PAIRS[$id]:-0}"
    if [ "$donePairs" -eq 0 ]; then
        echo "| ${name} (${method} ${path}) | ${conns} | 0 | - | - | - | - | - |"
        return 0
    fi

    local qpsList=() latList=() trList=() p non2xxSum=0 sockText="无"
    for p in $(seq 1 "$donePairs"); do
        local k="${id}|${p}|${side}" q l t
        q="${PAIR_QPS[$k]:-}"
        if isNum "$q"; then
            qpsList+=("$q")
        fi
        l="${PAIR_LAT[$k]:-}"
        if [ -n "$l" ]; then
            latList+=("$(toMicros "$l")")
        fi
        t="${PAIR_TRANSFER[$k]:-}"
        if [ -n "$t" ]; then
            trList+=("$(toMB "$t")")
        fi
        non2xxSum=$((non2xxSum + ${PAIR_NON2XX[$k]:-0}))
        if [ "${PAIR_SOCKERR_FLAG[$k]:-0}" = "1" ]; then
            sockText="${PAIR_SOCKERR[$k]}"
        fi
    done

    local medQ="N/A" medL="N/A" medT="N/A" mL mT
    if [ "${#qpsList[@]}" -gt 0 ]; then
        medQ=$(printf '%s\n' "${qpsList[@]}" | medianOf | awk '{ printf "%.2f", $1 }')
    fi
    if [ "${#latList[@]}" -gt 0 ]; then
        mL=$(printf '%s\n' "${latList[@]}" | medianOf)
        medL=$(formatMicros "$mL")
    fi
    if [ "${#trList[@]}" -gt 0 ]; then
        mT=$(printf '%s\n' "${trList[@]}" | medianOf)
        medT=$(printf '%.2fMB' "$mT")
    fi

    local nonMark="" sockMark=""
    if [ "$non2xxSum" != "0" ]; then
        nonMark=" ⚠️"
    fi
    if [ "$sockText" != "无" ]; then
        sockMark=" ⚠️"
    fi
    echo "| ${name} (${method} ${path}) | ${conns} | ${donePairs} | ${medQ} | ${medL} | ${medT} | ${sockText}${sockMark} | ${non2xxSum}${nonMark} |"
}

echoPairMiddlewareDecay() {
    echo "## 中间件链开销"
    echo ""

    local baseA baseB tenA tenB
    baseA=$(sceneMedianQps mw0 a)
    baseB=$(sceneMedianQps mw0 b)
    tenA=$(sceneMedianQps mw10 a)
    tenB=$(sceneMedianQps mw10 b)

    if [ -z "$baseA" ] && [ -z "$baseB" ]; then
        echo "基线 \`/middleware/0\` 没有有效数据，算不出衰减。"
        echo ""
        return 0
    fi

    echo "| 场景 | A 中位 QPS | A 相对 /middleware/0 | B 中位 QPS | B 相对 /middleware/0 |"
    echo "| --- | ---: | ---: | ---: | ---: |"

    local pctA="-" pctB="-"
    if [ -n "$tenA" ] && [ -n "$baseA" ]; then
        pctA=$(awk -v a="$baseA" -v b="$tenA" 'BEGIN { if (a + 0 > 0) printf "%+.1f%%", (b - a) / a * 100; else printf "N/A" }')
    fi
    if [ -n "$tenB" ] && [ -n "$baseB" ]; then
        pctB=$(awk -v a="$baseB" -v b="$tenB" 'BEGIN { if (a + 0 > 0) printf "%+.1f%%", (b - a) / a * 100; else printf "N/A" }')
    fi

    echo "| /middleware/0 | ${baseA:-跳过} | 基准 | ${baseB:-跳过} | 基准 |"
    echo "| /middleware/10 | ${tenA:-跳过} | ${pctA} | ${tenB:-跳过} | ${pctB} |"
    echo ""
}

echoPairSkipTable() {
    echo "## 跳过的场景"
    echo ""
    echo "配对模式两侧各探一次状态码，任一侧没过就整体跳过。wrk 不校验状态码，"
    echo "不先探一遍会把 404 的漂亮 QPS 当有效数据混进对比。"
    echo ""
    if [ "${#PAIR_SKIP_ORDER[@]}" -eq 0 ]; then
        echo "无，所有场景两侧端点预检均通过。"
        echo ""
        return 0
    fi
    echo "| 场景 | 原因 |"
    echo "| --- | --- |"
    local id
    for id in "${PAIR_SKIP_ORDER[@]}"; do
        echo "| ${PAIR_NAME[$id]:-未知}（\`${PAIR_LABEL[$id]:-未知}\`） | ${PAIR_SKIP[$id]:-未知} |"
    done
    echo ""
}

# 报告末尾的 A/B 总结：把场景分成"要深究"和"可以放过"两拨，省得读的人自己去
# 一行行比对差值范围。统计量跟「配对对比汇总」同源（都调 pairStatsOf），不会打架
echoPairVerdict() {
    local chaseRows=() okRows=()
    local def id cat name path method _rest
    for def in "${SCENES[@]}"; do
        IFS='|' read -r id cat name path method _rest <<< "$def"
        if ! sceneSelected "$id"; then
            continue
        fi
        local sceneName="${PAIR_NAME[$id]:-$name}"
        local sceneMethod="${PAIR_METHOD[$id]:-$method}"
        local scenePath="${PAIR_PATH[$id]:-$path}"
        local scene="${sceneName} (${sceneMethod} ${scenePath})"

        if [ -n "${PAIR_SKIP[$id]:-}" ]; then
            chaseRows+=("| ${scene} | - | - | - | 两侧端点预检未通过，本次没测到 |")
            continue
        fi

        local stats n medA medB medD minD maxD pos
        stats=$(pairStatsOf "$id")
        if [ -z "$stats" ]; then
            chaseRows+=("| ${scene} | - | - | - | 一对有效数据都没拿到，本次没测到 |")
            continue
        fi
        IFS=$'\t' read -r n medA medB medD minD maxD pos <<< "$stats"

        local spread range medTxt medAbs consistent=0
        spread=$(awk -v mn="$minD" -v mx="$maxD" 'BEGIN { printf "%.1f", mx - mn }')
        range=$(printf '%+.1f%% ~ %+.1f%%' "$minD" "$maxD")
        medTxt=$(printf '%+.1f%%' "$medD")
        # 中位差的绝对值，去掉符号和百分号，判阈值用，跟表里显示的数字保持一致
        medAbs="${medTxt#+}"
        medAbs="${medAbs#-}"
        medAbs="${medAbs%\%}"
        if [ "$pos" -eq 0 ] || [ "$pos" -eq "$n" ]; then
            consistent=1
        fi

        local reason=""
        if [ "$consistent" -eq 0 ]; then
            reason="方向不一致"
        fi
        if awk -v s="$spread" -v lim="$PAIR_SIG_PCT" 'BEGIN { exit !(s + 0 >= lim + 0) }'; then
            reason="${reason:+${reason}，}极差 ${spread} 个点"
        fi
        if [ "$consistent" -eq 1 ] && \
            awk -v d="$medAbs" -v lim="$PAIR_SIG_PCT" 'BEGIN { exit !(d + 0 >= lim + 0) }'; then
            reason="${reason:+${reason}，}方向一致但中位差 ${medTxt}，是稳定的真实差异"
        fi

        if [ -n "$reason" ]; then
            chaseRows+=("| ${scene} | ${medTxt} | ${range} | ${pos}/${n} | ${reason} |")
        else
            local dir="A 快"
            if [ "$pos" -eq "$n" ]; then
                dir="B 快"
            fi
            okRows+=("| ${scene} | ${medTxt} | ${range} | ${pos}/${n} | ${dir}，幅度和离散度都在噪声内 |")
        fi
    done

    local total=$(( ${#chaseRows[@]} + ${#okRows[@]} ))

    echo "## A/B 总结"
    echo ""
    if [ "$total" -gt 0 ]; then
        echo "${total} 个场景里 **${#chaseRows[@]} 个需要追查**，**${#okRows[@]} 个在范围内**。"
        echo ""
    fi
    if [ "${#chaseRows[@]}" -gt 0 ]; then
        echo "### 需要追查"
        echo ""
        echo "| 场景 | 中位差 | 差值范围 | 同号 | 为什么要追 |"
        echo "| --- | ---: | ---: | ---: | --- |"
        printf '%s\n' "${chaseRows[@]}"
        echo ""
    fi
    if [ "${#okRows[@]}" -gt 0 ]; then
        echo "### 符合范围"
        echo ""
        echo "| 场景 | 中位差 | 差值范围 | 同号 | 说明 |"
        echo "| --- | ---: | ---: | ---: | --- |"
        printf '%s\n' "${okRows[@]}"
        echo ""
    fi
    echo "> 判定口径："
    echo "> - **方向不一致**（同号数既不是 0 也不是 N）→ 至少有一对被外部干扰打偏，这次的数不可用，重跑"
    echo "> - **极差 ≥ ${PAIR_SIG_PCT} 个百分点** → 同上，测量不稳"
    echo "> - **方向一致但 |中位差| ≥ ${PAIR_SIG_PCT}%** → 这是稳定的真实差异，得去翻是哪个提交引进来的"
    echo "> - 三条都不沾 → 符合范围，两版本在该场景上没有可测量的差异"
    echo ">"
    echo "> 轻载档配对差值实测多落在 2 个点以内，所以 ${PAIR_SIG_PCT} 个点这条线不是拍脑袋，是踩着实测噪声定的。"
    echo ""
}

writePairReport() {
    local backup
    if [ -f "$OUTPUT_FILE" ]; then
        backup="${OUTPUT_FILE%.md}-$(date '+%Y%m%d-%H%M%S').md"
        mv "$OUTPUT_FILE" "$backup"
        echo "[*] 旧报告已备份: $backup"
    fi

    mkdir -p "$(dirname "$OUTPUT_FILE")"

    {
        echo "# Hical VM 本地压测结果（A/B 配对对比）"
        echo ""
        echo "> 由 \`benchmark/hical/vm_bench.sh\` 自动生成"
        echo ""

        if [ -n "$PAIR_CRASH_INFO" ]; then
            echo "> ⚠️ **压测被中断**：${PAIR_CRASH_INFO}，后续场景未执行。"
            echo ""

            if [ -n "$PAIR_CRASH_LOG_TAIL" ]; then
                echo "bench_server 崩溃前的日志尾部："
                echo ""
                echo '```'
                printf '%s\n' "$PAIR_CRASH_LOG_TAIL"
                echo '```'
                echo ""
            fi
        fi

        echoPairEnvTable
        echoPairVersionTable
        echoResourceTable
        echoPairBenchParams

        echo "---"
        echo ""
        echo "## 配对对比汇总"
        echo ""
        echoPairSummaryTable
        echoPairSummaryNotes

        echo "---"
        echo ""
        echo "## 逐场景配对详情"
        echo ""

        local def
        for def in "${SCENES[@]}"; do
            echoPairSceneDetail "$def"
        done

        echo "---"
        echo ""
        echo "## 分侧汇总"
        echo ""
        echoPairSideSummary "a"
        echoPairSideSummary "b"
        echoPairMiddlewareDecay
        echoPairSkipTable
        echoPairVerdict
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
deriveVersionRoot() {
    local bin="$1" root derived
    root="$PROJECT_ROOT"
    derived=$(git -C "$(dirname "$bin")" rev-parse --show-toplevel 2>/dev/null || true)
    if [ -n "$derived" ]; then
        root="$derived"
    fi
    echo "$root"
}

VERSION_ROOT=$(deriveVersionRoot "$SERVER_BIN")

# 给了 --server-b 才进配对模式。B 侧版本根单独推，它的二进制可能在别的工作区
if [ -n "$SERVER_BIN_B" ]; then
    PAIRED=1
    VERSION_ROOT_B=$(deriveVersionRoot "$SERVER_BIN_B")
fi

# 只有配对模式才落报告；冒烟模式不碰 OUTPUT_FILE，给了 -o 也用不上
if [ "$PAIRED" -eq 1 ] && [ -z "$OUTPUT_FILE" ]; then
    OUTPUT_FILE="$SCRIPT_DIR/result-paired.md"
fi

# 被 --only 选中的场景数，横幅里报这个数
SELECTED_SCENE_COUNT=0
for _def in "${SCENES[@]}"; do
    IFS='|' read -r _sid _ <<< "$_def"
    if sceneSelected "$_sid"; then
        SELECTED_SCENE_COUNT=$((SELECTED_SCENE_COUNT + 1))
    fi
done

echo "============================================"
echo "  Hical VM 本地压测"
echo "  服务:     $SERVER_BIN"
if [ "$PAIRED" -eq 1 ]; then
    echo "  B 侧:     $SERVER_BIN_B"
    echo "  模式:     A/B 配对对比   配对数: $ROUNDS"
else
    echo "  模式:     单版本冒烟（只打终端，不产出报告；正式对比请带 --server-b）"
    echo "  线程:     $THREADS   持续时间: $DURATION   轮数: $ROUNDS"
fi
echo "  场景数:   $SELECTED_SCENE_COUNT"
echo "============================================"
echo ""
if [ "$PAIRED" -eq 0 ] && [ "$OUTPUT_GIVEN" -eq 1 ]; then
    echo "[警告] 冒烟模式不产出报告文件，-o ${OUTPUT_FILE} 被忽略（要出报告请带 --server-b）"
    echo ""
fi
echo "[*] 项目根: $PROJECT_ROOT"

# 内核参数调优需要密码，提到最前面来，一跑脚本就输完，后面全自动
setupSysctl

buildIfRequested
checkServerBinary
if [ "$PAIRED" -eq 1 ]; then
    checkServerBinary "$SERVER_BIN_B"
fi
checkPortFree
setupUlimit
setupPinning

if [ "$PAIRED" -eq 1 ]; then
    preflightPaired
    runAllPairs

    echo ""
    echo "[*] 生成报告..."
    writePairReport
else
    startServer
    waitReadyOrDie
    preflightSide "$BASE_URL" "单版本"
    # 10000 并发光连接就吃掉 10000 个 fd，不够就别硬上
    if sceneSelected c10000 && ! fdLimitEnough "$ULIMIT_AFTER" "$MIN_FDS_FOR_10K"; then
        if [ -z "${PAIR_SKIP[c10000]:-}" ]; then
            PAIR_SKIP["c10000"]="ulimit -n 只有 ${ULIMIT_AFTER}，低于 ${MIN_FDS_FOR_10K}，跑不出真实并发"
            PAIR_SKIP_ORDER+=("c10000")
        fi
    fi
    runSmokeScenes
fi

echo ""
echo "============================================"
echo "  压测完成"
if [ "$PAIRED" -eq 1 ]; then
    echo "  报告路径: $OUTPUT_FILE"
    echo "  绝对路径: $(absPath "$OUTPUT_FILE")"
else
    echo "  单版本冒烟，未产出报告文件"
fi
echo "============================================"
