#!/usr/bin/env python3
"""
Hical 每连接内存成本测量 + 百万连接外推脚本

真造 100 万连接需要多机 + 几十 GB 内存 + 大改内核，成本极高。
工程上更务实的做法：在现有环境测准「每连接实际内存成本」，再线性外推到百万。

测试逻辑：
  1. 记录服务端空载 RSS（基线）
  2. 分多个档位累加建立长连接（持有不关），每档稳定后采样服务端 RSS
  3. 对 (连接数, RSS) 做最小二乘线性回归，得斜率 k（每连接字节）+ 截距 b（固定开销）
  4. 用 R² 判断是否真线性——线性成立，百万就只是「内存够不够」的问题
  5. 外推到 100 万，输出内存预算表

前提与约束：
  - 服务端必须跑在【本机】（脚本读 /proc/<pid>/status 取 RSS）。
    如果服务端在远端，本脚本测不到它的 RSS——需要在服务端机器上单独跑采样。
  - 单台客户端 + 单 IP + 单端口，受 TCP 四元组端口墙限制，档位上限建议 <= 55000。
    这足够做线性回归外推，不需要真摸到百万。
  - 要在单机突破 6 万端口墙做 10 万级实测对齐，用 --src-ips 配多个环回别名：
    端口墙是「每个 (源IP, 目标ip:port) 四元组」的墙，不是机器的墙，多一个源 IP
    就多一份 ~6 万源端口空间。服务端仍是单进程，RSS 采样逻辑不变。

用法:
  # 自动按进程名找服务端 PID（默认 bench_server）
  python3 scripts/conn_mem_extrapolate.py

  # 指定 PID（最可靠）
  python3 scripts/conn_mem_extrapolate.py --pid 12345

  # 自定义档位（累加值，逗号分隔）和目标外推连接数
  python3 scripts/conn_mem_extrapolate.py --steps 10000,20000,30000,50000 --target 1000000

  # 指定进程名
  python3 scripts/conn_mem_extrapolate.py --proc-name bench_server

  # 单机 10 万级对齐（先配环回别名 + 调大 fd）：
  #   sudo ip addr add 127.0.0.2/8 dev lo
  #   sudo ip addr add 127.0.0.3/8 dev lo
  #   sudo ip addr add 127.0.0.4/8 dev lo
  #   ulimit -n 300000
  python3 scripts/conn_mem_extrapolate.py \
      --src-ips 127.0.0.1,127.0.0.2,127.0.0.3,127.0.0.4 \
      --steps 50000,80000,100000
"""

import argparse
import errno
import os
import socket
import sys
import time
from concurrent.futures import ThreadPoolExecutor


# ==================== 辅助函数 ====================

def log(msg: str):
    """带时间戳的日志输出"""
    ts = time.strftime("%H:%M:%S")
    print(f"[{ts}] {msg}")


def human_bytes(n: float) -> str:
    """字节数转（KB/MB/GB）"""
    for unit in ("B", "KB", "MB", "GB", "TB"):
        if abs(n) < 1024.0:
            return f"{n:.2f} {unit}"
        n /= 1024.0
    return f"{n:.2f} PB"


# ==================== 服务端 RSS 采样 ====================

def find_pid_by_name(proc_name: str) -> int:
    """
    按进程名查找 PID（遍历 /proc，匹配 comm）。
    多个匹配时返回第一个并告警。找不到返回 0。
    """
    if not os.path.isdir("/proc"):
        return 0

    matches = []
    for entry in os.listdir("/proc"):
        if not entry.isdigit():
            continue
        try:
            with open(f"/proc/{entry}/comm", encoding="utf-8") as f:
                comm = f.read().strip()
            # comm 被内核截断到 15 字符，所以用前缀匹配
            if comm == proc_name or comm == proc_name[:15]:
                matches.append(int(entry))
        except OSError:
            continue

    if not matches:
        return 0
    if len(matches) > 1:
        log(f"  [WARN] 找到多个 '{proc_name}' 进程 {matches}，用第一个 {matches[0]}。建议用 --pid 指定")
    return matches[0]


def read_rss_bytes(pid: int) -> int:
    """
    读取进程 RSS（常驻内存），单位字节。
    多线程进程是单 PID，VmRSS 已经包含所有线程的物理内存，无需累加。
    @return RSS 字节数，读失败返回 -1
    """
    try:
        with open(f"/proc/{pid}/status", encoding="utf-8") as f:
            for line in f:
                if line.startswith("VmRSS:"):
                    # 格式: "VmRSS:    123456 kB"
                    kb = int(line.split()[1])
                    return kb * 1024
    except (OSError, ValueError, IndexError):
        return -1
    return -1


def sample_rss(pid: int, samples: int = 5, interval: float = 0.3) -> int:
    """
    多次采样 RSS 取均值，平掉抖动（GC、缓冲池伸缩会让 RSS 短时波动）。
    @return 平均 RSS 字节数
    """
    vals = []
    for _ in range(samples):
        v = read_rss_bytes(pid)
        if v > 0:
            vals.append(v)
        time.sleep(interval)
    if not vals:
        return -1
    return sum(vals) // len(vals)


# ==================== 建连 ====================

def _connect_one(host: str, port: int, src_ip: str = "", timeout: float = 3.0):
    """
    建立单个 TCP 连接，成功返回 socket，失败返回 None。
    src_ip 非空时把出站源地址绑到它——单机靠多个环回别名(127.0.0.2/3/4)
    突破端口墙：每个 (源IP, 目标ip:port) 四元组各有独立的 ~6 万源端口空间。
    """
    s = None
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.settimeout(timeout)
        if src_ip:
            # 源端口给 0 让内核自选；绑的是源 IP，不是固定端口
            s.bind((src_ip, 0))
        s.connect((host, port))
        return s
    except OSError as e:
        if s is not None:
            try:
                s.close()
            except OSError:
                pass
        # 客户端 fd 耗尽要醒目报警（最常见的环境坑）
        if e.errno in (errno.EMFILE, errno.ENFILE):
            return "EMFILE"
        return None


def connect_more(
    host: str,
    port: int,
    held: list,
    target_total: int,
    concurrency: int,
    src_ips: list = None,
) -> int:
    """
    把持有连接数从当前累加到 target_total（只增不减，避免反复建拆）。
    src_ips 非空时按 round-robin 把连接均摊到各源 IP，单机突破 6 万端口墙。
    @return 实际持有的连接总数
    """
    need = target_total - len(held)
    if need <= 0:
        return len(held)

    emfile_seen = False
    done = 0
    # 用 held 当前长度做起点轮转，保证跨档位累加时各源 IP 仍然均匀
    base = len(held)
    with ThreadPoolExecutor(max_workers=concurrency) as pool:
        futures = []
        for i in range(need):
            src_ip = src_ips[(base + i) % len(src_ips)] if src_ips else ""
            futures.append(pool.submit(_connect_one, host, port, src_ip))
        for fut in futures:
            r = fut.result()
            done += 1
            if r == "EMFILE":
                emfile_seen = True
            elif r is not None:
                held.append(r)
            if done % 5000 == 0:
                log(f"    建连进度 {done}/{need}: 当前持有 {len(held)}")

    if emfile_seen:
        # 单机自连接时客户端和服务端共用一套 fd（同进程算一份、同机两进程算两份），
        # 10 万对齐建议 ulimit -n 直接给到 30 万兜底
        log(f"  [WARN] 出现 EMFILE（客户端 fd 耗尽），档位未建满。请 ulimit -n 调大到 > {target_total * 2 + 5000}")
    return len(held)


def close_all(sockets: list):
    """关闭所有 socket"""
    for s in sockets:
        try:
            s.close()
        except OSError:
            pass
    sockets.clear()


# ==================== 线性回归 ====================

def linear_regression(xs: list, ys: list) -> tuple:
    """
    最小二乘线性回归 y = k*x + b（纯标准库，不依赖 numpy）。
    @return (k 斜率, b 截距, r2 决定系数)。样本不足返回 (0, 0, 0)。
    """
    n = len(xs)
    if n < 2:
        return 0.0, 0.0, 0.0

    sx = sum(xs)
    sy = sum(ys)
    sxx = sum(x * x for x in xs)
    sxy = sum(x * y for x, y in zip(xs, ys))

    denom = n * sxx - sx * sx
    if denom == 0:
        return 0.0, 0.0, 0.0

    k = (n * sxy - sx * sy) / denom
    b = (sy - k * sx) / n

    # R²：1 - 残差平方和 / 总平方和
    mean_y = sy / n
    ss_tot = sum((y - mean_y) ** 2 for y in ys)
    ss_res = sum((y - (k * x + b)) ** 2 for x, y in zip(xs, ys))
    r2 = 1.0 - ss_res / ss_tot if ss_tot > 0 else 1.0

    return k, b, r2


# ==================== 主流程 ====================

def parse_steps(raw: str) -> list:
    """解析 --steps，去重排序"""
    try:
        steps = sorted({int(x.strip()) for x in raw.split(",") if x.strip()})
    except ValueError:
        log(f"[FATAL] --steps 格式错误: {raw}")
        sys.exit(1)
    return [s for s in steps if s > 0]


def run_measurement(host, port, pid, steps, concurrency, src_ips=None):
    """
    执行测量：空载基线 + 各档位采样。
    @return (datapoints, held)
        datapoints: [(连接数, RSS增量字节), ...]，只含真实档位点
        held: 持有的 socket 列表（调用方负责清理）

    注意：不把 (0, 0) 塞进回归。空载 baseline 只用来算各档 delta；
    强行过原点会把回归截距 b 往 0 拉、污染斜率 k。让回归自己拟合出
    截距，b 才能真实反映「固定开销」。
    """
    log("采样空载 RSS（基线）...")
    baseline = sample_rss(pid)
    if baseline <= 0:
        log("[FATAL] 无法读取服务端 RSS，确认 --pid 正确且进程在本机")
        sys.exit(1)
    log(f"  空载 RSS: {human_bytes(baseline)}")
    log("")

    datapoints = []  # (连接数, RSS增量) —— 只放真实档位
    held = []

    for target in steps:
        log(f"档位: 累加到 {target} 个长连接")
        actual = connect_more(host, port, held, target, concurrency, src_ips)
        if actual < target * 0.95:
            log(f"  [WARN] 只建到 {actual}/{target}，按实际数采样（可能受端口/fd 墙限制）")

        # 等服务端把这批连接的缓冲都分配稳定
        log("  等待 2 秒让 RSS 稳定...")
        time.sleep(2)

        rss = sample_rss(pid)
        delta = rss - baseline
        per_conn = delta / actual if actual > 0 else 0
        log(f"  连接数 {actual}, RSS {human_bytes(rss)}, "
            f"增量 {human_bytes(delta)}, 每连接 {human_bytes(per_conn)}")
        log("")

        datapoints.append((actual, delta))

    return datapoints, held


def report_extrapolation(datapoints, target):
    """对数据点做回归并打印外推报告。"""
    log("=" * 64)
    log("线性回归 + 百万外推")
    log("=" * 64)

    # 剔除 (0,0) 后回归至少要 2 个真实档位才有意义
    if len(datapoints) < 2:
        log(f"  [FATAL] 有效档位只有 {len(datapoints)} 个，回归至少需要 2 个。")
        log("          请用 --steps 给出至少两个能建满的档位（如 10000,30000）")
        return

    xs = [d[0] for d in datapoints]
    ys = [d[1] for d in datapoints]
    k, b, r2 = linear_regression(xs, ys)

    # 斜率非正说明数据异常（RSS 没随连接增长），外推无意义
    if k <= 0:
        log(f"  [FATAL] 回归斜率 k={human_bytes(k)} 非正，RSS 未随连接数增长。")
        log("          可能服务端不在本机、采样到了错误进程，或连接没真正建立。")
        return

    log(f"  回归模型: RSS增量 = {human_bytes(k)}/连接 × N + {human_bytes(b)}（固定开销）")
    log(f"  每连接边际内存成本: {human_bytes(k)}")
    log(f"  R^2 = {r2:.4f}  ({'线性良好' if r2 >= 0.99 else '线性偏差较大，结果存疑' if r2 < 0.95 else '基本线性'})")
    log("")

    if r2 < 0.95:
        log("  [WARN] R^2 < 0.95，数据点不够线性。可能原因：")
        log("         - 档位太少或太密集，加大档位跨度重测")
        log("         - 采样期间服务端有其他负载干扰 RSS")
        log("         - 内存池分块伸缩导致阶梯式增长")
        log("         - 多个档位都卡在端口墙（~6 万），x 值挤在一起")
        log("")

    # 外推表（target 去重排序，避免和固定档位重复）
    rows = sorted({100_000, 500_000, target, 2_000_000})
    log(f"  外推内存预算表（每连接边际成本 {human_bytes(k)}）:")
    log(f"  {'连接数':>12} | {'预估增量内存':>16} | {'含固定开销总计':>16}")
    log(f"  {'-' * 12}-+-{'-' * 16}-+-{'-' * 16}")
    for n in rows:
        delta = k * n
        total = delta + b
        log(f"  {n:>12,} | {human_bytes(delta):>16} | {human_bytes(total):>16}")
    log("")

    target_total = k * target + b
    log(f"  >>> {target:,} 连接预计需要约 {human_bytes(target_total)} 服务端 RSS（含固定开销）")
    log("")
    log("  [重要] 这是【空闲连接】的内存下限，真实生产会更高：")
    log("    1) 测的是建连后不发数据的空闲长连接。Hical 的 PMR 缓冲、HttpSession")
    log("       readBuf 等很多在收到请求时才分配，活跃连接每个会比这里更吃内存。")
    log("    2) 只算了服务端进程 RSS，内核每个 TCP socket 还有 sk_buff/读写缓冲（几 KB）。")
    log("    → 生产内存预算建议在外推值基础上再留 30%-50% 余量。")
    log("")


def main():
    parser = argparse.ArgumentParser(description="Hical 每连接内存成本测量 + 百万外推")
    parser.add_argument("--host", default="127.0.0.1", help="服务器地址 (默认 127.0.0.1)")
    parser.add_argument("--port", type=int, default=8080, help="服务器端口 (默认 8080)")
    parser.add_argument("--pid", type=int, default=0, help="服务端进程 PID（最可靠，不填则按进程名查找）")
    parser.add_argument("--proc-name", default="bench_server", help="服务端进程名 (默认 bench_server)")
    parser.add_argument("--steps", default="10000,20000,30000,50000",
                        help="累加建连档位，逗号分隔 (默认 10000,20000,30000,50000)")
    parser.add_argument("--target", type=int, default=1_000_000,
                        help="外推目标连接数 (默认 1000000)")
    parser.add_argument("--concurrency", type=int, default=300,
                        help="并发建连度 (默认 300)")
    parser.add_argument("--src-ips", default="",
                        help="出站源 IP 列表，逗号分隔。单机靠环回别名(127.0.0.2/3/4)"
                             "突破 ~6 万端口墙，每个源 IP 各贡献约 5.5 万连接。"
                             "需先 `sudo ip addr add 127.0.0.2/8 dev lo`")
    args = parser.parse_args()

    host = args.host
    port = args.port
    steps = parse_steps(args.steps)
    concurrency = max(1, args.concurrency)

    if not steps:
        log("[FATAL] --steps 为空")
        sys.exit(1)

    # 解析源 IP，并逐个预检——能 bind 才说明环回别名真配好了，
    # 否则跑到一半才 EADDRNOTAVAIL，几万连接白建
    src_ips = [ip.strip() for ip in args.src_ips.split(",") if ip.strip()]
    for ip in src_ips:
        try:
            probe = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            probe.bind((ip, 0))
            probe.close()
        except OSError as e:
            log(f"[FATAL] 源 IP {ip} 无法 bind: {e}。"
                f"请先 `sudo ip addr add {ip}/8 dev lo` 并用 `ip addr show lo` 确认")
            sys.exit(1)

    # 解析 PID
    pid = args.pid
    if pid <= 0:
        pid = find_pid_by_name(args.proc_name)
        if pid <= 0:
            log(f"[FATAL] 找不到进程 '{args.proc_name}'，请用 --pid 指定，"
                f"或确认服务端在【本机】运行（远端服务端测不到 RSS）")
            sys.exit(1)
        log(f"自动定位服务端 PID: {pid}（进程名 {args.proc_name}）")

    if read_rss_bytes(pid) <= 0:
        log(f"[FATAL] PID {pid} 无法读取 RSS，确认进程存在且在本机")
        sys.exit(1)

    log("Hical 每连接内存成本测量 + 百万外推")
    log(f"  目标服务端: {host}:{port} (PID {pid})")
    log(f"  建连档位: {steps}")
    log(f"  外推目标: {args.target:,} 连接")
    log(f"  并发建连度: {concurrency}")
    if src_ips:
        log(f"  出站源 IP: {src_ips}（每个约可贡献 5.5 万，单机突破端口墙）")
    log("")

    held = []
    try:
        datapoints, held = run_measurement(host, port, pid, steps, concurrency, src_ips)
        report_extrapolation(datapoints, args.target)
        log("测量完成")
    finally:
        # 兜底：异常退出（含 Ctrl-C）时关掉所有持有连接
        if held:
            log(f"清理 {len(held)} 个连接")
            close_all(held)


if __name__ == "__main__":
    main()
