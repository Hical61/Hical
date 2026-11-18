#!/usr/bin/env python3
"""核对 CTest 找到的用例，确认每项都实际跑过并通过。"""

import json
from pathlib import Path
import re
import sys
import xml.etree.ElementTree as ET

from check_json_contract import ctestOutput, ZERO_TESTS


def validate(discovery: Path, junit: Path, suites: list[str]) -> int:
    """检查预期套件和 JUnit 日志，返回通过的用例数。"""
    names = [test["name"] for test in json.loads(discovery.read_text(encoding="utf-8"))["tests"]]
    if not names or len(names) != len(set(names)):
        raise ValueError("CTest 没有找到测试，或找到了重名的测试")
    found = {name.split(".", 1)[0] for name in names}
    if found != set(suites):
        raise ValueError(f"CTest 应找到套件 {sorted(suites)}，实际找到 {sorted(found)}")

    root = ET.parse(junit).getroot()
    for suite in root.iter("testsuite"):
        if any(int(suite.get(key, "0")) for key in ("failures", "errors", "disabled", "skipped", "notrun")):
            raise ValueError("JUnit 报告里有测试失败、出错或没运行")
    cases = list(root.iter("testcase"))
    if sorted(case.get("name", "") for case in cases) != sorted(names):
        raise ValueError("JUnit 报告里的测试和 CTest 找到的测试对不上")
    for case in cases:
        name = case.get("name")
        if case.get("status") != "run" or any(
            case.find(tag) is not None for tag in ("failure", "error", "skipped", "disabled", "notrun")
        ):
            raise ValueError(f"JUnit 没有记录用例 {name!r} 成功运行")
        output = "\n".join(ctestOutput(node) for node in case.findall("system-out"))
        markers = set(re.findall(r"^\[ *(RUN|OK) *\] +(\S+)", output, re.MULTILINE))
        if ZERO_TESTS.search(output) or not {("RUN", name), ("OK", name)} <= markers:
            raise ValueError(f"用例 {name!r} 缺少 GoogleTest 的运行或通过标记")
    return len(cases)


if __name__ == "__main__":
    # Windows 控制台默认不是 UTF-8，中文提示会变成乱码。
    sys.stdout.reconfigure(encoding="utf-8", errors="backslashreplace")
    sys.stderr.reconfigure(encoding="utf-8", errors="backslashreplace")
    try:
        count = validate(Path(sys.argv[1]), Path(sys.argv[2]), sys.argv[3:])
    except (OSError, ValueError, ET.ParseError, KeyError) as error:
        sys.exit(f"无法确认契约回归通过。{error}")
    print(f"已确认 {count} 项契约回归用例都实际运行并通过。")
