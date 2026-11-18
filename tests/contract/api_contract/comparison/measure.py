#!/usr/bin/env python3
"""在独立的 Linux 环境里轮流压测各服务的房间列表，并保存原始数据。"""

import argparse
import itertools
import json
import os
from pathlib import Path
import re
import statistics
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[4] / "scripts"))
from check_api_contract import exercise, request, require, serve

STATUS_SCRIPT = Path(__file__).with_name("verify_status.lua").resolve()


def proc_stats(pid):
    base = Path("/proc") / str(pid)
    # comm 里可能有空格。先跳过最后一个右括号，再从 stat 的第 3 列读起。
    fields = (base / "stat").read_text().rsplit(")", 1)[1].split()
    status = (base / "status").read_text()
    return {
        "cpu_seconds": (int(fields[11]) + int(fields[12])) / os.sysconf("SC_CLK_TCK"),
        "rss_kib": int(re.search(r"(?m)^VmRSS:\s+(\d+) kB$", status)[1]),
        "peak_rss_kib": int(re.search(r"(?m)^VmHWM:\s+(\d+) kB$", status)[1]),
    }


def load(wrk, cpu, base, seconds, log):
    command = ["taskset", "-c", str(cpu), str(wrk), "-t1", "-c8",
               f"-d{seconds}s", "--latency", "-s", str(STATUS_SCRIPT), base + "/rooms"]
    completed = subprocess.run(command, check=False, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, timeout=seconds + 15,
                               text=True, encoding="utf-8",
                               env={**os.environ, "LC_ALL": "C"})
    log.write_text(completed.stdout, encoding="utf-8")
    require(completed.returncode == 0, f"wrk 退出码为 {completed.returncode}，请查看 {log}")
    require("Socket errors:" not in completed.stdout and "Non-2xx" not in completed.stdout,
            f"压测时出现网络或 HTTP 错误，请查看 {log}")
    count = re.search(r"(?m)^\s*(\d+) requests in", completed.stdout)
    p99 = re.search(r"(?m)^\s*99%\s+([0-9.]+)(us|ms|s)\s*$", completed.stdout)
    statuses = re.search(r"(?m)^HTTP_200=(\d+) HTTP_OTHER=(\d+)\s*$", completed.stdout)
    require(count and p99 and statuses and int(count[1]) > 0, f"压测统计不完整，请查看 {log}")
    require(int(statuses[2]) == 0 and int(statuses[1]) == int(count[1]),
            f"返回 HTTP 200 的响应数与已完成请求数不一致，请查看 {log}")
    scale = {"us": 1, "ms": 1000, "s": 1000000}
    return int(count[1]), float(p99[1]) * scale[p99[2]]


def measure_one(args, binary, directory):
    with serve(["taskset", "-c", str(args.server_cpu), str(binary), "0"], directory) as (base, _, _, process):
        rooms = exercise(base, request)
        expected = (200, {"rooms": rooms, "total": len(rooms)})
        load(args.wrk, args.client_cpu, base, 2, directory / "warmup.log")
        before = proc_stats(process.pid)
        requests, p99_us = load(args.wrk, args.client_cpu, base, args.seconds, directory / "wrk.log")
        after = proc_stats(process.pid)
        require(request(base, "GET", "/rooms") == expected, "压测后返回的房间列表变了")
        return {
            "requests": requests,
            "p99_us": p99_us,
            "cpu_us_per_request": (after["cpu_seconds"] - before["cpu_seconds"]) * 1e6 / requests,
            "rss_kib": after["rss_kib"],
            "peak_rss_kib": after["peak_rss_kib"],
        }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--wrk", type=Path, required=True)
    parser.add_argument("--server", nargs=2, metavar=("NAME", "BINARY"), action="append", required=True)
    parser.add_argument("--server-cpu", type=int, default=0)
    parser.add_argument("--client-cpu", type=int, default=1)
    parser.add_argument("--seconds", type=int, default=6)
    parser.add_argument("--rounds", type=int, default=5)
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()
    args.wrk = args.wrk.resolve()
    require(args.rounds >= 3 and args.seconds >= 2, "请至少跑 3 轮，每轮至少 2 秒")
    require(args.server_cpu != args.client_cpu
            and {args.server_cpu, args.client_cpu} <= os.sched_getaffinity(0),
            "请为服务和压测客户端指定不同的可用 CPU")
    names = [name for name, _ in args.server]
    require(len(names) == len(set(names)) and all(re.fullmatch(r"[a-zA-Z0-9_-]+", n) for n in names),
            "服务名不能重复，只能用字母、数字、下划线和连字符")
    binaries = {name: Path(path).resolve(strict=True) for name, path in args.server}
    output = args.output_dir.resolve()
    output.mkdir()
    rows = {name: [] for name in names}
    orders = []
    for round_no in range(args.rounds):
        order = list(itertools.islice(itertools.cycle(names), round_no % len(names), round_no % len(names) + len(names)))
        orders.append(order)
        for name in order:
            result = measure_one(args, binaries[name], output / f"{round_no + 1:02}-{name}")
            rows[name].append(result)
            print(f"{round_no + 1} {name}: {result}", flush=True)
    medians = {name: {key: statistics.median(row[key] for row in samples) for key in samples[0]}
               for name, samples in rows.items()}
    report = {"seconds": args.seconds, "cpus": [args.server_cpu, args.client_cpu],
              "round_order": orders, "samples": rows, "medians": medians}
    (output / "summary.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
