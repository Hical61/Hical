#!/usr/bin/env python3
"""
Hical 连接数上限测试脚本

验证 HttpServer::setMaxConnections() 限流是否正确生效。
测试逻辑：
  1. 并发建立 TCP 连接，观察成功/失败数（并发才能压出 accept 竞态）
  2. 验证成功连接数 <= maxConnections
  3. 复用已建立连接发 HTTP 请求，验证超限不影响已有连接服务
  4. 释放部分连接后验证新连接能建立（腾位测试）
  5. 全部释放后验证服务器恢复正常

用法:
  python3 scripts/conn_limit_test.py                  # 默认: 上限5000, 尝试6000
  python3 scripts/conn_limit_test.py --limit 1000     # 服务端设的上限
  python3 scripts/conn_limit_test.py --limit 1000 --attempt 1500
  python3 scripts/conn_limit_test.py --host 192.168.1.100 --port 8080
  python3 scripts/conn_limit_test.py --concurrency 500 # 并发建连度（默认 300）
  python3 scripts/conn_limit_test.py --release-count 1000  # 腾位测试释放数
  python3 scripts/conn_limit_test.py --skip-reclaim   # 跳过腾位测试（快速模式）
"""

import argparse
import errno
import socket
import sys
import time
from concurrent.futures import ThreadPoolExecutor


# 单台客户端 + 单 IP + 单端口受 TCP 四元组端口墙限制，源端口最多约 64K，
# 扣掉系统占用实际能稳定建立约 6 万连接。超过这个量级本脚本既测不准、
# 又会因一次性持有百万 socket 把客户端自己撑爆。容量评估应改用
# scripts/conn_mem_extrapolate.py（层次 B 外推）或多机 + tcpkali（层次 C）。
kSingleHostConnLimit = 60000


# ==================== 辅助函数 ====================

def log(msg: str):
    """带时间戳的日志输出"""
    ts = time.strftime("%H:%M:%S")
    print(f"[{ts}] {msg}")


class ConnectStats:
    """
    一批并发建连的结果统计。
    按 errno 区分失败类型，便于判断是「框架限流」还是「环境问题」。
    """

    def __init__(self):
        self.sockets: list[socket.socket] = []  # 成功建立的连接
        self.reset = 0                          # 服务端 accept 后主动 close（限流生效）
        self.refused = 0                        # listen backlog 满（内核层，非框架）
        self.emfile = 0                         # 客户端自己 fd 耗尽（测试环境问题，结果无效）
        self.other = 0                          # 其他错误（超时等）

    @property
    def connected(self) -> int:
        return len(self.sockets)

    @property
    def failed(self) -> int:
        return self.reset + self.refused + self.emfile + self.other

    def summary(self) -> str:
        return (f"成功 {self.connected}, 失败 {self.failed} "
                f"(reset={self.reset} refused={self.refused} "
                f"emfile={self.emfile} other={self.other})")


def _connect_one(host: str, port: int, timeout: float = 3.0):
    """
    建立单个 TCP 连接，并探测服务端是否真正保留了它。

    关键：Linux 上 connect() 在三次握手完成、连接进入服务端 backlog 队列时就返回成功，
    哪怕服务端 acceptLoop 取出后因超限立即 close()，客户端的 connect 也早已返回成功了。
    所以光看 connect 返回值会把「被服务端踢掉的超限连接」误判为成功。
    这里 connect 成功后做一次存活探测：短超时 recv，
      - 收到 EOF(b"") 或 ECONNRESET → 服务端已关闭这个连接（超限被踢），算 reset 失败
      - 超时（无数据但连接还在）→ 连接被服务端保留，真正成功

    @return (socket 或 None, 失败分类标签)。成功时标签为 None。
            标签取值: "reset" / "refused" / "emfile" / "other"
    """
    s = None
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.settimeout(timeout)
        s.connect((host, port))

        # 存活探测：短超时读一下，看对端是否已经把我们踢了
        s.settimeout(0.3)
        try:
            data = s.recv(1)
            if data == b"":
                # 对端已关闭（FIN）→ 超限被踢
                s.close()
                return None, "reset"
            # 居然收到了数据（一般不会，服务端不会主动推）——当作存活，继续持有
        except socket.timeout:
            pass  # 超时 = 没数据但连接健在 = 真正成功
        # 探测完恢复成阻塞，交回调用方持有
        s.settimeout(None)
        return s, None
    except OSError as e:
        if s is not None:
            try:
                s.close()
            except OSError:
                pass
        # 按 errno 精确分类，把不同语义的失败拆开
        ec = e.errno
        if ec in (errno.ECONNRESET, errno.ECONNABORTED):
            return None, "reset"
        if ec == errno.ECONNREFUSED:
            return None, "refused"
        if ec in (errno.EMFILE, errno.ENFILE):
            return None, "emfile"
        return None, "other"


def batch_connect(
    host: str,
    port: int,
    count: int,
    concurrency: int,
    batch_label: str = "",
) -> ConnectStats:
    """
    并发建立 TCP 连接。

    串行建连压不出 accept 竞态（check activeConnections_ 到 close 的窗口需要
    瞬时并发涌入才会暴露），所以这里用线程池并发提交。

    @param host 目标地址
    @param port 目标端口
    @param count 尝试连接数
    @param concurrency 并发度（同时在飞的 connect 数）
    @param batch_label 日志前缀标签
    @return ConnectStats 统计结果
    """
    stats = ConnectStats()
    label = f"[{batch_label}] " if batch_label else ""
    done = 0
    first_errors = 0  # 前几个失败打印具体错误，方便调试

    with ThreadPoolExecutor(max_workers=concurrency) as pool:
        futures = [pool.submit(_connect_one, host, port) for _ in range(count)]
        for fut in futures:
            sock, tag = fut.result()
            done += 1
            if sock is not None:
                stats.sockets.append(sock)
            else:
                if tag == "reset":
                    stats.reset += 1
                elif tag == "refused":
                    stats.refused += 1
                elif tag == "emfile":
                    stats.emfile += 1
                else:
                    stats.other += 1
                if first_errors < 3:
                    first_errors += 1
                    log(f"  {label}有连接失败 (类型={tag})")

            if done % 500 == 0:
                log(f"  {label}进度 {done}/{count}: 成功 {stats.connected}, 失败 {stats.failed}")

    return stats


def close_sockets(sockets: list[socket.socket]):
    """关闭一批 socket"""
    for s in sockets:
        try:
            s.close()
        except OSError:
            pass


def check_server_alive(host: str, port: int) -> bool:
    """检测服务器是否能建立新连接"""
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.settimeout(3)
        s.connect((host, port))
        s.close()
        return True
    except OSError:
        return False


def http_request_on(sock: socket.socket) -> tuple[bool, int]:
    """
    在一个已建立的 socket 上发 HTTP GET / 请求，返回 (是否成功, 状态码)。
    用于验证超限后已有连接仍能正常服务——必须复用已建立连接，
    而不是新建（满载时新建必被拒）。

    注: 用 Connection: keep-alive，发完不关 socket，调用方继续持有。
    """
    try:
        sock.settimeout(5)
        sock.sendall(b"GET / HTTP/1.1\r\nHost: localhost\r\nConnection: keep-alive\r\n\r\n")
        resp = sock.recv(4096)
        if not resp:
            return False, 0
        first_line = resp.split(b"\r\n")[0].decode("ascii", errors="replace")
        status_code = int(first_line.split()[1])
        return True, status_code
    except (OSError, ValueError, IndexError):
        return False, 0


def http_request_new(host: str, port: int) -> tuple[bool, int]:
    """新建连接发一个 HTTP GET / 请求（用于恢复验证）。返回 (是否成功, 状态码)。"""
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.settimeout(5)
        s.connect((host, port))
        s.sendall(b"GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n")
        resp = b""
        while True:
            chunk = s.recv(4096)
            if not chunk:
                break
            resp += chunk
        s.close()
        first_line = resp.split(b"\r\n")[0].decode("ascii", errors="replace")
        status_code = int(first_line.split()[1])
        return True, status_code
    except (OSError, ValueError, IndexError):
        return False, 0


def poll_until(predicate, timeout: float, interval: float = 0.5) -> bool:
    """
    轮询等待 predicate() 为真，最多等 timeout 秒。
    替代固定 sleep——连接回收快时早退出，慢时多等一会，比硬编码 sleep 准。

    @return predicate 在超时前为真返回 True，否则 False
    """
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return True
        time.sleep(interval)
    return predicate()


# ==================== 测试阶段 ====================

def test_fill_to_limit(
    host: str,
    port: int,
    limit: int,
    attempt: int,
    concurrency: int,
) -> list[socket.socket]:
    """
    阶段 1: 并发建连，验证上限生效。

    @return 成功建立的 socket 列表（后续测试用）
    """
    log(f"阶段 1: 并发建立 {attempt} 个连接（并发度 {concurrency}，服务端上限 {limit}）")
    log("=" * 60)

    stats = batch_connect(host, port, attempt, concurrency, "填充")
    connected = stats.connected

    log("")
    log(f"  结果: 尝试 {attempt}, {stats.summary()}")

    # 客户端自己 fd 耗尽 → 结果无效，优先报警
    if stats.emfile > 0:
        log(f"  [WARN] 出现 {stats.emfile} 次 EMFILE/ENFILE（客户端 fd 耗尽），"
            f"测试结果可能失真。检查 ulimit -n（需 >= {attempt + 1000}）")

    # 判定
    # 允许 5% 容差: 多线程 accept 在检查 activeConnections_ 和实际 close 之间有微小窗口
    tolerance = max(int(limit * 0.05), 10)

    if limit > 0 and connected > limit + tolerance:
        log(f"  [FAIL] 成功连接数 {connected} 超过上限 {limit} + 容差 {tolerance}")
    elif stats.failed == 0 and attempt > limit > 0:
        log(f"  [FAIL] 尝试 {attempt} 次全部成功，限流未生效")
    elif limit > 0 and attempt > limit and stats.reset == 0 and stats.refused > 0:
        # 全靠 backlog 满拒绝、没有一个 reset，说明框架层根本没拒到，疑似没设上限
        log(f"  [WARN] 超限连接全被 backlog 拒绝（refused={stats.refused}），"
            f"无 reset。可能 setMaxConnections 未生效，或 backlog 太小先满了")
    elif limit > 0 and stats.reset > 0 and connected < limit * 0.8:
        # 服务端确实在踢连接（reset>0），但触顶值远低于脚本声称的 --limit。
        # 说明服务端真实上限比 --limit 小——多半是改了 bench_main 没重编/没重启，
        # 或 --limit 传错了。这种情况 connected <= limit 恒成立，会假装 PASS，必须点破。
        log(f"  [WARN] 服务端实际只接受了 ~{connected} 个就开始拒（reset={stats.reset}），"
            f"远低于声称的上限 {limit}。")
        log(f"         服务端真实上限可能是 ~{connected}，而非 {limit}——"
            f"检查 bench_main.cpp 的 setMaxConnections 是否改了并【重新编译+重启】。")
    elif limit > 0:
        log(f"  [PASS] 成功连接数 {connected} <= 上限 {limit} (容差 {tolerance})")
    else:
        log(f"  [INFO] maxConnections=0 (不限制)，全部 {connected} 个连接成功")

    log("")
    return stats.sockets


def test_existing_connections_work(held_sockets: list[socket.socket]):
    """
    阶段 2: 验证已有连接仍能正常处理 HTTP 请求。
    超限只拒绝新连接，不影响已有连接的服务能力。

    关键: 复用 held 列表里已建立的连接发请求，而不是新建——
    满载时新建必被拒，那测的就不是「已有连接能否服务」了。
    """
    log("阶段 2: 验证已有连接正常服务（复用已建立连接）")
    log("=" * 60)

    if not held_sockets:
        log("  [SKIP] 当前没有持有任何连接，跳过")
        log("")
        return

    # 拿持有列表里第一个连接发请求
    ok, status = http_request_on(held_sockets[0])
    if ok and status == 200:
        log(f"  [PASS] 已有连接 HTTP GET / 返回 {status}，超限不影响已建立连接")
    elif ok:
        log(f"  [WARN] 已有连接 HTTP GET / 返回 {status} (非 200)")
    else:
        log(f"  [FAIL] 已有连接无法服务（请求失败），超限不应影响已建立连接")

    log("")


def test_reclaim_slots(
    host: str,
    port: int,
    held_sockets: list[socket.socket],
    release_count: int,
    concurrency: int,
):
    """
    阶段 3: 腾位测试。
    释放一部分连接后，新连接应该能建立。
    """
    log(f"阶段 3: 释放 {release_count} 个连接后验证新连接")
    log("=" * 60)

    # 释放前 release_count 个
    to_release = held_sockets[:release_count]
    close_sockets(to_release)
    del held_sockets[:release_count]

    # 轮询等服务端检测到断开（TCP FIN 处理 + 可能的扫描），比固定 sleep 准
    log("  等待服务端完成连接回收（轮询，最多 8 秒）...")
    poll_until(lambda: check_server_alive(host, port), timeout=8.0, interval=0.5)

    # 尝试并发建立新连接
    stats = batch_connect(host, port, release_count, concurrency, "腾位")
    connected = stats.connected

    if connected >= release_count * 0.8:
        log(f"  [PASS] 释放 {release_count} 后新建 {connected} 个连接成功")
    else:
        log(f"  [WARN] 释放 {release_count} 但只成功新建 {connected} 个（{stats.summary()}）")

    # 清理新连接
    close_sockets(stats.sockets)
    log("")


def test_full_recovery(host: str, port: int, held_sockets: list[socket.socket]):
    """
    阶段 4: 全部释放后验证服务器恢复正常。
    """
    log("阶段 4: 全部释放后验证恢复")
    log("=" * 60)

    close_sockets(held_sockets)
    held_sockets.clear()

    # 轮询等恢复，最多 8 秒
    log("  等待服务端完成连接回收（轮询，最多 8 秒）...")
    recovered = poll_until(lambda: check_server_alive(host, port), timeout=8.0, interval=0.5)

    if recovered:
        ok, status = http_request_new(host, port)
        if ok and status == 200:
            log(f"  [PASS] 服务器恢复正常，HTTP GET / 返回 {status}")
        else:
            log(f"  [FAIL] 能连上但请求异常，ok={ok}, status={status}")
    else:
        log("  [FAIL] 服务器未在 8 秒内恢复接受新连接")

    log("")


# ==================== 主流程 ====================

def main():
    parser = argparse.ArgumentParser(description="Hical 连接数上限测试")
    parser.add_argument("--host", default="127.0.0.1", help="服务器地址 (默认 127.0.0.1)")
    parser.add_argument("--port", type=int, default=8080, help="服务器端口 (默认 8080)")
    parser.add_argument("--limit", type=int, default=5000,
                        help="服务端 setMaxConnections 设的上限值 (默认 5000)")
    parser.add_argument("--attempt", type=int, default=0,
                        help="尝试建立的连接数 (默认 limit * 1.2)")
    parser.add_argument("--concurrency", type=int, default=300,
                        help="并发建连度，同时在飞的 connect 数 (默认 300)")
    parser.add_argument("--release-count", type=int, default=0,
                        help="腾位测试释放的连接数 (默认 min(500, 持有数/4))")
    parser.add_argument("--skip-reclaim", action="store_true",
                        help="跳过腾位测试（阶段 3）")
    args = parser.parse_args()

    limit = args.limit
    attempt = args.attempt if args.attempt > 0 else int(limit * 1.2)
    host = args.host
    port = args.port
    concurrency = max(1, args.concurrency)

    # 误用提醒：本脚本是限流正确性测试，不是百万级容量评估。
    # 单机单端口建连受端口墙限制（~6 万），超过就引导去用对的工具。
    if attempt > kSingleHostConnLimit:
        log(f"[WARN] attempt={attempt} 超过单机单端口上限约 {kSingleHostConnLimit}（TCP 四元组端口墙）")
        log("       本脚本是【限流正确性测试】，测不了百万级容量，且会因持有大量 socket 撑爆客户端。")
        log("       容量评估请用 scripts/conn_mem_extrapolate.py（单机外推），")
        log("       真百万终验请走多机 + tcpkali。")
        log("")

    log("Hical 连接数上限测试")
    log(f"  目标: {host}:{port}")
    log(f"  服务端上限: {limit}")
    log(f"  尝试连接数: {attempt}")
    log(f"  并发建连度: {concurrency}")
    log("")

    # 先检查服务器是否在线
    if not check_server_alive(host, port):
        log("[FATAL] 无法连接服务器，请确认 bench_server 已启动")
        sys.exit(1)
    log("服务器在线，开始测试")
    log("")

    held: list[socket.socket] = []
    try:
        # 阶段 1: 填充到上限
        held = test_fill_to_limit(host, port, limit, attempt, concurrency)

        # 阶段 2: 已有连接正常服务（复用持有的连接，不新建）
        test_existing_connections_work(held)

        # 阶段 3: 腾位测试
        if not args.skip_reclaim and len(held) > 0:
            release_count = args.release_count if args.release_count > 0 else min(500, len(held) // 4)
            if release_count > 0:
                test_reclaim_slots(host, port, held, release_count, concurrency)

        # 阶段 4: 全部释放 + 恢复验证
        test_full_recovery(host, port, held)

        log("测试完成")
    finally:
        # 兜底：异常退出（含 Ctrl-C）时关掉所有还持有的连接，避免泄漏
        if held:
            log(f"清理残留的 {len(held)} 个连接")
            close_sockets(held)


if __name__ == "__main__":
    main()
