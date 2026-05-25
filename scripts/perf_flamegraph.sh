#!/bin/bash
# perf 火焰图一键生成脚本
# 用法：sudo true && bash scripts/perf_flamegraph.sh [并发数] [持续秒数]
# 示例：sudo true && bash scripts/perf_flamegraph.sh 10000 30
#
# 前置条件：
#   1. 已编译 bench_server：cmake --build build -j$(nproc)
#   2. 已安装 perf：sudo apt-get install -y linux-tools-$(uname -r)
#   3. 已克隆 FlameGraph：git clone --depth 1 https://github.com/brendangregg/FlameGraph.git ~/FlameGraph
#   4. ulimit -n >= 65535（高并发时）
#   5. 先执行 sudo true 刷新密码缓存，避免后台 sudo 卡住

set -e

CONNECTIONS=${1:-10000}
DURATION=${2:-30}
FLAMEGRAPH_DIR="${FLAMEGRAPH_DIR:-$HOME/FlameGraph}"

# 检查依赖
if ! command -v perf &> /dev/null; then
    echo "错误：perf 未安装，执行 sudo apt-get install -y linux-tools-\$(uname -r)"
    exit 1
fi
if [ ! -d "$FLAMEGRAPH_DIR" ]; then
    echo "错误：FlameGraph 未安装，执行 git clone --depth 1 https://github.com/brendangregg/FlameGraph.git ~/FlameGraph"
    exit 1
fi
if ! command -v wrk &> /dev/null; then
    echo "错误：wrk 未安装"
    exit 1
fi

# 检查 ulimit
FD_LIMIT=$(ulimit -n)
if [ "$FD_LIMIT" -lt 65535 ] && [ "$CONNECTIONS" -ge 10000 ]; then
    echo "警告：ulimit -n=$FD_LIMIT 不足，执行 ulimit -n 65535"
    exit 1
fi

# 清理残留进程
pkill bench_server 2>/dev/null || true
sleep 1

echo "=== 启动 bench_server ==="
./build/bench_server &
SERVER_PID=$!
sleep 1

# 验证服务启动
if ! curl -sf http://127.0.0.1:8080/ > /dev/null; then
    echo "错误：bench_server 启动失败"
    kill $SERVER_PID 2>/dev/null
    exit 1
fi
echo "bench_server PID: $SERVER_PID"

echo "=== 开始 perf 录制（${DURATION}s）+ wrk 压测（-c${CONNECTIONS}）==="
sudo perf record -g -F 999 -p $SERVER_PID -- sleep "$DURATION" &
PERF_PID=$!
sleep 1

wrk -t4 -c"$CONNECTIONS" -d"${DURATION}s" http://127.0.0.1:8080/

wait $PERF_PID
echo ""
echo "=== 生成火焰图 ==="
sudo perf script > perf_out.txt
"$FLAMEGRAPH_DIR/stackcollapse-perf.pl" perf_out.txt | "$FLAMEGRAPH_DIR/flamegraph.pl" > flame.svg

echo "=== 清理 ==="
kill $SERVER_PID 2>/dev/null || true
rm -f perf_out.txt perf.data perf.data.old

ls -lh flame.svg
echo ""
echo "完成！火焰图已生成：flame.svg"
echo "拷贝到宿主机查看：cp flame.svg /mnt/hical_host/docker/"
