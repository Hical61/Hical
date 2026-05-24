#!/bin/bash
# Benchmark 启动脚本：设置网络 softirq 亲和后运行 bench_server
# 需要容器有 CAP_NET_ADMIN 权限（或 --privileged）

# 启用 RPS（Receive Packet Steering）：将收包 softirq 分散到所有 CPU
# 减少单核 softirq 积压导致的 sendto 延迟
for rxq in /sys/class/net/eth0/queues/rx-*/rps_cpus; do
    echo "f" > "$rxq" 2>/dev/null
done

# 启用 RFS（Receive Flow Steering）：将收包处理对齐到消费该 flow 的 CPU
# 与 SO_REUSEPORT + CPU affinity 配合，减少跨核 IPI
if [ -w /proc/sys/net/core/rps_sock_flow_entries ]; then
    echo 32768 > /proc/sys/net/core/rps_sock_flow_entries
fi
for rxq in /sys/class/net/eth0/queues/rx-*/rps_flow_cnt; do
    echo 32768 > "$rxq" 2>/dev/null
done

exec /bench_server
