#!/usr/bin/env python3
"""有限原生 HTTP 测量：四种服务端配置交替运行，客户端面向独立 nghttpd。

身份文件示例（source_identity 来自实际源码快照，binary_sha256 必须匹配输入）：
{"variants": {"baseline": {"revision": "cab5e33...", "source_identity": "...",
 "binary_sha256": "...", "build_log": "..."}, "off": {...}, "on_h1": {...}, "h2": {...}},
 "client": {"source_identity": "...", "binary_sha256": "...", "build_log": "..."}}
身份文件是调用者提供的构建关联声明，脚本保留原文及哈希，不用 Git HEAD 代替未提交源码。
prepare-only 只读文件和生成计划，不启动程序、监听端口或访问网络。
"""

from __future__ import annotations

import argparse
from collections import Counter
from contextlib import contextmanager
import ctypes
from ctypes import wintypes
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import re
import socket
import ssl
import statistics
import subprocess
import sys
import threading
import time
import traceback
import uuid

from test_http2_interop import Runner, file_record, tool_argument, utc_now, wait_ready, write_json


VARIANTS = ("baseline", "off", "on_h1", "h2")
SCENARIOS = ("hello", "json-dom", "json-dto", "json-writer", "post", "file", "gzip", "mixed-slow")
REQUEST_COUNTS = re.compile(r"requests:\s*(\d+) total,\s*(\d+) started,\s*(\d+) done,\s*(\d+) succeeded,\s*(\d+) failed,\s*(\d+) errored,\s*(\d+) timeout")


def percentile(values: list[int], fraction: float) -> int:
    """最近秩百分位；保留原始样本；样本过少时不能据此判断分布是否稳定。"""
    if not values:
        raise ValueError("零个请求不能计算延迟")
    ordered = sorted(values)
    return ordered[max(0, math.ceil(len(ordered) * fraction) - 1)]


def request_log(path: Path, expected: int) -> dict:
    """解析 h2load 官方三列微秒日志，失败流、零请求和数量不符均失败。"""
    rows = []
    for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        columns = line.split()
        if len(columns) < 3:
            raise ValueError(f"请求日志第 {number} 行不足三列")
        start, status, elapsed = map(int, columns[:3])
        if start <= 0 or not 200 <= status < 300 or elapsed < 0:
            raise ValueError(f"请求日志含失败状态或时间：第 {number} 行")
        rows.append((start, status, elapsed))
    if not rows or len(rows) != expected:
        raise ValueError(f"实际请求 {len(rows)}，期望 {expected}")
    latencies = [row[2] for row in rows]
    span = max(start + elapsed for start, _, elapsed in rows) - min(start for start, _, _ in rows)
    # 同一时刻先移除结束事件，避免把首尾相接的两次请求算成并发。
    events = sorted([(start, 1) for start, _, elapsed in rows if elapsed > 0] +
                    [(start + elapsed, -1) for start, _, elapsed in rows if elapsed > 0])
    active = peak = 0
    for _, change in events:
        active += change
        peak = max(peak, active)
    return {"requests": len(rows), "p50_us": percentile(latencies, .5), "p99_us": percentile(latencies, .99),
            "request_span_us": span, "request_span_rps": len(rows) * 1e6 / span if span > 0 else None,
            "observed_peak_overlapping_request_spans": peak, "raw": file_record(path),
            "latency_definition": "h2load request_time 到 stream_close；request_span 吞吐排除首次 TLS 建连，完整进程吞吐另存"}


def check_h2load(record: dict, count: int, h2: bool) -> str:
    output = Path(record["stdout"]).read_text(encoding="utf-8", errors="replace")
    counts = REQUEST_COUNTS.search(output)
    protocol = "h2" if h2 else "http/1.1"
    if not counts or tuple(map(int, counts.groups())) != (count, count, count, count, 0, 0, 0):
        raise ValueError("h2load 汇总未证明所有请求成功")
    if f"Application protocol: {protocol}" not in output:
        raise ValueError(f"h2load 未实际协商 {protocol}")
    return output


class ProcessReader:
    """读取被本驱动启动的单一进程；Windows 用原生句柄，Linux 用 /proc。"""

    def __init__(self, pid: int):
        self.pid = pid
        self.handle = None
        if os.name == "nt":
            self.kernel = ctypes.WinDLL("kernel32", use_last_error=True)
            self.psapi = ctypes.WinDLL("psapi", use_last_error=True)
            self.kernel.OpenProcess.argtypes = (wintypes.DWORD, wintypes.BOOL, wintypes.DWORD)
            self.kernel.OpenProcess.restype = wintypes.HANDLE
            self.kernel.GetProcessTimes.argtypes = (wintypes.HANDLE, *([ctypes.POINTER(wintypes.FILETIME)] * 4))
            self.kernel.GetProcessTimes.restype = wintypes.BOOL
            self.kernel.CloseHandle.argtypes = (wintypes.HANDLE,)
            self.kernel.CloseHandle.restype = wintypes.BOOL

            class Memory(ctypes.Structure):
                _fields_ = [("cb", wintypes.DWORD), ("PageFaultCount", wintypes.DWORD),
                            *[(name, ctypes.c_size_t) for name in ("PeakWorkingSetSize", "WorkingSetSize", "QuotaPeakPagedPoolUsage", "QuotaPagedPoolUsage", "QuotaPeakNonPagedPoolUsage", "QuotaNonPagedPoolUsage", "PagefileUsage", "PeakPagefileUsage")]]

            self.Memory = Memory
            self.psapi.GetProcessMemoryInfo.argtypes = (wintypes.HANDLE, ctypes.POINTER(Memory), wintypes.DWORD)
            self.psapi.GetProcessMemoryInfo.restype = wintypes.BOOL
            # 仅查询和读取工作集；不请求写入、终止或调试其他进程的权限。
            self.handle = self.kernel.OpenProcess(0x0400 | 0x0010, False, pid)
            if not self.handle:
                raise ctypes.WinError(ctypes.get_last_error())
        elif not sys.platform.startswith("linux"):
            raise RuntimeError("RSS/CPU 采样目前只实现 Windows 和 Linux")

    def read(self) -> dict:
        if self.handle:
            memory = self.Memory()
            memory.cb = ctypes.sizeof(memory)
            times = [wintypes.FILETIME() for _ in range(4)]
            if not self.psapi.GetProcessMemoryInfo(self.handle, ctypes.byref(memory), memory.cb):
                raise ctypes.WinError(ctypes.get_last_error())
            if not self.kernel.GetProcessTimes(self.handle, *(ctypes.byref(item) for item in times)):
                raise ctypes.WinError(ctypes.get_last_error())
            cpu = sum((item.dwHighDateTime << 32) | item.dwLowDateTime for item in times[2:]) / 1e7
            rss = memory.WorkingSetSize
        else:
            root = Path("/proc") / str(self.pid)
            fields = root.joinpath("stat").read_text().rsplit(")", 1)[1].split()
            cpu = (int(fields[11]) + int(fields[12])) / os.sysconf("SC_CLK_TCK")
            rss = int(root.joinpath("statm").read_text().split()[1]) * os.sysconf("SC_PAGE_SIZE")
        return {"monotonic_seconds": time.monotonic(), "rss_bytes": rss, "cpu_seconds": cpu}

    def close(self):
        if self.handle:
            self.kernel.CloseHandle(self.handle)
            self.handle = None


class ProcessSamples:
    def __init__(self, process, interval: float):
        self.process = process
        self.interval = interval
        self.rows: list[dict] = []
        self.errors: list[str] = []
        self.done = threading.Event()
        self.thread = threading.Thread(target=self.collect, daemon=True)

    def collect(self):
        reader = None
        try:
            reader = ProcessReader(self.process.pid)
            while True:
                try:
                    self.rows.append(reader.read())
                except (OSError, ValueError) as error:
                    if self.process.poll() is None:
                        self.errors.append(str(error))
                    break
                if self.done.wait(self.interval):
                    if self.process.poll() is None:
                        self.rows.append(reader.read())
                    break
        except BaseException as error:
            self.errors.append(str(error))
        finally:
            if reader:
                reader.close()

    def start(self):
        self.thread.start()

    def finish(self, output: Path, connections: int, streams: int) -> dict:
        self.done.set()
        self.thread.join(timeout=3)
        if self.thread.is_alive():
            raise RuntimeError("进程采样线程没有退出")
        write_json(output, {"pid": self.process.pid, "interval_seconds": self.interval,
                            "samples": self.rows, "errors": self.errors})
        if self.errors or len(self.rows) < 2:
            return {"available": False, "raw": file_record(output), "reason": self.errors or "样本太短，未取得两个进程样本"}
        first, last = self.rows[0], self.rows[-1]
        elapsed = last["monotonic_seconds"] - first["monotonic_seconds"]
        peak = max(item["rss_bytes"] for item in self.rows)
        cpu = max(0, last["cpu_seconds"] - first["cpu_seconds"])
        return {"available": True, "raw": file_record(output), "first_rss_bytes": first["rss_bytes"],
                "peak_rss_bytes_sampled": peak, "peak_minus_first_rss_bytes": peak - first["rss_bytes"],
                "peak_rss_divided_by_configured_connections": peak / connections,
                "peak_rss_divided_by_configured_connection_stream_slots": peak / (connections * streams),
                "cpu_seconds_delta": cpu, "sample_span_seconds": elapsed,
                "cpu_percent_of_one_core": cpu * 100 / elapsed if elapsed > 0 else None,
                "note": "RSS 为整个进程工作集，包含固定成本和缓存；除以配置连接/流槽只是描述性商，不是边际每连接/流内存。CPU 为采样区间的进程用户态+内核态时间，不是整机占用。"}


def identities(args) -> dict:
    declared = json.loads(args.identity_manifest.read_text(encoding="utf-8"))
    observed = {}
    for label in VARIANTS:
        observed[label] = file_record(getattr(args, label + "_server"))
        identity = declared.get("variants", {}).get(label, {})
        if not identity.get("source_identity") or not identity.get("build_log"):
            raise ValueError(f"{label} 缺少源码身份或构建日志关联")
        if identity.get("binary_sha256") != observed[label].get("sha256"):
            raise ValueError(f"{label} 的二进制哈希不符，必须先重新留证实际构建")
        if label == "baseline" and not identity.get("revision", "").startswith("cab5e33"):
            raise ValueError("主基线必须显式关联 cab5e33")
    if args.client_driver:
        observed["client"] = file_record(args.client_driver)
        identity = declared.get("client", {})
        if not identity.get("source_identity") or not identity.get("build_log") or identity.get("binary_sha256") != observed["client"].get("sha256"):
            raise ValueError("客户端身份、构建日志或二进制哈希不符")
    return {"declarations": declared, "manifest": file_record(args.identity_manifest), "observed_binaries": observed,
            "tools": {name: file_record(getattr(args, name)) for name in ("h2load", "nghttpd") if getattr(args, name)},
            "notice": "声明的源码与构建关联由调用者提供；脚本核对实际二进制哈希并保留原文，不把当前 HEAD 当候选内容"}


@contextmanager
def server(args, runner: Runner, label: str, directory: Path, payload: Path):
    directory.mkdir(parents=True)
    ready, stop = directory / "ready.json", directory / "stop"
    h2 = label == "h2"
    child = runner.start("server-" + label, [str(getattr(args, label + "_server")), "--ready", str(ready),
        "--stop-file", str(stop), "--file", str(payload), "--cert", str(args.cert), "--key", str(args.key),
        "--mode", "h2" if h2 else "h1", "--tls", "on", "--slow-ms", str(args.slow_ms),
        "--lifetime-seconds", str(args.server_lifetime), "--gzip-body-bytes", str(args.gzip_bytes)], lifetime=args.server_lifetime + 10)
    try:
        data = wait_ready(child, lambda: json.loads(ready.read_text(encoding="utf-8")) if ready.is_file() else None,
                          min(args.timeout, 30))
        expected_compiled = label in ("on_h1", "h2")
        if not data.get("ready") or not data.get("io_callback_executed") or not data.get("tls") or data.get("mode") != ("h2" if h2 else "h1") or bool(data.get("compiled_http2")) != expected_compiled:
            raise ValueError("四配置的实际编译标识、启用模式或 owner loop 就绪证明不符")
        if data.get("file_sha256") != file_record(payload)["sha256"]:
            raise ValueError("服务端夹具哈希不符")
        gzip_body = payload.read_bytes()[:args.gzip_bytes]
        if data.get("gzip_body_bytes") != len(gzip_body) or data.get("gzip_body_sha256") != hashlib.sha256(gzip_body).hexdigest():
            raise ValueError("服务端 gzip 夹具长度或哈希不符")
        yield child, data
    finally:
        stop.write_bytes(b"stop\n")
        try:
            child[0].wait(timeout=5)
        except subprocess.TimeoutExpired:
            child[1]["graceful_stop_timed_out"] = True
        runner.stop(child)
        if child[1].get("graceful_stop_timed_out") or child[1].get("timed_out") or child[1]["exit_code"] != 0:
            raise RuntimeError("服务端正常收尾失败")


def load_command(args, port: int, shape: tuple[int, int], h2: bool, count: int, log: Path, scenario: str, payload: Path) -> list[str]:
    connections, streams = shape
    base = f"https://127.0.0.1:{port}"
    command = [str(args.h2load), "-n", str(count), "-c", str(connections), "-m", str(streams if h2 else 1),
               "-t", "1", "--log-file=" + tool_argument(log, args.h2load)]
    if not h2:
        command.append("--h1")
    if scenario == "post":
        command += ["-d", tool_argument(payload, args.h2load), "-H", "content-type: application/octet-stream", base + "/echo"]
    elif scenario == "mixed-slow":
        command += [base + "/hello"] * 9 + [base + "/slow"]
    else:
        route = {"file": "file.bin"}.get(scenario, scenario)
        command += ["-H", "accept-encoding: gzip" if scenario == "gzip" else "accept-encoding: identity", base + "/" + route]
    return command


def multiplex_proof(args, runner: Runner, port: int, shape: tuple[int, int], directory: Path, payload: Path) -> dict:
    connections, streams = shape
    count = connections * streams
    log = directory / "multiplex.tsv"
    command = load_command(args, port, shape, True, count, log, "hello", payload)
    command[-1] = f"https://127.0.0.1:{port}/slow"
    command.insert(1, "-v")
    record = runner.run("multiplex-proof", command)
    output = check_h2load(record, count, True)
    actual = Counter(map(int, re.findall(r"\[stream_id=(\d+)\]\s+:status:\s*200", output)))
    expected = Counter({2 * index + 1: connections for index in range(streams)})
    if actual != expected:
        raise ValueError(f"实际 H2 流 ID 不能证明配置的连接×流数：{actual} != {expected}")
    result = request_log(log, count)
    if streams > 1 and result["observed_peak_overlapping_request_spans"] <= connections:
        raise ValueError("慢路由请求日志未观察到超出连接数的重叠流")
    return {"configured_connections": connections, "streams_per_connection": streams,
            "response_stream_id_counts": dict(actual), "request_log": result,
            "note": "独立预检：h2load 每个连接固定分配 streams 个请求，奇数流 ID 的重复次数证明多个连接；慢请求时间重叠证明多流在途。预检 verbose 成本不计入正式样本。"}


def server_sample(args, runner: Runner, label: str, round_index: int, shape: tuple[int, int], scenario: str, payload: Path) -> dict:
    connections, requested_streams = shape
    streams = requested_streams if label == "h2" else 1
    directory = args.out / f"round-{round_index + 1}" / f"{label}-{connections}x{streams}-{scenario}"
    with server(args, runner, label, directory, payload) as (child, ready):
        proof = multiplex_proof(args, runner, ready["port"], (connections, streams), directory, payload) if label == "h2" else None
        warm_count = connections * max(value for count, value in args.shapes if count == connections)
        warm_log = directory / "warmup.tsv"
        warm_command = load_command(args, ready["port"], (connections, streams), label == "h2", warm_count,
                                    warm_log, "hello", payload)
        warm_command[-1] = f"https://127.0.0.1:{ready['port']}/slow"
        warm_record = runner.run("common-warmup", warm_command)
        check_h2load(warm_record, warm_count, label == "h2")
        warmup = request_log(warm_log, warm_count)
        samples = ProcessSamples(child[0], args.sample_interval)
        samples.start()
        started = time.monotonic()
        try:
            log = directory / "requests.tsv"
            record = runner.run("h2load-" + scenario, load_command(args, ready["port"], (connections, streams), label == "h2", args.requests, log, scenario, payload))
            output = check_h2load(record, args.requests, label == "h2")
            elapsed = time.monotonic() - started
        finally:
            process = samples.finish(directory / "process-samples.json", connections, streams)
        result = request_log(log, args.requests)
        reported = re.search(r"finished in [^,]+,\s*([\d.]+) req/s", output)
        result.update(variant=label, round=round_index + 1, scenario=scenario, connections=connections,
                      streams_per_connection=streams, process_wall_seconds=elapsed,
                      process_wall_rps=args.requests / elapsed, h2load_reported_rps=float(reported[1]) if reported else None,
                      process=process, multiplex_preflight=proof, server_ready=ready,
                      common_warmup=warmup,
                      queue_peak=None, queue_note="公开服务端探针未暴露内部队列统计，不以并发请求数代替")
        write_json(directory / "sample.json", result)
        return result


@contextmanager
def upstream(args, runner: Runner, payload: Path):
    with socket.socket() as reservation:
        reservation.bind(("127.0.0.1", 0))
        port = reservation.getsockname()[1]
    child = runner.start("nghttpd", [str(args.nghttpd), "-v", "-a", "127.0.0.1", "--echo-upload",
        "-d", tool_argument(payload.parent, args.nghttpd), str(port), tool_argument(args.key, args.nghttpd),
        tool_argument(args.cert, args.nghttpd)], lifetime=args.server_lifetime)
    context = ssl.create_default_context(cafile=str(args.ca))
    context.set_alpn_protocols(["h2"])
    def ready():
        with socket.create_connection(("127.0.0.1", port), timeout=.5) as raw:
            with context.wrap_socket(raw, server_hostname="127.0.0.1") as tls:
                if tls.selected_alpn_protocol() != "h2":
                    raise ValueError("独立上游未实际协商 h2")
                return True
    try:
        wait_ready(child, ready, min(20, args.timeout))
        yield child, port
    finally:
        runner.stop(child)


def client_sample(args, runner: Runner, identity: dict, round_index: int, shape: tuple[int, int], driver: str, payload: Path, method: str) -> dict:
    connections, streams = shape
    directory = args.out / f"client-round-{round_index + 1}" / f"{driver}-{connections}x{streams}-{method}"
    directory.mkdir(parents=True)
    report_path = directory / "client.json"
    run_id = uuid.uuid4().hex
    with upstream(args, runner, payload) as (upstream_child, port):
        command = [str(args.client_driver), "--host", "127.0.0.1", "--port", str(port), "--target", "/" + payload.name,
            "--driver", driver, "--mode", "h2", "--run-id", run_id, "--source-identity", identity["declarations"]["client"]["source_identity"],
            "--expected", str(payload), "--report", str(report_path), "--method", method, "--ca", str(args.ca),
            "--requests", str(args.requests), "--concurrency", str(min(256, connections * streams + args.client_queue)),
            "--connections", str(connections), "--streams", str(streams), "--timeout-ms", str(int(args.timeout * 1000))]
        if method == "POST":
            command += ["--body", str(payload)]
        child = runner.start("client-" + driver, command)
        sampler = ProcessSamples(child[0], args.sample_interval)
        sampler.start()
        try:
            child[0].wait(timeout=args.timeout + 2)
        finally:
            process = sampler.finish(directory / "process-samples.json", connections, streams)
            runner.stop(child)
        if child[1].get("timed_out") or child[1]["exit_code"] != 0:
            raise ValueError("客户端测量失败或超时")
        report = json.loads(report_path.read_text(encoding="utf-8"))
        if not report.get("ok") or report.get("request_count") != args.requests or report.get("run_id") != run_id or report.get("source_identity") != identity["declarations"]["client"]["source_identity"] or len(report.get("requests", [])) != args.requests or not all(item.get("ok") and item.get("protocol") == "h2" and item.get("body_matches") for item in report["requests"]):
            raise ValueError("客户端请求数、身份、协议或完整响应校验失败")
    upstream_log = Path(upstream_child[1]["stdout"]).read_text(encoding="utf-8", errors="replace")
    pairs = {(int(session), int(stream)) for session, stream in re.findall(r"\[id=(\d+)\][^\n]*recv \(stream_id=(\d+)[^\n]*x-interop-run:\s*" + re.escape(run_id), upstream_log)}
    sessions = {session for session, _ in pairs}
    if len(pairs) != args.requests or not sessions or len(sessions) > connections:
        raise ValueError("nghttpd 日志未证明真实请求和连接预算")
    if streams > 1 and args.requests > connections and not any(sum(owner == session for owner, _ in pairs) > 1 for session in sessions):
        raise ValueError("独立上游未看到同一连接上的多条 H2 流")
    latencies = [item["latency_us"] for item in report["requests"]]
    result = {"driver": driver, "method": method, "round": round_index + 1, "connections": connections,
              "streams_per_connection": streams, "observed_connections": len(sessions), "requests": args.requests,
              "p50_us": percentile(latencies, .5), "p99_us": percentile(latencies, .99),
              "requests_per_second": report["requests_per_second"], "process": process,
              "peak_queued_requests": report["peak_queued_requests"] if driver == "pool" else None,
              "upstream_proof": {"run_id": run_id, "session_stream_pairs": sorted(pairs), "raw": file_record(Path(upstream_child[1]["stdout"]))},
              "raw_client": file_record(report_path)}
    write_json(directory / "sample.json", result)
    return result


def summary(samples: list[dict], value_key: str, grouping: tuple[str, ...]) -> list[dict]:
    groups = {}
    for sample in samples:
        groups.setdefault(tuple(sample[key] for key in grouping), []).append(sample)
    result = []
    for identity, rows in groups.items():
        item = dict(zip(grouping, identity))
        item.update(sample_count=len(rows), median_rps=statistics.median(row[value_key] for row in rows),
                    median_p50_us=statistics.median(row["p50_us"] for row in rows),
                    median_p99_us=statistics.median(row["p99_us"] for row in rows))
        result.append(item)
    return result


def arguments():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    for name in ("baseline-server", "off-server", "on-h1-server", "h2-server", "identity-manifest", "h2load", "cert", "key", "ca", "out"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--client-driver", type=Path)
    parser.add_argument("--nghttpd", type=Path)
    parser.add_argument("--rounds", type=int, default=3)
    parser.add_argument("--requests", type=int, default=1000)
    parser.add_argument("--shape", action="append", help="连接数x每连接流数，可重复；默认 1x1、1x8、4x8")
    parser.add_argument("--scenario", action="append", choices=SCENARIOS)
    parser.add_argument("--variant", action="append", choices=VARIANTS, help="仅筛选服务端变体；默认全部")
    parser.add_argument("--gzip-bytes", type=int, default=32 * 1024, help="gzip 原始正文，默认32KiB整体压缩；文件和POST仍为256KiB")
    parser.add_argument("--section", action="append", choices=("server", "client"))
    parser.add_argument("--timeout", type=float, default=60)
    parser.add_argument("--server-lifetime", type=int, default=180)
    parser.add_argument("--slow-ms", type=int, default=50)
    parser.add_argument("--client-queue", type=int, default=8, help="在流槽数之上增加的 worker 数；池统计记录实际排队")
    parser.add_argument("--sample-interval", type=float, default=.01)
    parser.add_argument("--prepare-only", action="store_true")
    args = parser.parse_args()
    args.section = args.section or ["server", "client"]
    args.scenario = args.scenario or list(SCENARIOS)
    args.variant = args.variant or list(VARIANTS)
    args.shapes = []
    for value in args.shape or ("1x1", "1x8", "4x8"):
        if not re.fullmatch(r"[1-9]\d*x[1-9]\d*", value):
            parser.error("shape 必须形如 1x8")
        shape = tuple(map(int, value.split("x")))
        if shape[0] > 64 or shape[1] > 64 or shape[0] * shape[1] > 256:
            parser.error("连接最多 64、流最多 64，总并发槽最多 256")
        if shape not in args.shapes:
            args.shapes.append(shape)
    if not 1 <= args.rounds <= 100 or not 10 <= args.requests <= 1000000 or not .001 <= args.sample_interval <= 1 or not 1 <= args.timeout <= 600 or not args.timeout + 10 < args.server_lifetime <= 1800 or not 1 <= args.slow_ms <= 1000 or not 0 <= args.client_queue <= 256 or not 1024 <= args.gzip_bytes <= 262144:
        parser.error("轮数、请求数、采样间隔、期限或队列参数越界；服务寿命须大于单样本期限+10秒")
    if args.requests < max(connections * streams for connections, streams in args.shapes):
        parser.error("请求数不能少于最大连接×流数")
    if "client" in args.section and (not args.client_driver or not args.nghttpd):
        parser.error("客户端测量需要 --client-driver 和 --nghttpd")
    for name, value in vars(args).copy().items():
        if isinstance(value, Path):
            setattr(args, name, value.resolve())
    return args


def main() -> int:
    args = arguments()
    args.out.mkdir(parents=True, exist_ok=False)
    runner = Runner(args.out, args.timeout)
    result = {"schema": 1, "started_utc": utc_now(), "platform": platform.platform(), "ok": False,
              "prepare_only": args.prepare_only, "rounds": args.rounds, "requests_per_sample": args.requests,
              "shapes": args.shapes, "scenarios": args.scenario, "server_samples": [], "client_samples": [],
              "server_variants": args.variant,
              "gzip_fixture": {"uncompressed_bytes": args.gzip_bytes,
                               "size_class": "below_64KiB" if args.gzip_bytes < 65536 else "at_least_64KiB",
                               "encoding_note": "探针使用同步中间件：H1 大正文使用 chunked 封装；H2 发送原始 gzip 字节，大小标签不表示流式压缩。"},
              "measurement_notice": "本机回环短样本；不预设吞吐收益、不使用 CI 性能阈值。外部 h2load 不校验证书链，生产客户端和 nghttpd 就绪探测使用指定 CA 与主机身份校验。"}
    try:
        result["identity"] = identities(args)
        for path in (args.cert, args.key, args.ca):
            if not path.is_file():
                raise ValueError("证书、私钥和 CA 必须预先生成")
        fixture = args.out / "fixtures" / "payload.bin"
        fixture.parent.mkdir()
        fixture.write_bytes((b"Hical HTTP2 performance fixture.\n" * 8192)[:262144])
        result["fixture"] = file_record(fixture)
        result["fixture"]["notice"] = "可压缩的固定 256 KiB 夹具；gzip 测量必须结合这一内容分布解释"
        if args.prepare_only:
            result["ok"] = True
            return 0
        for round_index in range(args.rounds):
            variants = tuple(value for value in VARIANTS if value in args.variant)
            order = variants if round_index % 2 == 0 else tuple(reversed(variants))
            if "server" in args.section:
                for scenario in args.scenario:
                    for connections, streams in args.shapes:
                        for label in order:
                            # H1 没有流复用，同轮同场景同连接数只测一次。
                            if label != "h2" and (connections, streams) != next(shape for shape in args.shapes if shape[0] == connections):
                                continue
                            sample = server_sample(args, runner, label, round_index, (connections, streams), scenario, fixture)
                            result["server_samples"].append(sample)
                            write_json(args.out / "result.json", result)
            if "client" in args.section:
                for shape in args.shapes:
                    for method in ("GET", "POST"):
                        for driver in (("shared", "pool") if round_index % 2 == 0 else ("pool", "shared")):
                            result["client_samples"].append(client_sample(args, runner, result["identity"], round_index, shape, driver, fixture, method))
                            write_json(args.out / "result.json", result)
            print(f"完成测量轮次 {round_index + 1}/{args.rounds}", flush=True)
        if not result["server_samples"] and not result["client_samples"]:
            raise ValueError("没有执行任何测量")
        result["server_summary"] = summary(result["server_samples"], "process_wall_rps", ("variant", "scenario", "connections", "streams_per_connection"))
        result["client_summary"] = summary(result["client_samples"], "requests_per_second", ("driver", "method", "connections", "streams_per_connection"))
        result["ok"] = True
        return 0
    except BaseException as error:
        result["error"] = str(error)
        result["traceback"] = traceback.format_exc()
        print(str(error), file=sys.stderr)
        return 1
    finally:
        result["cleanup_errors"] = runner.close()
        if result["cleanup_errors"]:
            result["ok"] = False
        result["commands"] = runner.commands
        result["ended_utc"] = utc_now()
        write_json(args.out / "result.json", result)
        if result["cleanup_errors"]:
            return 1


if __name__ == "__main__":
    raise SystemExit(main())
