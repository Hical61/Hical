#!/usr/bin/env bash
set -euo pipefail

# ============================================================
# collect_stats.sh — 采集内存、二进制大小、镜像大小、代码行数
#
# 前置条件：docker compose --profile <mode> up -d 已启动服务
# 运行方式：cd benchmark && BENCH_MODE=cpp bash collect_stats.sh
# 输出文件：benchmark/stats.md
# ============================================================

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUTPUT="$SCRIPT_DIR/stats.md"
PREFIX="benchmark"
WRK_CONTAINER="${PREFIX}-wrk-1"

# 运行模式（与 run_bench.sh 一致）
BENCH_MODE="${BENCH_MODE:-cpp}"

# 压测参数
DURATION=30
THREADS=4
CONNECTIONS=100
SAMPLE_DELAY=5

# 框架定义：显示名称|容器后缀|源码路径|服务端口
CPP_FRAMEWORKS=(
    "Hical (C++)|hical|hical/main.cpp|8080"
    "Drogon (C++)|drogon|drogon/main.cpp|8083"
    "Crow (C++)|crow|crow/main.cpp|8084"
    "Oat++ (C++)|oatpp|oatpp/main.cpp|8085"
    "cpp-httplib (C++)|cpphttplib|cpphttplib/main.cpp|8086"
    "Cinatra (C++)|cinatra|cinatra/main.cpp|8087"
)

CROSS_LANG_FRAMEWORKS=(
    "Hical (C++)|hical|hical/main.cpp|8080"
    "Gin (Go)|gin|gin/main.go|8081"
    "Actix-web (Rust)|actix|actix/src/main.rs|8082"
)

ALL_FRAMEWORKS=(
    "Hical (C++)|hical|hical/main.cpp|8080"
    "Drogon (C++)|drogon|drogon/main.cpp|8083"
    "Crow (C++)|crow|crow/main.cpp|8084"
    "Oat++ (C++)|oatpp|oatpp/main.cpp|8085"
    "cpp-httplib (C++)|cpphttplib|cpphttplib/main.cpp|8086"
    "Cinatra (C++)|cinatra|cinatra/main.cpp|8087"
    "Gin (Go)|gin|gin/main.go|8081"
    "Actix-web (Rust)|actix|actix/src/main.rs|8082"
)

case "$BENCH_MODE" in
    cpp)        FRAMEWORKS=("${CPP_FRAMEWORKS[@]}") ;;
    cross-lang) FRAMEWORKS=("${CROSS_LANG_FRAMEWORKS[@]}") ;;
    all)        FRAMEWORKS=("${ALL_FRAMEWORKS[@]}") ;;
    *)
        echo "[错误] 未知 BENCH_MODE: $BENCH_MODE（可选: cpp / cross-lang / all）"
        exit 1
        ;;
esac

# 颜色输出
info() { printf "\033[1;34m[INFO]\033[0m  %s\n" "$1"; }
ok()   { printf "\033[1;32m[OK]\033[0m    %s\n" "$1"; }
err()  { printf "\033[1;31m[ERR]\033[0m   %s\n" "$1" >&2; }

# 关联数组：以容器后缀为 key
declare -A DISPLAY_NAME CONTAINER SRC_PATH PORT
declare -A IDLE_MEM LOAD_MEM LOAD_CPU BIN_SIZE IMG_SIZE LOC

# 解析框架数组
for entry in "${FRAMEWORKS[@]}"; do
    IFS='|' read -r name suffix src port <<< "$entry"
    DISPLAY_NAME[$suffix]="$name"
    CONTAINER[$suffix]="${PREFIX}-${suffix}-1"
    SRC_PATH[$suffix]="$src"
    PORT[$suffix]="$port"
done

# 后缀顺序数组（保持输出顺序）
SUFFIXES=()
for entry in "${FRAMEWORKS[@]}"; do
    IFS='|' read -r name suffix src port <<< "$entry"
    SUFFIXES+=("$suffix")
done

# ============================================================
# 1. 检查容器状态
# ============================================================
info "检查容器运行状态..."

for suffix in "${SUFFIXES[@]}"; do
    c="${CONTAINER[$suffix]}"
    if ! docker inspect --format='{{.State.Running}}' "$c" 2>/dev/null | grep -q true; then
        err "容器 $c 未运行，请先执行 docker compose up -d"
        exit 1
    fi
done

if ! docker inspect --format='{{.State.Running}}' "$WRK_CONTAINER" 2>/dev/null | grep -q true; then
    err "容器 $WRK_CONTAINER 未运行，请先执行 docker compose up -d"
    exit 1
fi

ok "所有容器正常运行"

# ============================================================
# 2. 采集空载内存
# ============================================================
info "采集空载内存..."

for suffix in "${SUFFIXES[@]}"; do
    c="${CONTAINER[$suffix]}"
    IDLE_MEM[$suffix]=$(docker stats --no-stream --format '{{.MemUsage}}' "$c" \
        | awk -F'/' '{gsub(/ /,"",$1); print $1}')
done

idle_summary=""
for suffix in "${SUFFIXES[@]}"; do
    idle_summary+="${DISPLAY_NAME[$suffix]}=${IDLE_MEM[$suffix]}  "
done
ok "空载: ${idle_summary}"

# ============================================================
# 3. 压测中采集满载内存
# ============================================================
info "启动压测 (${DURATION}s) 并在 ${SAMPLE_DELAY}s 后采样满载内存..."

# 动态构建所有框架的 wrk 命令
wrk_cmds=""
for suffix in "${SUFFIXES[@]}"; do
    wrk_cmds+="wrk -t${THREADS} -c${CONNECTIONS} -d${DURATION}s http://${suffix}:${PORT[$suffix]}/ >/dev/null 2>&1 & "
done

docker exec "$WRK_CONTAINER" sh -c "${wrk_cmds}sleep 2 && echo 'wrk started' && wait" &
WRK_PID=$!

sleep "$SAMPLE_DELAY"

info "采样满载数据（3 次，间隔 2s）..."
for suffix in "${SUFFIXES[@]}"; do
    LOAD_MEM[$suffix]=""
    LOAD_CPU[$suffix]=""
done

for i in 1 2 3; do
    for suffix in "${SUFFIXES[@]}"; do
        c="${CONTAINER[$suffix]}"
        LOAD_MEM[$suffix]=$(docker stats --no-stream --format '{{.MemUsage}}' "$c" \
            | awk -F'/' '{gsub(/ /,"",$1); print $1}')
        LOAD_CPU[$suffix]=$(docker stats --no-stream --format '{{.CPUPerc}}' "$c")
    done
    [ "$i" -lt 3 ] && sleep 2
done

load_summary=""
for suffix in "${SUFFIXES[@]}"; do
    load_summary+="${DISPLAY_NAME[$suffix]}=${LOAD_MEM[$suffix]} (CPU ${LOAD_CPU[$suffix]})  "
done
ok "满载: ${load_summary}"

info "等待压测进程结束..."
wait "$WRK_PID" 2>/dev/null || true

# ============================================================
# 4. 二进制文件大小
# ============================================================
info "采集二进制文件大小..."

for suffix in "${SUFFIXES[@]}"; do
    c="${CONTAINER[$suffix]}"
    BIN_SIZE[$suffix]=$(docker exec "$c" sh -c "ls -lh /server" | awk '{print $5}')
done

bin_summary=""
for suffix in "${SUFFIXES[@]}"; do
    bin_summary+="${DISPLAY_NAME[$suffix]}=${BIN_SIZE[$suffix]}  "
done
ok "二进制: ${bin_summary}"

# ============================================================
# 5. Docker 镜像大小
# ============================================================
info "采集 Docker 镜像大小..."

for suffix in "${SUFFIXES[@]}"; do
    IMG_SIZE[$suffix]=$(docker images "${PREFIX}-${suffix}" --format '{{.Size}}' | head -1)
done

img_summary=""
for suffix in "${SUFFIXES[@]}"; do
    img_summary+="${DISPLAY_NAME[$suffix]}=${IMG_SIZE[$suffix]}  "
done
ok "镜像: ${img_summary}"

# ============================================================
# 6. 代码行数
# ============================================================
info "统计代码行数..."

for suffix in "${SUFFIXES[@]}"; do
    src_file="$SCRIPT_DIR/${SRC_PATH[$suffix]}"
    if [ -f "$src_file" ]; then
        LOC[$suffix]=$(wc -l < "$src_file")
    else
        LOC[$suffix]="N/A"
    fi
done

loc_summary=""
for suffix in "${SUFFIXES[@]}"; do
    loc_summary+="${DISPLAY_NAME[$suffix]}=${LOC[$suffix]}  "
done
ok "代码行数: ${loc_summary}"

# ============================================================
# 7. 生成 stats.md
# ============================================================
info "生成 $OUTPUT ..."

TIMESTAMP=$(date '+%Y-%m-%d %H:%M:%S')

{
    cat << EOF
# 补充数据采集结果

> 采集时间：${TIMESTAMP}
> 采集工具：collect_stats.sh
> 压测参数：wrk -t${THREADS} -c${CONNECTIONS} -d${DURATION}s

---

## 1. 内存占用

| 框架 | 空载内存 | 满载内存 | 满载 CPU |
| ---- | -------- | -------- | -------- |
EOF

    for suffix in "${SUFFIXES[@]}"; do
        printf "| %-20s | %-14s | %-14s | %-10s |\n" \
            "${DISPLAY_NAME[$suffix]}" \
            "${IDLE_MEM[$suffix]}" \
            "${LOAD_MEM[$suffix]}" \
            "${LOAD_CPU[$suffix]}"
    done

    cat << EOF

> 空载：服务启动后无请求压力。满载：所有服务同时被 wrk 压测时采样（${SAMPLE_DELAY}s 时刻）。

---

## 2. 二进制 & 镜像大小

| 框架 | 二进制大小 | Docker 镜像大小 |
| ---- | ---------- | --------------- |
EOF

    for suffix in "${SUFFIXES[@]}"; do
        printf "| %-20s | %-10s | %-15s |\n" \
            "${DISPLAY_NAME[$suffix]}" \
            "${BIN_SIZE[$suffix]}" \
            "${IMG_SIZE[$suffix]}"
    done

    cat << EOF

---

## 3. 代码行数（wc -l，含注释和空行）

| 框架 | 文件 | 行数 |
| ---- | ---- | ---: |
EOF

    for suffix in "${SUFFIXES[@]}"; do
        printf "| %-20s | %-25s | %s |\n" \
            "${DISPLAY_NAME[$suffix]}" \
            "${SRC_PATH[$suffix]}" \
            "${LOC[$suffix]}"
    done

# 从第一个框架容器中读取实际内存限制
_first_suffix="${SUFFIXES[0]}"
_first_container="${CONTAINER[$_first_suffix]}"
MEM_LIMIT_BYTES=$(docker inspect --format='{{.HostConfig.Memory}}' "$_first_container" 2>/dev/null || echo "0")
if [ "$MEM_LIMIT_BYTES" -gt 0 ] 2>/dev/null; then
    MEM_LIMIT_MB=$(( MEM_LIMIT_BYTES / 1024 / 1024 ))
else
    MEM_LIMIT_MB="unknown"
fi
# 读取 nofile 限制
NOFILE_LIMIT=$(docker inspect --format='{{(index .HostConfig.Ulimits 0).Hard}}' "$_first_container" 2>/dev/null || echo "unknown")

    cat << EOF

---

> 数据采集于 Docker 容器环境（每容器 4 CPU / ${MEM_LIMIT_MB}MB 限制，nofile=${NOFILE_LIMIT}）。
> 内存数据来自 \`docker stats --no-stream\`，为容器级 RSS。
> 二进制大小为容器内 \`/server\` 文件，镜像大小来自 \`docker images\`。
EOF
} > "$OUTPUT"

ok "数据已写入 $OUTPUT"
echo ""
cat "$OUTPUT"
