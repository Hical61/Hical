#!/usr/bin/env python3
"""交替执行同一组旧/新 Google Benchmark 二进制，保留原始 JSON 并比较中位数。"""

import argparse
import hashlib
import json
import pathlib
import platform
import statistics
import subprocess
import sys

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8")


def run_benchmark(binary, destination, benchmark_filter, min_time):
    """运行一份基准并读取每个用例的墙钟时间（ns）。"""
    command = [
        str(binary.resolve()),
        f"--benchmark_filter={benchmark_filter}",
        f"--benchmark_min_time={min_time}s",
        "--benchmark_repetitions=1",
        f"--benchmark_out={destination.resolve()}",
        "--benchmark_out_format=json",
    ]
    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode:
        raise RuntimeError(f"基准失败：{binary}\n{result.stdout}\n{result.stderr}")
    report = json.loads(destination.read_text(encoding="utf-8"))
    measurements = {}
    for item in report["benchmarks"]:
        if item.get("error_occurred"):
            raise RuntimeError(f"用例失败：{item}")
        if item.get("run_type", "iteration") != "iteration":
            continue
        scale = {"ns": 1, "us": 1000, "ms": 1_000_000, "s": 1_000_000_000}
        measurements[item["name"]] = item["real_time"] * scale[item["time_unit"]]
    if not measurements:
        raise RuntimeError(f"基准没有执行任何用例：{binary}")
    return measurements


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("before", type=pathlib.Path)
    parser.add_argument("after", type=pathlib.Path)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    parser.add_argument("--rounds", type=int, default=5)
    parser.add_argument("--filter", default=".")
    parser.add_argument("--min-time", type=float, default=0.1)
    args = parser.parse_args()
    if args.rounds < 1 or args.min_time <= 0:
        parser.error("轮数和每个用例的最短时间必须大于零")
    args.output.mkdir(parents=True, exist_ok=True)
    metadata = {"platform": platform.platform(), "rounds": args.rounds, "min_time": args.min_time}
    for label in ("before", "after"):
        binary = getattr(args, label)
        metadata[label] = {
            "path": str(binary.resolve()),
            "sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
        }
    if metadata["before"]["sha256"] == metadata["after"]["sha256"]:
        raise RuntimeError("旧/新二进制完全相同，不能用作优化对比")
    (args.output / "metadata.json").write_text(
        json.dumps(metadata, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    samples = {"before": {}, "after": {}}
    for round_index in range(args.rounds):
        order = ("before", "after") if round_index % 2 == 0 else ("after", "before")
        for label in order:
            result = run_benchmark(
                getattr(args, label),
                args.output / f"{label}-{round_index + 1}.json",
                args.filter,
                args.min_time,
            )
            for name, value in result.items():
                samples[label].setdefault(name, []).append(value)
        print(f"完成 {round_index + 1}/{args.rounds} 轮", flush=True)
    if samples["before"].keys() != samples["after"].keys():
        raise RuntimeError("旧/新二进制执行的用例集合不一致")
    summary = []
    for name, before_samples in samples["before"].items():
        after_samples = samples["after"][name]
        if len(before_samples) != args.rounds or len(after_samples) != args.rounds:
            raise RuntimeError(f"用例样本数量异常：{name}")
        before = statistics.median(before_samples)
        after = statistics.median(after_samples)
        summary.append({
            "name": name,
            "before_ns": before,
            "after_ns": after,
            "reduction_percent": (before - after) / before * 100,
            "before_samples_ns": before_samples,
            "after_samples_ns": after_samples,
        })
        print(f"{name}: {before:.2f} -> {after:.2f} ns，耗时减少 {(before - after) / before:.1%}")
    (args.output / "summary.json").write_text(
        json.dumps(summary, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )


if __name__ == "__main__":
    main()
