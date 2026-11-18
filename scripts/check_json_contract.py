#!/usr/bin/env python3
"""比较宏轨和原生轨的 JSON 结果，并检查两边的 CTest JUnit 报告。"""

from __future__ import annotations

import argparse
import json
import math
from pathlib import Path
import re
import sys
import xml.etree.ElementTree as ET


CASE_IDS = (
    "valid_room", "missing_name", "empty_name", "missing_capacity", "below_minimum",
    "above_maximum", "signed_overflow", "unsigned_overflow", "double_token", "null_name",
    "unknown_key", "ignored_field", "nested_room", "nonempty_room_list", "output_outside_input_range",
)
SCHEMA_KEYS = ("input", "output", "nested_input", "nested_output", "list_output")
BUILD_KEYS = ("expected_reflection", "hical_has_reflection", "cpp_impl_reflection", "cpp_lib_reflection")
JSON_TYPES = {dict: "对象", list: "数组", str: "字符串", bool: "布尔值", int: "整数", float: "浮点数", type(None): "null"}
CONTRACT_SUITE = "JsonContractTest"
EXPORT_TEST = CONTRACT_SUITE + ".ExportsGoldenCases"
RUN_MARKER = re.compile(r"^\s*\[\s*RUN\s*\]\s+(\S+)", re.MULTILINE)
ZERO_TESTS = re.compile(r"Running\s+0\s+tests?\b")
ANSI_COLOR = re.compile(r"\x1b\[[0-9;]*m")


class CheckError(Exception):
    """记录结果文件或测试报告的问题，供命令行直接显示。"""


def uniqueObject(pairs):
    """把键值对转成对象，遇到重复键就报错。"""
    result = {}
    for key, value in pairs:
        if key in result:
            raise CheckError(f"存在重复 JSON 键 {key!r}")
        result[key] = value
    return result


def finiteFloat(token: str) -> float:
    """解析 JSON 小数，转成浮点数后如果溢出就报错。"""
    value = float(token)
    if not math.isfinite(value):
        raise CheckError("JSON 数值超出有限浮点范围")
    return value


def rejectConstant(token: str):
    """拒绝 JSON 不支持的 NaN 和 Infinity。"""
    raise CheckError(f"JSON 不接受非标准数值 {token}")


def readJson(path: Path, label: str):
    """读入 UTF-8 JSON，出错时带上文件名和所属模式。"""
    try:
        return json.loads(path.read_text(encoding="utf-8-sig"), object_pairs_hook=uniqueObject,
                          parse_float=finiteFloat, parse_constant=rejectConstant)
    except (CheckError, OSError, ValueError, RecursionError) as error:
        raise CheckError(f"{label} JSON 文件读取失败（{path}）：{error}") from None


def requireKeys(value, required, label: str, optional=()) -> None:
    """检查对象有没有缺字段或多字段。"""
    if type(value) is not dict:
        raise CheckError(f"{label} 必须是 JSON 对象")
    missing = set(required) - value.keys()
    extra = value.keys() - set(required) - set(optional)
    if missing:
        raise CheckError(f"{label} 缺少字段：{', '.join(sorted(missing))}")
    if extra:
        raise CheckError(f"{label} 有未声明字段：{', '.join(sorted(extra))}")


def validateDocument(document, mode: str):
    """检查一个模式的结果和构建信息，返回按 ID 查找的用例。"""
    requireKeys(document, ("mode", "build", "cases", "schemas"), mode)
    if document["mode"] != mode:
        raise CheckError(f"{mode}.mode 必须为 {mode!r}")
    build = document["build"]
    requireKeys(build, BUILD_KEYS, f"{mode}.build", ("compiler", "cpp_standard"))
    for key in BUILD_KEYS:
        if type(build[key]) is not int or build[key] < 0:
            raise CheckError(f"{mode}.build.{key} 必须是非负整数，不能用布尔值代替")
    expectedFlag = 1 if mode == "native" else 0
    for key in BUILD_KEYS[:2]:
        if build[key] != expectedFlag:
            raise CheckError(f"{mode}.build.{key} 应为 {expectedFlag}，实际值不符")
    for key in BUILD_KEYS[2:]:
        if (mode == "native" and build[key] < 202506) or (mode == "macro" and build[key] != 0):
            requirement = "不低于 202506" if mode == "native" else "为 0"
            raise CheckError(f"{mode}.build.{key} 必须{requirement}")
    for key in ("compiler", "cpp_standard"):
        if key in build and type(build[key]) not in (str, int, float):
            raise CheckError(f"{mode}.build.{key} 只能记录文本或数字")

    cases = document["cases"]
    if type(cases) is not list or not cases:
        raise CheckError(f"{mode}.cases 必须是非空对象数组")
    byId = {}
    for index, case in enumerate(cases):
        label = f"{mode}.cases[{index}]"
        requireKeys(case, ("id", "accepted", "result"), label)
        if type(case["id"]) is not str or not case["id"]:
            raise CheckError(f"{label}.id 必须是非空字符串")
        caseId = case["id"]
        if caseId in byId:
            raise CheckError(f"{mode}.cases 存在重复 ID：{caseId}")
        if type(case["accepted"]) is not bool:
            raise CheckError(f"{label}.accepted 必须是布尔值")
        if case["accepted"]:
            if type(case["result"]) not in (dict, list):
                raise CheckError(f"{label} 接受输入时，result 要保存实际 JSON 对象或数组")
        elif type(case["result"]) is not str or not case["result"].strip():
            raise CheckError(f"{label} 拒绝输入时，result 要保存非空分类字符串")
        byId[caseId] = case
    missing = set(CASE_IDS) - byId.keys()
    extra = byId.keys() - set(CASE_IDS)
    if missing or extra:
        raise CheckError(f"{mode}.cases 和固定用例表对不上，缺少 {sorted(missing)}，未声明 {sorted(extra)}")
    requireKeys(document["schemas"], SCHEMA_KEYS, f"{mode}.schemas")
    for key in SCHEMA_KEYS:
        if type(document["schemas"][key]) is not dict:
            raise CheckError(f"{mode}.schemas.{key} 必须是 Schema 对象")
    return byId


def compareJson(macro, native, path: str, schema: bool = False) -> None:
    """逐项比较两轨 JSON。保留数值类型和数组顺序，只给 Schema 的 required 字符串数组排序。"""
    if type(macro) is not type(native):
        raise CheckError(f"{path} 的 JSON 类型不一致（宏轨{JSON_TYPES[type(macro)]}，原生{JSON_TYPES[type(native)]}）")
    if type(macro) is dict:
        if macro.keys() != native.keys():
            raise CheckError(f"{path} 的对象字段不一致（宏轨独有 {sorted(macro.keys() - native.keys())}，"
                             f"原生独有 {sorted(native.keys() - macro.keys())}）")
        for key in sorted(macro):
            left, right = macro[key], native[key]
            if schema and key == "required" and type(left) is list and type(right) is list:
                if all(type(item) is str for item in left + right):
                    left, right = sorted(left), sorted(right)
            compareJson(left, right, f"{path}.{key}", schema)
    elif type(macro) is list:
        if len(macro) != len(native):
            raise CheckError(f"{path} 的数组长度不一致（宏轨 {len(macro)}，原生 {len(native)}）")
        for index, (left, right) in enumerate(zip(macro, native)):
            compareJson(left, right, f"{path}[{index}]", schema)
    elif macro != native:
        raise CheckError(f"{path} 的值不一致")


def xmlTag(node) -> str:
    """返回去掉命名空间的 XML 标签名。"""
    return node.tag.rsplit("}", 1)[-1]


def junitCount(node, attribute: str, label: str) -> int:
    """读取 JUnit 的计数。计数不是非负整数时，指出出错的属性。"""
    text = node.get(attribute, "0")
    try:
        if not re.fullmatch(r"[0-9]+", text):
            raise ValueError
        return int(text)
    except ValueError:
        raise CheckError(f"{label} 的 JUnit {attribute} 不是合法非负整数") from None


def ctestOutput(node) -> str:
    """从 system-out 取出日志，还原 CTest 转义的 ESC 后再去掉颜色。"""
    output = "".join(node.itertext()).replace("[NON-XML-CHAR-0x1B]", "\x1b")
    return ANSI_COLOR.sub("", output)


def validateJunit(path: Path, label: str) -> int:
    """检查 JUnit 计数、状态和 GoogleTest 日志，返回实际跑过的契约用例数。"""
    try:
        root = ET.parse(path).getroot()
    except (OSError, ET.ParseError, ValueError, LookupError) as error:
        raise CheckError(f"{label} JUnit 文件读取失败（{path}）：{error}") from None
    if xmlTag(root) not in ("testsuite", "testsuites"):
        raise CheckError(f"{label} JUnit 根元素必须是 testsuite 或 testsuites")
    nodes = list(root.iter())
    cases = [node for node in nodes if xmlTag(node) == "testcase"]
    if not cases:
        raise CheckError(f"{label} JUnit 没有实际测试用例")
    for node in nodes:
        tag = xmlTag(node)
        if tag in ("failure", "error", "skipped", "disabled", "notrun"):
            raise CheckError(f"{label} JUnit 里出现了 {tag}，有测试没通过")
        if tag in ("testsuite", "testsuites"):
            for attribute in ("failures", "errors", "skipped", "disabled", "notrun"):
                if junitCount(node, attribute, label):
                    raise CheckError(f"{label} JUnit 的 {attribute} 非零，有测试没通过")
            actual = sum(xmlTag(child) == "testcase" for child in node.iter())
            if "tests" in node.attrib and junitCount(node, "tests", label) != actual:
                raise CheckError(f"{label} JUnit tests 计数与实际 testcase 数量不符")
        if tag == "system-out" and ZERO_TESTS.search(ctestOutput(node)):
            raise CheckError(f"{label} JUnit 日志写了 Running 0 test，GoogleTest 实际空跑")

    contractCases = set()
    for case in cases:
        name = case.get("name", "").strip()
        if not name:
            raise CheckError(f"{label} JUnit testcase 缺少名称")
        for attribute, allowed in (("status", ("run", "passed", "completed", "success")),
                                   ("result", ("completed", "passed", "success"))):
            if attribute in case.attrib and case.get(attribute).strip().lower() not in allowed:
                raise CheckError(f"{label} JUnit 用例 {name!r} 未成功执行，请看 {attribute} 的值")
        for attribute in ("disabled", "skipped", "notrun"):
            if attribute in case.attrib and junitCount(case, attribute, label):
                raise CheckError(f"{label} JUnit 用例 {name!r} 的 {attribute} 非零")
        fullName = name if name.startswith(CONTRACT_SUITE + ".") else ""
        if not fullName and case.get("classname") == CONTRACT_SUITE:
            fullName = CONTRACT_SUITE + "." + name
        if fullName:
            if fullName in contractCases:
                raise CheckError(f"{label} JUnit 存在重复用例 {fullName!r}")
            output = "\n".join(ctestOutput(node) for node in case.iter() if xmlTag(node) == "system-out")
            if fullName not in RUN_MARKER.findall(output):
                raise CheckError(f"{label} JUnit 没找到用例 {fullName!r} 对应 [ RUN ] 标记")
            contractCases.add(fullName)
    if EXPORT_TEST not in contractCases:
        raise CheckError(f"{label} JUnit 找不到已运行的导出用例 {EXPORT_TEST}")
    return len(contractCases)


def main(argv=None) -> int:
    """检查传入的四个文件，通过时返回 0，有问题时返回 1。"""
    parser = argparse.ArgumentParser(description=__doc__, allow_abbrev=False)
    parser.add_argument("--macro-json", type=Path, required=True, help="宏轨生成的 JSON 结果")
    parser.add_argument("--native-json", type=Path, required=True, help="原生轨生成的 JSON 结果")
    parser.add_argument("--macro-junit", type=Path, required=True, help="宏轨的 CTest JUnit 报告")
    parser.add_argument("--native-junit", type=Path, required=True, help="原生轨的 CTest JUnit 报告")
    args = parser.parse_args(argv)
    try:
        macro = readJson(args.macro_json, "宏轨")
        native = readJson(args.native_json, "原生轨")
        macroCases = validateDocument(macro, "macro")
        nativeCases = validateDocument(native, "native")
        macroTests = validateJunit(args.macro_junit, "宏轨")
        nativeTests = validateJunit(args.native_junit, "原生轨")
        for caseId in CASE_IDS:
            compareJson(macroCases[caseId], nativeCases[caseId], f"cases.{caseId}")
        for key in SCHEMA_KEYS:
            compareJson(macro["schemas"][key], native["schemas"][key], f"schemas.{key}", schema=True)
    except CheckError as error:
        print(f"双轨契约检查失败：{error}", file=sys.stderr)
        return 1
    except RecursionError:
        print("双轨契约检查失败：JSON 结构嵌套过深", file=sys.stderr)
        return 1
    print(f"双轨契约一致：{len(CASE_IDS)} 个固定用例、{len(SCHEMA_KEYS)} 份 Schema，"
          f"JUnit 宏轨 {macroTests} 项、原生 {nativeTests} 项。")
    return 0


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8", errors="backslashreplace")
    sys.stderr.reconfigure(encoding="utf-8", errors="backslashreplace")
    raise SystemExit(main())
