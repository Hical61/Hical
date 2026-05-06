#!/usr/bin/env bash
set -euo pipefail

# ============================================================
# collect_stats.sh — 采集内存、二进制大小、镜像大小、代码行数
#
# 前置条件：docker compose up -d 已启动所有服务
# 运行方式：cd benchmark && bash collect_stats.sh
# 输出文件：benchmark/stats.md
# ============================================================

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUTPUT="$SCRIPT_DIR/stats.md"

# 容器名（docker compose 默认前缀为目录名）
PREFIX="benchmark"
HICAL="${PREFIX}-hical-1"
GIN="${PREFIX}-gin-1"
ACTIX="${PREFIX}-actix-1"
WRK="${PREFIX}-wrk-1"

# 压测参数
DURATION=30
THREADS=4
CONNECTIONS=100
SAMPLE_DELAY=5

# 颜色输出
info()  { printf "\033[1;34m[INFO]\033[0m  %s\n" "$1"; }
ok()    { printf "\033[1;32m[OK]\033[0m    %s\n" "$1"; }
err()   { printf "\033[1;31m[ERR]\033[0m   %s\n" "$1" >&2; }

# ============================================================
# 1. 检查容器状态
# ============================================================
info "检查容器运行状态..."

for c in "$HICAL" "$GIN" "$ACTIX" "$WRK"; do
    if ! docker inspect --format='{{.State.Running}}' "$c" 2>/dev/null | grep -q true; then
        err "容器 $c 未运行，请先执行 docker compose up -d"
        exit 1
    fi
done
ok "所有容器正常运行"

# ============================================================
# 2. 采集空载内存
# ============================================================
info "采集空载内存..."

idle_hical=$(docker stats --no-stream --format '{{.MemUsage}}' "$HICAL" | awk -F'/' '{gsub(/ /,"",$1); print $1}')
idle_gin=$(docker stats --no-stream --format '{{.MemUsage}}' "$GIN" | awk -F'/' '{gsub(/ /,"",$1); print $1}')
idle_actix=$(docker stats --no-stream --format '{{.MemUsage}}' "$ACTIX" | awk -F'/' '{gsub(/ /,"",$1); print $1}')

ok "空载: Hical=$idle_hical  Gin=$idle_gin  Actix=$idle_actix"

# ============================================================
# 3. 压测中采集满载内存
# ============================================================
info "启动压测 (${DURATION}s) 并在 ${SAMPLE_DELAY}s 后采样满载内存..."

# 前台启动压测进程（后台子 shell），确保 wrk 进程实际运行
docker exec "$WRK" sh -c "\
    wrk -t${THREADS} -c${CONNECTIONS} -d${DURATION}s http://hical:8080/ >/dev/null 2>&1 & \
    wrk -t${THREADS} -c${CONNECTIONS} -d${DURATION}s http://gin:8081/ >/dev/null 2>&1 & \
    wrk -t${THREADS} -c${CONNECTIONS} -d${DURATION}s http://actix:8082/ >/dev/null 2>&1 & \
    sleep 2 && echo 'wrk started' && wait" &
WRK_PID=$!

sleep "$SAMPLE_DELAY"

# 多次采样取值（Windows Docker stats 刷新间隔较长）
info "采样满载数据（3 次，间隔 2s）..."
best_load_hical="" best_load_gin="" best_load_actix=""
best_cpu_hical="" best_cpu_gin="" best_cpu_actix=""

for i in 1 2 3; do
    load_hical=$(docker stats --no-stream --format '{{.MemUsage}}' "$HICAL" | awk -F'/' '{gsub(/ /,"",$1); print $1}')
    load_gin=$(docker stats --no-stream --format '{{.MemUsage}}' "$GIN" | awk -F'/' '{gsub(/ /,"",$1); print $1}')
    load_actix=$(docker stats --no-stream --format '{{.MemUsage}}' "$ACTIX" | awk -F'/' '{gsub(/ /,"",$1); print $1}')
    cpu_hical=$(docker stats --no-stream --format '{{.CPUPerc}}' "$HICAL")
    cpu_gin=$(docker stats --no-stream --format '{{.CPUPerc}}' "$GIN")
    cpu_actix=$(docker stats --no-stream --format '{{.CPUPerc}}' "$ACTIX")

    # 保留最后一次有效采样作为结果
    best_load_hical="$load_hical"; best_load_gin="$load_gin"; best_load_actix="$load_actix"
    best_cpu_hical="$cpu_hical"; best_cpu_gin="$cpu_gin"; best_cpu_actix="$cpu_actix"

    [ "$i" -lt 3 ] && sleep 2
done

load_hical="$best_load_hical"; load_gin="$best_load_gin"; load_actix="$best_load_actix"
cpu_hical="$best_cpu_hical"; cpu_gin="$best_cpu_gin"; cpu_actix="$best_cpu_actix"

ok "满载: Hical=$load_hical (CPU $cpu_hical)  Gin=$load_gin (CPU $cpu_gin)  Actix=$load_actix (CPU $cpu_actix)"

# 等压测结束
info "等待压测进程结束..."
wait "$WRK_PID" 2>/dev/null || true

# ============================================================
# 4. 二进制文件大小
# ============================================================
info "采集二进制文件大小..."

bin_hical=$(docker exec "$HICAL" sh -c "ls -lh /server" | awk '{print $5}')
bin_gin=$(docker exec "$GIN" sh -c "ls -lh /server" | awk '{print $5}')
bin_actix=$(docker exec "$ACTIX" sh -c "ls -lh /server" | awk '{print $5}')

ok "二进制: Hical=$bin_hical  Gin=$bin_gin  Actix=$bin_actix"

# ============================================================
# 5. Docker 镜像大小
# ============================================================
info "采集 Docker 镜像大小..."

img_hical=$(docker images "${PREFIX}-hical" --format '{{.Size}}' | head -1)
img_gin=$(docker images "${PREFIX}-gin" --format '{{.Size}}' | head -1)
img_actix=$(docker images "${PREFIX}-actix" --format '{{.Size}}' | head -1)

ok "镜像: Hical=$img_hical  Gin=$img_gin  Actix=$img_actix"

# ============================================================
# 6. 代码行数
# ============================================================
info "统计代码行数..."

loc_hical=$(wc -l < "$SCRIPT_DIR/hical/main.cpp")
loc_gin=$(wc -l < "$SCRIPT_DIR/gin/main.go")
loc_actix=$(wc -l < "$SCRIPT_DIR/actix/src/main.rs")

ok "代码行数: Hical=${loc_hical}  Gin=${loc_gin}  Actix=${loc_actix}"

# ============================================================
# 7. 生成 stats.md
# ============================================================
info "生成 $OUTPUT ..."

TIMESTAMP=$(date '+%Y-%m-%d %H:%M:%S')

cat > "$OUTPUT" << EOF
# 补充数据采集结果

> 采集时间：${TIMESTAMP}
> 采集工具：collect_stats.sh
> 压测参数：wrk -t${THREADS} -c${CONNECTIONS} -d${DURATION}s

---

## 1. 内存占用

| 框架             | 空载内存       | 满载内存       | 满载 CPU |
| ---------------- | -------------- | -------------- | -------- |
| Hical (C++)      | ${idle_hical}  | ${load_hical}  | ${cpu_hical} |
| Gin (Go)         | ${idle_gin}    | ${load_gin}    | ${cpu_gin} |
| Actix-web (Rust) | ${idle_actix}  | ${load_actix}  | ${cpu_actix} |

> 空载：服务启动后无请求压力。满载：三个服务同时被 wrk 压测时采样（${SAMPLE_DELAY}s 时刻）。

---

## 2. 二进制 & 镜像大小

| 框架             | 二进制大小 | Docker 镜像大小 |
| ---------------- | ---------- | --------------- |
| Hical (C++)      | ${bin_hical} | ${img_hical} |
| Gin (Go)         | ${bin_gin}   | ${img_gin}   |
| Actix-web (Rust) | ${bin_actix} | ${img_actix} |

---

## 3. 代码行数（wc -l，含注释和空行）

| 框架             | 文件              | 行数     |
| ---------------- | ----------------- | -------: |
| Hical (C++)      | hical/main.cpp    | ${loc_hical} |
| Gin (Go)         | gin/main.go       | ${loc_gin} |
| Actix-web (Rust) | actix/src/main.rs | ${loc_actix} |

---

> 数据采集于 Docker 容器环境（每容器 4 CPU / 512MB 限制）。
> 内存数据来自 \`docker stats --no-stream\`，为容器级 RSS。
> 二进制大小为容器内 \`/server\` 文件，镜像大小来自 \`docker images\`。
EOF

ok "数据已写入 $OUTPUT"
echo ""
cat "$OUTPUT"
